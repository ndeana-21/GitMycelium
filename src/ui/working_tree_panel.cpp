#include "ui/working_tree_panel.h"

#include "theme/icons.h"
#include "theme/theme.h"
#include "ui/grip_splitter.h"
#include "ui/themed_icons.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <functional>
#include <set>
#include <utility>

namespace gk {

/// Draws the "Stage File" / "Unstage File" button on the row under the pointer and
/// answers a click on it. A widget per row would not survive a model rebuild; a
/// delegate paints from the model and so cannot point at the wrong file.
class FileActionDelegate : public QStyledItemDelegate {
public:
    FileActionDelegate(QString text, bool danger, QObject* parent)
        : QStyledItemDelegate(parent), m_text(std::move(text)), m_danger(danger) {}

    std::function<void(const QString&)> onClicked;

    /// True once after a click landed on the button. The view emits `clicked` for
    /// that release all the same, and opening the file's diff is not what was asked.
    bool takeButtonClick() {
        return std::exchange(m_buttonClicked, false);
    }

    QRect buttonRect(const QStyleOptionViewItem& option) const {
        const QFontMetrics metrics(option.font);
        const int width = metrics.horizontalAdvance(m_text) + 16;
        const int height = std::min(option.rect.height() - 4, metrics.height() + 6);
        return QRect(option.rect.right() - width - 4,
                     option.rect.top() + (option.rect.height() - height) / 2, width, height);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        if (!(option.state & QStyle::State_MouseOver) || !CommitFilesModel::isFile(index)) {
            return;
        }
        const QRect rect = buttonRect(option);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(m_danger ? theme::danger() : theme::success());
        painter->drawRoundedRect(rect, theme::radiusSmall(), theme::radiusSmall());
        QFont font = option.font;
        font.setBold(true);
        painter->setFont(font);
        painter->setPen(theme::onAccent());
        painter->drawText(rect, Qt::AlignCenter, m_text);
        painter->restore();
    }

    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                     const QModelIndex& index) override {
        const bool press = event->type() == QEvent::MouseButtonPress ||
                           event->type() == QEvent::MouseButtonDblClick;
        const bool release = event->type() == QEvent::MouseButtonRelease;
        if ((press || release) && CommitFilesModel::isFile(index)) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (buttonRect(option).contains(mouse->position().toPoint())) {
                m_buttonClicked = release;
                if (release && onClicked) {
                    // After the event: the action refreshes the model this click is
                    // still being delivered through.
                    const QString path = CommitFilesModel::pathOf(index);
                    QTimer::singleShot(0, this, [this, path] { onClicked(path); });
                }
                return true;
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

private:
    QString m_text;
    bool m_danger = false;
    bool m_buttonClicked = false;
};

namespace {

QString filesText(int count) {
    return count == 1 ? QStringLiteral("1 File") : QStringLiteral("%1 Files").arg(count);
}

QFrame* messageBox(QWidget* parent, QLineEdit** summary, QPlainTextEdit** description,
                   const QString& summaryHint) {
    auto* box = new QFrame(parent);
    box->setProperty("role", QStringLiteral("messageBox"));
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    *summary = new QLineEdit(box);
    (*summary)->setPlaceholderText(summaryHint);
    *description = new QPlainTextEdit(box);
    (*description)->setPlaceholderText(QStringLiteral("Description"));
    (*description)->setTabChangesFocus(true);
    (*description)->setMinimumHeight(36);
    layout->addWidget(*summary);
    layout->addWidget(*description, 1);
    return box;
}

/// The summary and the stash title are half again the description's size: they are the
/// line that is read, but twice read as a heading over a body instead of as the first
/// line of one message.
void styleMessageFields(QLineEdit* summary, QPlainTextEdit* description) {
    const QString family = QApplication::font().family();
    summary->setStyleSheet(QStringLiteral("font-size: %1px; padding: 4px 8px;")
                               .arg(qRound(theme::baseFontPixelSize() * 1.5)));
    description->setStyleSheet(QStringLiteral("font-family: \"%1\"; font-size: %2px;")
                                   .arg(family)
                                   .arg(theme::baseFontPixelSize()));
}

QString joinMessage(const QString& summary, const QString& description) {
    const QString body = description.trimmed().isEmpty() ? QString() : description;
    if (body.isEmpty()) {
        return summary;
    }
    return summary.isEmpty() ? body : summary + QStringLiteral("\n\n") + body;
}

} // namespace

WorkingTreePanel::WorkingTreePanel(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground);
    buildUi();
}

void WorkingTreePanel::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(buildHeader());
    outer->addWidget(buildFileToolbar());

    auto* split = new GripSplitter(Qt::Vertical, this);
    split->setObjectName(QStringLiteral("commitPanelSplit"));
    split->addWidget(buildFiles());
    split->addWidget(buildForms());
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    split->setSizes({420, 300});
    outer->addWidget(split, 1);

    refreshTheme();
    refreshCounts();
}

QWidget* WorkingTreePanel::buildHeader() {
    auto* header = new QWidget(this);
    auto* row = new QHBoxLayout(header);
    row->setContentsMargins(8, 8, 8, 4);
    row->setSpacing(6);

    m_discardAll = new QPushButton(header);
    m_discardAll->setObjectName(QStringLiteral("discardAll"));
    m_discardAll->setProperty("variant", QStringLiteral("dangerSolid"));
    m_discardAll->setProperty("buttonSize", QStringLiteral("small"));
    m_discardAll->setToolTip(QStringLiteral("Discard all changes"));
    m_discardAll->setAccessibleName(QStringLiteral("Discard all changes"));
    setThemedIcon(m_discardAll, Icon::Trash, IconTint::OnAccent);
    connect(m_discardAll, &QPushButton::clicked, this, &WorkingTreePanel::discardAllRequested);

    m_countLabel = new QLabel(header);
    m_countLabel->setObjectName(QStringLiteral("changeCount"));
    m_countLabel->setMinimumWidth(1);
    m_branchPill = new QLabel(header);
    m_branchPill->setObjectName(QStringLiteral("branchPill"));
    m_branchPill->setMinimumWidth(1);

    // The count and the branch sit in the middle of the panel, not of the space left
    // beside the button: an invisible twin of the button on the right balances it.
    auto* balance = new QWidget(header);
    balance->setFixedWidth(m_discardAll->sizeHint().width());

    row->addWidget(m_discardAll);
    row->addStretch(1);
    row->addWidget(m_countLabel);
    row->addWidget(m_branchPill);
    row->addStretch(1);
    row->addWidget(balance);
    return header;
}

QWidget* WorkingTreePanel::buildFileToolbar() {
    auto* bar = new QWidget(this);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(8, 2, 8, 4);
    row->setSpacing(0);

    m_sort = new QToolButton(bar);
    m_sort->setObjectName(QStringLiteral("sortOrder"));
    connect(m_sort, &QToolButton::clicked, this, [this] {
        m_descending = !m_descending;
        applySort();
    });

    m_pathMode = new QToolButton(bar);
    m_pathMode->setText(QStringLiteral("Path"));
    m_pathMode->setCheckable(true);
    m_pathMode->setChecked(true);
    m_treeMode = new QToolButton(bar);
    m_treeMode->setText(QStringLiteral("Tree"));
    m_treeMode->setCheckable(true);
    auto* modes = new QButtonGroup(bar);
    modes->setExclusive(true);
    modes->addButton(m_pathMode);
    modes->addButton(m_treeMode);
    connect(m_pathMode, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            setMode(CommitFilesModel::PathMode);
        }
    });
    connect(m_treeMode, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            setMode(CommitFilesModel::TreeMode);
        }
    });

    auto* balance = new QWidget(bar);
    balance->setFixedWidth(m_sort->sizeHint().width());

    row->addWidget(m_sort);
    row->addStretch(1);
    row->addWidget(m_pathMode);
    row->addWidget(m_treeMode);
    row->addStretch(1);
    row->addWidget(balance);
    return bar;
}

QWidget* WorkingTreePanel::buildFiles() {
    auto* area = new QWidget(this);
    auto* layout = new QVBoxLayout(area);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    buildSection(m_unstaged, area, false);
    buildSection(m_staged, area, true);
    for (Section* section : {&m_unstaged, &m_staged}) {
        auto* head = new QWidget(area);
        auto* row = new QHBoxLayout(head);
        row->setContentsMargins(4, 4, 8, 4);
        row->setSpacing(6);
        row->addWidget(section->toggle, 1);
        row->addWidget(section->all);
        layout->addWidget(head);
        layout->addWidget(section->view, 1);
        wireSection(*section);
    }
    // Takes the room only when both lists are folded, so the headers stay on top.
    layout->addStretch(0);
    return area;
}

void WorkingTreePanel::buildSection(Section& section, QWidget* parent, bool staged) {
    section.staged = staged;
    section.toggle = new QToolButton(parent);
    section.toggle->setObjectName(staged ? QStringLiteral("stagedToggle")
                                         : QStringLiteral("unstagedToggle"));
    section.toggle->setCheckable(true);
    section.toggle->setChecked(true);
    section.toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    section.toggle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    section.toggle->setProperty("role", QStringLiteral("sectionToggle"));

    section.all = new QPushButton(staged ? QStringLiteral("Unstage All Changes")
                                         : QStringLiteral("Stage All Changes"),
                                  parent);
    section.all->setObjectName(staged ? QStringLiteral("unstageAll") : QStringLiteral("stageAll"));
    section.all->setProperty("variant", staged ? QStringLiteral("dangerSolid")
                                               : QStringLiteral("success"));
    section.all->setProperty("buttonSize", QStringLiteral("small"));

    section.model = new CommitFilesModel(this);
    section.view = new QTreeView(parent);
    section.view->setObjectName(staged ? QStringLiteral("stagedFiles")
                                       : QStringLiteral("unstagedFiles"));
    section.view->setModel(section.model);
    section.view->setHeaderHidden(true);
    section.view->setUniformRowHeights(true);
    section.view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    section.view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    section.view->setSelectionBehavior(QAbstractItemView::SelectRows);
    section.view->setExpandsOnDoubleClick(false);
    section.view->setContextMenuPolicy(Qt::CustomContextMenu);
    section.view->setMouseTracking(true);
    section.view->setMinimumHeight(0);
    section.view->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    section.action = new FileActionDelegate(
        staged ? QStringLiteral("Unstage File") : QStringLiteral("Stage File"), staged,
        section.view);
    section.view->setItemDelegate(section.action);
}

void WorkingTreePanel::wireSection(Section& section) {
    Section* self = &section;
    section.action->onClicked = [this, self](const QString& path) {
        if (self->staged) {
            emit unstageRequested({path});
        } else {
            emit stageRequested({path});
        }
    };
    connect(section.toggle, &QToolButton::toggled, this, [this, self] {
        refreshSectionState(*self);
    });
    connect(section.all, &QPushButton::clicked, this, [this, self] {
        if (self->staged) {
            emit unstageAllRequested();
        } else {
            emit stageAllRequested();
        }
    });
    connect(section.view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this, self](const QModelIndex& index) {
                if (!CommitFilesModel::isFile(index)) {
                    return;
                }
                // One file is "the file": picking one here lets go of the other list's.
                Section& twin = other(*self);
                twin.view->selectionModel()->clear();
                emit fileSelected(CommitFilesModel::pathOf(index), self->staged);
            });
    connect(section.view, &QTreeView::clicked, this, [this, self](const QModelIndex& index) {
        if (self->action->takeButtonClick()) {
            return;
        }
        if (CommitFilesModel::isFile(index)) {
            emit fileActivated(CommitFilesModel::pathOf(index), self->staged);
        } else {
            self->view->setExpanded(index, !self->view->isExpanded(index));
        }
    });
    connect(section.view, &QTreeView::customContextMenuRequested, this,
            [this, self](const QPoint& pos) { showContextMenu(*self, pos); });
}

void WorkingTreePanel::showContextMenu(Section& section, const QPoint& pos) {
    const QModelIndex index = section.view->indexAt(pos);
    if (!CommitFilesModel::isFile(index)) {
        return;
    }
    if (!section.view->selectionModel()->isSelected(index)) {
        section.view->setCurrentIndex(index);
    }
    const QStringList paths = selectedPathsIn(section);
    QMenu menu(this);
    QAction* move = menu.addAction(section.staged ? QStringLiteral("Unstage")
                                                  : QStringLiteral("Stage"));
    QAction* discard = nullptr;
    if (!section.staged) {
        discard = menu.addAction(QStringLiteral("Discard Changes…"));
    }
    menu.addSeparator();
    QAction* stash = menu.addAction(QStringLiteral("Stash Selected Files…"));

    // Ignoring is a choice of scope and of what to ignore, and both are the
    // reader's: a repository's `.gitignore` is shared, `info/exclude` is not.
    QMenu* ignore = menu.addMenu(QStringLiteral("Ignore…"));
    const auto addScope = [&](const QString& title, const QString& scope) {
        QMenu* scopeMenu = ignore->addMenu(title);
        scopeMenu->addAction(QStringLiteral("This file"))
            ->setData(QStringList{scope, QStringLiteral("file")});
        const QString suffix = QFileInfo(paths.value(0)).suffix();
        if (!suffix.isEmpty()) {
            scopeMenu->addAction(QStringLiteral("All *.%1 files").arg(suffix))
                ->setData(QStringList{scope, QStringLiteral("ext")});
        }
        scopeMenu->addAction(QStringLiteral("This folder"))
            ->setData(QStringList{scope, QStringLiteral("dir")});
    };
    addScope(QStringLiteral("In .gitignore"), QStringLiteral("gitignore"));
    addScope(QStringLiteral("In .git/info/exclude"), QStringLiteral("exclude"));

    QAction* chosen = menu.exec(section.view->viewport()->mapToGlobal(pos));
    if (!chosen) {
        return;
    }
    if (chosen == move) {
        if (section.staged) {
            emit unstageRequested(paths);
        } else {
            emit stageRequested(paths);
        }
    } else if (chosen == discard) {
        emit discardRequested(paths);
    } else if (chosen == stash) {
        emit stashRequested(paths);
    } else {
        const QStringList info = chosen->data().toStringList();
        if (info.size() == 2) {
            emit ignoreRequested(paths, info.at(0), info.at(1));
        }
    }
}

QWidget* WorkingTreePanel::buildForms() {
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("commitTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(buildCommitForm(), QStringLiteral("Commit"));
    m_tabs->addTab(buildStashForm(), QStringLiteral("Stash"));
    return m_tabs;
}

QWidget* WorkingTreePanel::buildCommitForm() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    m_amend = new QCheckBox(QStringLiteral("Amend previous commit"), page);
    m_amend->setObjectName(QStringLiteral("amendCommit"));
    connect(m_amend, &QCheckBox::toggled, this, [this](bool on) {
        if (on) {
            m_draftBeforeAmend = commitMessage();
        } else {
            setCommitMessage(m_draftBeforeAmend);
            m_draftBeforeAmend.clear();
        }
        refreshCommitButton();
        emit amendToggled(on);
    });
    layout->addWidget(m_amend);

    layout->addWidget(messageBox(page, &m_summary, &m_description,
                                 QStringLiteral("Commit summary")),
                      1);
    m_summary->setObjectName(QStringLiteral("commitSummary"));
    m_description->setObjectName(QStringLiteral("commitDescription"));
    connect(m_summary, &QLineEdit::textChanged, this, &WorkingTreePanel::refreshCommitButton);
    connect(m_description, &QPlainTextEdit::textChanged, this,
            &WorkingTreePanel::refreshCommitButton);

    // The options are folded away and drawn faint: they are set once and forgotten,
    // and the form should read as summary, description, button.
    m_optionsToggle = new QToolButton(page);
    m_optionsToggle->setObjectName(QStringLiteral("commitOptions"));
    m_optionsToggle->setProperty("role", QStringLiteral("faint"));
    m_optionsToggle->setText(QStringLiteral("Commit options"));
    m_optionsToggle->setCheckable(true);
    m_optionsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setThemedIcon(m_optionsToggle, Icon::ChevronRight, IconTint::Dim);
    layout->addWidget(m_optionsToggle, 0, Qt::AlignLeft);

    m_options = new QWidget(page);
    auto* options = new QVBoxLayout(m_options);
    options->setContentsMargins(18, 0, 0, 0);
    options->setSpacing(4);
    m_pushAfter = new QCheckBox(QStringLiteral("Push after committing"), m_options);
    m_pushAfter->setObjectName(QStringLiteral("pushAfterCommit"));
    m_signCommit = new QCheckBox(QStringLiteral("Sign commit"), m_options);
    m_signCommit->setObjectName(QStringLiteral("signCommit"));
    m_signCommit->setToolTip(
        QStringLiteral("Sign this commit with your GPG or SSH key (writes commit.gpgsign)"));
    m_coauthors = new QPushButton(QStringLiteral("Co-authors…"), m_options);
    m_coauthors->setProperty("buttonSize", QStringLiteral("small"));
    m_coauthors->setToolTip(
        QStringLiteral("Add a Co-authored-by trailer to the message before committing"));
    options->addWidget(m_pushAfter);
    options->addWidget(m_signCommit);
    options->addWidget(m_coauthors, 0, Qt::AlignLeft);
    m_options->setVisible(false);
    layout->addWidget(m_options);
    connect(m_optionsToggle, &QToolButton::toggled, this, [this](bool open) {
        m_options->setVisible(open);
        setThemedIcon(m_optionsToggle, open ? Icon::ChevronDown : Icon::ChevronRight,
                      IconTint::Dim);
    });
    connect(m_signCommit, &QCheckBox::toggled, this, &WorkingTreePanel::signChanged);
    connect(m_coauthors, &QPushButton::clicked, this, &WorkingTreePanel::coauthorsRequested);

    m_commit = new QPushButton(page);
    m_commit->setObjectName(QStringLiteral("commitButton"));
    m_commit->setProperty("variant", QStringLiteral("success"));
    m_commit->setProperty("buttonSize", QStringLiteral("large"));
    m_commit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    layout->addWidget(m_commit);

    const auto commit = [this] {
        if (m_commit->isEnabled()) {
            emit commitRequested(commitMessage());
        }
    };
    connect(m_commit, &QPushButton::clicked, this, commit);
    // Ctrl+Enter commits from either half of the message, the way a reader who is
    // done typing tries to finish.
    for (QWidget* field : {static_cast<QWidget*>(m_summary), static_cast<QWidget*>(m_description)}) {
        auto* shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), field);
        shortcut->setContext(Qt::WidgetShortcut);
        connect(shortcut, &QShortcut::activated, this, commit);
    }
    return page;
}

QWidget* WorkingTreePanel::buildStashForm() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addWidget(messageBox(page, &m_stashTitle, &m_stashDescription,
                                 QStringLiteral("Stash title")),
                      1);
    m_stashTitle->setObjectName(QStringLiteral("stashTitle"));
    m_stashDescription->setObjectName(QStringLiteral("stashDescription"));
    connect(m_stashTitle, &QLineEdit::textChanged, this, &WorkingTreePanel::refreshStashButton);
    connect(m_stashDescription, &QPlainTextEdit::textChanged, this,
            &WorkingTreePanel::refreshStashButton);

    m_stash = new QPushButton(page);
    m_stash->setObjectName(QStringLiteral("stashButton"));
    m_stash->setProperty("variant", QStringLiteral("success"));
    m_stash->setProperty("buttonSize", QStringLiteral("large"));
    m_stash->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    layout->addWidget(m_stash);
    connect(m_stash, &QPushButton::clicked, this, [this] {
        if (m_stash->isEnabled()) {
            emit stashAllRequested(stashMessage());
        }
    });
    return page;
}

void WorkingTreePanel::setChanges(const std::vector<FileChange>& changes) {
    m_unstaged.model->setWorkingTreeSide(changes, false);
    m_staged.model->setWorkingTreeSide(changes, true);
    std::set<std::string> paths;
    for (const FileChange& change : changes) {
        paths.insert(change.path);
    }
    m_changedPaths = static_cast<int>(paths.size());
    for (Section* section : {&m_unstaged, &m_staged}) {
        section->view->expandAll();
    }
    refreshCounts();
    selectFirstFile();
}

void WorkingTreePanel::setBranch(const QString& name, const QColor& color) {
    m_branchName = name;
    m_branchColor = color.isValid() ? color : theme::accent();
    const QColor branch = m_branchColor;
    m_branchPill->setText(name.isEmpty() ? QStringLiteral("detached HEAD") : name);
    m_branchPill->setStyleSheet(
        QStringLiteral("background: %1; border: 1px solid %2; color: %3; border-radius: %4px;"
                       "padding: 1px 8px; font-weight: 600;")
            .arg(theme::branchLabelFill(branch).name(QColor::HexArgb),
                 theme::branchLabelBorder(branch).name(QColor::HexArgb),
                 theme::branchLabelText(branch).name())
            .arg(theme::radiusSmall()));
}

void WorkingTreePanel::setMode(CommitFilesModel::Mode mode) {
    {
        const QSignalBlocker pathBlock(m_pathMode);
        const QSignalBlocker treeBlock(m_treeMode);
        (mode == CommitFilesModel::TreeMode ? m_treeMode : m_pathMode)->setChecked(true);
    }
    for (Section* section : {&m_unstaged, &m_staged}) {
        section->model->setMode(mode);
        section->view->setRootIsDecorated(mode == CommitFilesModel::TreeMode);
        section->view->expandAll();
    }
}

void WorkingTreePanel::applySort() {
    setThemedIcon(m_sort, m_descending ? Icon::SortDescending : Icon::SortAscending);
    m_sort->setToolTip(m_descending ? QStringLiteral("Sorted Z → A; click for A → Z")
                                    : QStringLiteral("Sorted A → Z; click for Z → A"));
    for (Section* section : {&m_unstaged, &m_staged}) {
        section->model->setSort(m_descending ? CommitFilesModel::PathDescending
                                             : CommitFilesModel::PathAscending);
        section->view->expandAll();
    }
}

void WorkingTreePanel::refreshCounts() {
    const int files = m_changedPaths;
    m_countLabel->setText(files == 1 ? QStringLiteral("1 file change on")
                                     : QStringLiteral("%1 file changes on").arg(files));
    m_discardAll->setEnabled(files > 0);
    refreshSectionState(m_unstaged);
    refreshSectionState(m_staged);
    refreshCommitButton();
    refreshStashButton();
}

void WorkingTreePanel::refreshSectionState(Section& section) {
    const int count = section.model->fileCount();
    const bool open = section.toggle->isChecked();
    section.toggle->setText(QStringLiteral("%1 Files (%2)")
                                .arg(section.staged ? QStringLiteral("Staged")
                                                    : QStringLiteral("Unstaged"))
                                .arg(count));
    setThemedIcon(section.toggle, open ? Icon::ChevronDown : Icon::ChevronRight, IconTint::Dim);
    section.view->setVisible(open);
    section.all->setEnabled(count > 0);
}

void WorkingTreePanel::refreshCommitButton() {
    const bool hasMessage = !m_summary->text().trimmed().isEmpty();
    const int staged = stagedCount();
    QString text;
    bool enabled = false;
    if (!hasMessage) {
        text = QStringLiteral("Type a Message to Commit");
    } else if (amend()) {
        text = QStringLiteral("Amend Previous Commit");
        enabled = true;
    } else if (staged == 0) {
        text = QStringLiteral("Stage Changes to Commit");
    } else {
        text = QStringLiteral("Commit Changes to %1").arg(filesText(staged));
        enabled = true;
    }
    m_commit->setText(text);
    m_commit->setEnabled(enabled);
}

void WorkingTreePanel::refreshStashButton() {
    const bool hasMessage = !m_stashTitle->text().trimmed().isEmpty();
    const int files = changedCount();
    if (!hasMessage) {
        m_stash->setText(QStringLiteral("Type a Message to Stash"));
        m_stash->setEnabled(false);
    } else if (files == 0) {
        m_stash->setText(QStringLiteral("Nothing to Stash"));
        m_stash->setEnabled(false);
    } else {
        m_stash->setText(QStringLiteral("Stash Changes to %1").arg(filesText(files)));
        m_stash->setEnabled(true);
    }
}

void WorkingTreePanel::refreshTheme() {
    styleMessageFields(m_summary, m_description);
    styleMessageFields(m_stashTitle, m_stashDescription);
    QFont large = m_commit->font();
    large.setPixelSize(theme::baseFontPixelSize() + 1);
    m_commit->setFont(large);
    m_stash->setFont(large);
    applySort();
    if (!m_branchPill->text().isEmpty()) {
        setBranch(m_branchName, m_branchColor);
    }
}

QStringList WorkingTreePanel::selectedPathsIn(const Section& section) const {
    QStringList out;
    for (const QModelIndex& index : section.view->selectionModel()->selectedIndexes()) {
        const QString path = CommitFilesModel::pathOf(index);
        if (!path.isEmpty() && !out.contains(path)) {
            out.append(path);
        }
    }
    return out;
}

QStringList WorkingTreePanel::selectedPaths() const {
    QStringList out = selectedPathsIn(m_unstaged);
    for (const QString& path : selectedPathsIn(m_staged)) {
        if (!out.contains(path)) {
            out.append(path);
        }
    }
    return out;
}

QModelIndex WorkingTreePanel::firstFileIn(const Section& section) const {
    const CommitFilesModel* model = section.model;
    QModelIndex index = model->index(0, 0);
    while (index.isValid() && !CommitFilesModel::isFile(index)) {
        if (model->rowCount(index) > 0) {
            index = model->index(0, 0, index);
            continue;
        }
        index = model->index(index.row() + 1, 0, index.parent());
    }
    return index;
}

QString WorkingTreePanel::firstFilePath() const {
    const QModelIndex unstaged = firstFileIn(m_unstaged);
    return CommitFilesModel::pathOf(unstaged.isValid() ? unstaged : firstFileIn(m_staged));
}

bool WorkingTreePanel::firstFileIsStaged() const {
    return !firstFileIn(m_unstaged).isValid() && firstFileIn(m_staged).isValid();
}

void WorkingTreePanel::selectFirstFile() {
    for (Section* section : {&m_unstaged, &m_staged}) {
        const QModelIndex index = firstFileIn(*section);
        if (index.isValid()) {
            section->view->setCurrentIndex(index);
            return;
        }
    }
}

bool WorkingTreePanel::selectPath(const QString& path, bool staged) {
    Section& section = staged ? m_staged : m_unstaged;
    const std::function<QModelIndex(const QModelIndex&)> find =
        [&](const QModelIndex& parent) -> QModelIndex {
        for (int row = 0; row < section.model->rowCount(parent); ++row) {
            const QModelIndex index = section.model->index(row, 0, parent);
            if (CommitFilesModel::pathOf(index) == path) {
                return index;
            }
            const QModelIndex deeper = find(index);
            if (deeper.isValid()) {
                return deeper;
            }
        }
        return {};
    };
    const QModelIndex index = find(QModelIndex());
    if (!index.isValid()) {
        return false;
    }
    section.view->setCurrentIndex(index);
    section.view->scrollTo(index);
    return true;
}

WorkingTreePanel::Section& WorkingTreePanel::other(const Section& section) {
    return section.staged ? m_unstaged : m_staged;
}

QString WorkingTreePanel::commitMessage() const {
    return joinMessage(m_summary->text(), m_description->toPlainText());
}

void WorkingTreePanel::setCommitMessage(const QString& message) {
    if (commitMessage() == message) {
        return;
    }
    const qsizetype newline = message.indexOf(QLatin1Char('\n'));
    QString summary = newline < 0 ? message : message.left(newline);
    QString description = newline < 0 ? QString() : message.mid(newline + 1);
    // The blank line between the two is the separator, not part of the description.
    if (description.startsWith(QLatin1Char('\n'))) {
        description.remove(0, 1);
    }
    m_summary->setText(summary);
    m_description->setPlainText(description);
    // At the end: the reader is going to keep typing, and a co-author insert that
    // jumped the caret to the top would make the next word land in the wrong place.
    m_description->moveCursor(QTextCursor::End);
}

void WorkingTreePanel::clearCommitMessage() {
    m_summary->clear();
    m_description->clear();
}

void WorkingTreePanel::focusCommitMessage() {
    m_tabs->setCurrentIndex(0);
    m_summary->setFocus();
    m_summary->end(false);
}

QString WorkingTreePanel::stashMessage() const {
    return joinMessage(m_stashTitle->text(), m_stashDescription->toPlainText());
}

void WorkingTreePanel::clearStashMessage() {
    m_stashTitle->clear();
    m_stashDescription->clear();
}

bool WorkingTreePanel::signCommit() const {
    return m_signCommit->isChecked();
}

void WorkingTreePanel::setSignCommit(bool on) {
    if (m_signCommit->isChecked() == on) {
        return;
    }
    // The window sets this from the config; that is not the reader toggling it.
    const QSignalBlocker block(m_signCommit);
    m_signCommit->setChecked(on);
}

bool WorkingTreePanel::amend() const {
    return m_amend->isChecked();
}

void WorkingTreePanel::resetAmend() {
    {
        const QSignalBlocker block(m_amend);
        m_amend->setChecked(false);
    }
    m_draftBeforeAmend.clear();
    refreshCommitButton();
}

bool WorkingTreePanel::pushAfterCommit() const {
    return m_pushAfter->isChecked();
}

int WorkingTreePanel::stagedCount() const {
    return m_staged.model->fileCount();
}

QStringList WorkingTreePanel::stagedPaths() const {
    QStringList out;
    const std::function<void(const QModelIndex&)> collect = [&](const QModelIndex& parent) {
        for (int row = 0; row < m_staged.model->rowCount(parent); ++row) {
            const QModelIndex index = m_staged.model->index(row, 0, parent);
            const QString path = CommitFilesModel::pathOf(index);
            if (!path.isEmpty()) {
                out.append(path);
            }
            collect(index);
        }
    };
    collect(QModelIndex());
    return out;
}

int WorkingTreePanel::unstagedCount() const {
    return m_unstaged.model->fileCount();
}

int WorkingTreePanel::changedCount() const {
    return m_changedPaths;
}

} // namespace gk
