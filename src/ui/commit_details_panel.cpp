#include "ui/commit_details_panel.h"

#include "model/commit_files_model.h"
#include "model/date_format.h"
#include "theme/glyphs.h"
#include "theme/icons.h"
#include "theme/theme.h"
#include "ui/grip_splitter.h"
#include "ui/settings_store.h"
#include "ui/themed_icons.h"
#include "ui/working_tree_panel.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDateTime>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTextCursor>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <functional>

namespace gk {
namespace {

constexpr int kAvatarSideBase = 38;

int avatarSide() {
    return theme::scaledPixels(kAvatarSideBase);
}

/// Initials of a signature, at most two letters: an avatar is a shape to
/// recognise, and a circle of three letters is a smear.
QString initialsFor(const std::string& name) {
    const QString full = QString::fromStdString(name).trimmed();
    if (full.isEmpty()) {
        return QStringLiteral("?");
    }
    QString initials;
    for (const QString& part : full.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        initials += part.left(1).toUpper();
        if (initials.size() == 2) {
            break;
        }
    }
    return initials.isEmpty() ? QStringLiteral("?") : initials;
}

QString messageText(const CommitInfo& commit) {
    QString text = QString::fromStdString(commit.summary);
    if (!commit.body.empty()) {
        text += QLatin1Char('\n') + QString::fromStdString(commit.body);
    }
    return text;
}

/// The author's own clock, not ours: a commit made in another timezone must not
/// look like it was made at a time they never saw. The chosen format applies here
/// too, so a date cannot read one way in the list and another in the panel.
QString formatWhen(const Signature& who) {
    const QDateTime when = QDateTime::fromSecsSinceEpoch(who.whenUnix).toOffsetFromUtc(
        who.offsetMinutes * 60);
    const CommitDateFormat format = commitDateFormatFromKey(
        SettingsStore().stringValue(SettingsStore::globalKey(QStringLiteral("commit-date-format")),
                                    QStringLiteral("iso")));
    return formatCommitDate(when, format);
}

QPixmap avatarFor(const Signature& who) {
    // No network here, so no gravatar: a disc with the initials, in the same
    // author colour the graph's node carries.
    const QColor fill = theme::authorColor(
        QString::fromStdString(who.email.empty() ? who.name : who.email));

    const int side = avatarSide();
    QPixmap pixmap(QSize(side * 2, side * 2));
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawEllipse(QRectF(1.0, 1.0, side - 2.0, side - 2.0));

    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(theme::baseFontPixelSize() + 2);
    painter.setFont(font);
    painter.setPen(theme::textOn(fill));
    painter.drawText(QRectF(0, 0, side, side), Qt::AlignCenter, initialsFor(who.name));
    painter.end();
    return pixmap;
}

constexpr int kIconSideBase = 14;

int statusIconSide() {
    return theme::scaledPixels(kIconSideBase);
}

/// One "1 modified" pair: the glyph that says how, and the count that says how
/// many. Built as widgets rather than rich text so the icon is the same glyph the
/// file rows use, not a second drawing of it.
QWidget* statusChip(Glyph glyph, const QColor& colour, int count, const QString& label) {
    auto* chip = new QWidget;
    auto* layout = new QHBoxLayout(chip);
    layout->setContentsMargins(0, 0, 12, 0);
    layout->setSpacing(4);

    auto* icon = new QLabel(chip);
    icon->setPixmap(glyphIcon(glyph, colour, statusIconSide()).pixmap(statusIconSide(), statusIconSide()));
    icon->setFixedWidth(statusIconSide());
    auto* text = new QLabel(QStringLiteral("%1 %2").arg(count).arg(label), chip);

    layout->addWidget(icon);
    layout->addWidget(text);
    return chip;
}

} // namespace

CommitDetailsPanel::CommitDetailsPanel(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground);
    buildUi();
}

void CommitDetailsPanel::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Two faces with one minimum size: a QStackedWidget is as small as its largest
    // page allows, so the side panel cannot be dragged narrower for a commit than
    // for the working tree, or the other way round.
    m_pages = new QStackedWidget(this);
    outer->addWidget(m_pages);

    // The description is one column whose two heights can be dragged: the message box
    // above the author block, and the whole of it above the file list. The handle the
    // reader looks for when a long message is cut off is the first one. Both are the
    // Commit Panel's grip, so the two faces of this stack cannot grow two different
    // drag handles.
    auto* split = new GripSplitter(Qt::Vertical, m_pages);
    split->setObjectName(QStringLiteral("commitDetailsSplit"));
    m_pages->addWidget(split);

    m_wip = new WorkingTreePanel(m_pages);
    m_pages->addWidget(m_wip);
    wireWorkingTree();

    // The message and the amend control that belongs to it, in a widget of its own so
    // that the handle below it changes this height and nothing else.
    m_messageArea = new QWidget(split);
    m_messageArea->setObjectName(QStringLiteral("commitMessageArea"));
    auto* messageLayout = new QVBoxLayout(m_messageArea);
    messageLayout->setContentsMargins(10, 8, 10, 0);
    messageLayout->setSpacing(6);

    m_message = new QPlainTextEdit(m_messageArea);
    m_message->setObjectName(QStringLiteral("commitMessage"));
    m_message->setPlaceholderText(QStringLiteral("Commit message"));
    m_message->setTabChangesFocus(true);
    m_message->setMinimumHeight(48);
    messageLayout->addWidget(m_message, 1);

    auto* messageButtons = new QHBoxLayout;
    messageButtons->setContentsMargins(0, 0, 0, 0);
    messageButtons->addStretch(1);
    m_amend = new QPushButton(QStringLiteral("Amend message"), m_messageArea);
    setThemedIcon(m_amend, Icon::FileModified);
    m_amend->setVisible(false);
    messageButtons->addWidget(m_amend);
    messageLayout->addLayout(messageButtons);

    // Everything below the grip, in one widget: who wrote it, how much changed, the
    // controls for the file list, and the files themselves. The page has exactly one
    // handle — the one between the message and the author — so the author block and
    // the list share whatever height is left under the message.
    m_bodyArea = new QWidget(split);
    m_metaArea = new QWidget(m_bodyArea);
    m_metaArea->setObjectName(QStringLiteral("commitMetaArea"));
    auto* top = m_metaArea;
    auto* topLayout = new QVBoxLayout(top);
    topLayout->setContentsMargins(10, 6, 10, 6);
    topLayout->setSpacing(6);

    auto* body = topLayout;
    auto* bottom = top;

    // Author row: avatar, name, and when it was authored; the parent on the far
    // right, where it does not compete with the name.
    auto* authorRow = new QHBoxLayout;
    authorRow->setContentsMargins(0, 0, 0, 0);
    authorRow->setSpacing(8);
    m_avatar = new QLabel(bottom);
    m_avatar->setFixedSize(avatarSide(), avatarSide());
    authorRow->addWidget(m_avatar, 0, Qt::AlignTop);

    auto* who = new QVBoxLayout;
    who->setContentsMargins(0, 0, 0, 0);
    who->setSpacing(1);
    m_author = new QLabel(bottom);
    QFont authorFont = m_author->font();
    authorFont.setBold(true);
    m_author->setFont(authorFont);
    m_when = new QLabel(bottom);
    m_when->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
    m_signature = new QLabel(bottom);
    m_signature->setTextFormat(Qt::RichText);
    m_signature->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
    who->addWidget(m_author);
    who->addWidget(m_when);
    who->addWidget(m_signature);
    authorRow->addLayout(who, 1);

    m_parent = new QLabel(bottom);
    m_parent->setTextFormat(Qt::RichText);
    m_parent->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
    authorRow->addWidget(m_parent, 0, Qt::AlignTop);
    body->addLayout(authorRow);

    m_summary = new QLabel(bottom);
    body->addWidget(m_summary);

    auto* toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(6);

    // The sort is a button with an icon, not a dropdown: one click moves to the next
    // order and the tooltip says which one is showing. A menu listed three entries for
    // one piece of state the reader cannot see, and it looked nothing like the Commit
    // Panel's own sort control sitting a page away in the same panel.
    m_sort = new QToolButton(bottom);
    m_sort->setObjectName(QStringLiteral("commitSort"));
    m_sort->setAutoRaise(true);
    connect(m_sort, &QToolButton::clicked, this, &CommitDetailsPanel::cycleSort);
    toolbar->addWidget(m_sort);

    m_pathMode = new QToolButton(bottom);
    m_pathMode->setText(QStringLiteral("Path"));
    m_pathMode->setCheckable(true);
    m_pathMode->setChecked(true);
    m_treeMode = new QToolButton(bottom);
    m_treeMode->setText(QStringLiteral("Tree"));
    m_treeMode->setCheckable(true);
    auto* modes = new QButtonGroup(bottom);
    modes->setExclusive(true);
    modes->addButton(m_pathMode);
    modes->addButton(m_treeMode);
    toolbar->addWidget(m_pathMode);
    toolbar->addWidget(m_treeMode);

    m_viewAll = new QCheckBox(QStringLiteral("View all files"), bottom);
    m_viewAll->setToolTip(QStringLiteral(
        "Show every file of the commit even while a folder is selected in the tree"));
    toolbar->addStretch(1);
    toolbar->addWidget(m_viewAll);
    body->addLayout(toolbar);

    auto* listArea = new QWidget(m_bodyArea);
    auto* listLayout = new QVBoxLayout(listArea);
    listLayout->setContentsMargins(10, 6, 10, 8);
    listLayout->setSpacing(6);

    m_files = new CommitFilesModel(this);
    m_fileView = new QTreeView(listArea);
    m_fileView->setObjectName(QStringLiteral("commitFiles"));
    m_fileView->setModel(m_files);
    m_fileView->setHeaderHidden(true);
    m_fileView->setUniformRowHeights(true);
    m_fileView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_fileView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_fileView->setExpandsOnDoubleClick(false);
    m_fileView->setRootIsDecorated(true);
    m_fileView->setContextMenuPolicy(Qt::CustomContextMenu);
    listLayout->addWidget(m_fileView, 1);

    auto* bodyLayout = new QVBoxLayout(m_bodyArea);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(m_metaArea);
    bodyLayout->addWidget(listArea, 1);

    split->addWidget(m_messageArea);
    split->addWidget(m_bodyArea);
    // The message keeps the height the reader dragged it to, and everything under it —
    // author block, controls and files — splits what is left.
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({120, 430});

    connect(m_message, &QPlainTextEdit::textChanged, this, &CommitDetailsPanel::refreshMessageState);
    connect(m_amend, &QPushButton::clicked, this, [this] {
        emit messageAmendRequested(m_message->toPlainText());
    });
    // Ctrl+Enter saves, Escape puts the original back: the two things anyone tries
    // in a text field they did not mean to be editing.
    auto* save = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), m_message);
    connect(save, &QShortcut::activated, this, [this] {
        if (m_amend->isVisible()) {
            emit messageAmendRequested(m_message->toPlainText());
        }
    });
    auto* revert = new QShortcut(QKeySequence(Qt::Key_Escape), m_message);
    connect(revert, &QShortcut::activated, this, [this] { m_message->setPlainText(m_originalMessage); });

    connect(m_pathMode, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            m_files->setMode(CommitFilesModel::PathMode);
            m_fileView->expandAll();
        }
    });
    connect(m_treeMode, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            m_files->setMode(CommitFilesModel::TreeMode);
        }
    });
    connect(m_viewAll, &QCheckBox::toggled, this, [this] { onFileSelectionChanged(); });
    connect(m_fileView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& index) {
                onFileSelectionChanged();
                if (CommitFilesModel::isFile(index)) {
                    emit fileSelected(CommitFilesModel::pathOf(index),
                                      CommitFilesModel::isStaged(index));
                }
            });
    connect(m_fileView, &QTreeView::clicked, this, [this](const QModelIndex& index) {
        if (CommitFilesModel::isFile(index)) {
            emit fileActivated(CommitFilesModel::pathOf(index), CommitFilesModel::isStaged(index));
        } else {
            m_fileView->setExpanded(index, !m_fileView->isExpanded(index));
        }
    });
    connect(m_parent, &QLabel::linkActivated, this,
            [this](const QString& link) { emit commitActivated(link); });

    applySort();
}

void CommitDetailsPanel::cycleSort() {
    switch (m_sortOrder) {
    case CommitFilesModel::PathAscending:
        m_sortOrder = CommitFilesModel::PathDescending;
        break;
    case CommitFilesModel::PathDescending:
        m_sortOrder = CommitFilesModel::ByStatus;
        break;
    case CommitFilesModel::ByStatus:
        m_sortOrder = CommitFilesModel::PathAscending;
        break;
    }
    applySort();
}

void CommitDetailsPanel::applySort() {
    m_files->setSort(m_sortOrder);
    m_fileView->expandAll();
    // The button is the whole control, so the icon and the tooltip are the only place
    // the current order can be read. Grouping gets the folder glyph because that is
    // what it draws — one group row per kind of change.
    switch (m_sortOrder) {
    case CommitFilesModel::PathDescending:
        setThemedIcon(m_sort, Icon::SortDescending);
        m_sort->setToolTip(QStringLiteral("Sorted Z → A — click to group by status"));
        break;
    case CommitFilesModel::ByStatus:
        setThemedIcon(m_sort, Icon::Folder);
        m_sort->setToolTip(QStringLiteral("Grouped by status — click to sort A → Z"));
        break;
    case CommitFilesModel::PathAscending:
        setThemedIcon(m_sort, Icon::SortAscending);
        m_sort->setToolTip(QStringLiteral("Sorted A → Z — click to sort Z → A"));
        break;
    }
}

void CommitDetailsPanel::wireWorkingTree() {
    connect(m_wip, &WorkingTreePanel::fileSelected, this, &CommitDetailsPanel::fileSelected);
    connect(m_wip, &WorkingTreePanel::fileActivated, this, &CommitDetailsPanel::fileActivated);
    connect(m_wip, &WorkingTreePanel::stageRequested, this, &CommitDetailsPanel::stageRequested);
    connect(m_wip, &WorkingTreePanel::unstageRequested, this,
            &CommitDetailsPanel::unstageRequested);
    connect(m_wip, &WorkingTreePanel::discardRequested, this,
            &CommitDetailsPanel::discardRequested);
    connect(m_wip, &WorkingTreePanel::stashRequested, this, &CommitDetailsPanel::stashRequested);
    connect(m_wip, &WorkingTreePanel::ignoreRequested, this, &CommitDetailsPanel::ignoreRequested);
    connect(m_wip, &WorkingTreePanel::stageAllRequested, this,
            &CommitDetailsPanel::stageAllRequested);
    connect(m_wip, &WorkingTreePanel::unstageAllRequested, this,
            &CommitDetailsPanel::unstageAllRequested);
    connect(m_wip, &WorkingTreePanel::discardAllRequested, this,
            &CommitDetailsPanel::discardAllRequested);
    connect(m_wip, &WorkingTreePanel::commitRequested, this, &CommitDetailsPanel::commitRequested);
    connect(m_wip, &WorkingTreePanel::amendToggled, this, &CommitDetailsPanel::amendToggled);
    connect(m_wip, &WorkingTreePanel::stashAllRequested, this,
            &CommitDetailsPanel::stashAllRequested);
    connect(m_wip, &WorkingTreePanel::signChanged, this, &CommitDetailsPanel::signChanged);
    connect(m_wip, &WorkingTreePanel::coauthorsRequested, this,
            &CommitDetailsPanel::coauthorsRequested);
}

void CommitDetailsPanel::showCommit(const CommitInfo& commit, const std::vector<ChangedFile>& files,
                                    bool amendable) {
    m_workingTree = false;
    m_comparing = false;
    m_pages->setCurrentIndex(0);
    m_commitId = QString::fromStdString(commit.id);
    m_amendable = amendable;
    m_originalMessage = messageText(commit);

    m_message->setPlainText(m_originalMessage);
    // Older commits are shown, not edited. Saying why beats a greyed-out box with
    // no explanation.
    m_message->setReadOnly(!amendable);
    m_message->setToolTip(amendable
                              ? QStringLiteral("HEAD — Ctrl+Enter amends the message")
                              : QStringLiteral("Only the latest commit can be reworded: changing "
                                               "an older one means rewriting the history after it"));
    m_amend->setVisible(false);

    m_avatar->setPixmap(avatarFor(commit.author));
    m_lastAuthor = commit.author;
    m_author->setText(QString::fromStdString(commit.author.name));
    m_when->setText(QStringLiteral("authored %1").arg(formatWhen(commit.author)));

    if (commit.parents.empty()) {
        m_parent->clear();
    } else {
        const QString shortId = QString::fromStdString(commit.parents.front()).left(7);
        m_parent->setText(QStringLiteral("parent: <a href=\"%1\">%1</a>")
                              .arg(shortId.toHtmlEscaped()));
    }

    m_files->setFiles(files);
    refreshSummary();
    m_fileView->expandAll();

    // A folder filter from the previous commit would hide this one's files.
    m_files->setFolderFilter(QString());
    selectFirstFile();
}

void CommitDetailsPanel::showWorkingTree(const std::vector<FileChange>& changes) {
    m_workingTree = true;
    m_comparing = false;
    m_commitId.clear();
    m_signature->setVisible(false);
    m_pages->setCurrentIndex(1);
    m_wip->setChanges(changes);
}

void CommitDetailsPanel::setWorkingTreeBranch(const QString& name, const QColor& color) {
    m_wip->setBranch(name, color);
}

void CommitDetailsPanel::clear() {
    m_commitId.clear();
    m_comparing = false;
    m_originalMessage.clear();
    m_message->clear();
    m_files->setFiles({});
    refreshSummary();
}

void CommitDetailsPanel::refreshTheme() {
    // The avatar is the only thing here with a size of its own, and the two dim
    // labels carry a literal colour — everything else is styled by the global
    // stylesheet, which the window re-applies on the same call.
    m_wip->refreshTheme();
    m_avatar->setFixedSize(avatarSide(), avatarSide());
    if (!m_workingTree && !m_commitId.isEmpty()) {
        m_avatar->setPixmap(avatarFor(m_lastAuthor));
    }
    const QString dim = QStringLiteral("color: %1;").arg(theme::textDim().name());
    m_when->setStyleSheet(dim);
    m_parent->setStyleSheet(dim);
}

void CommitDetailsPanel::showComparison(const QString& base, const QString& target,
                                        const std::vector<ChangedFile>& files,
                                        const QString& label) {
    m_workingTree = false;
    m_comparing = true;
    m_commitId.clear(); // a comparison is not a commit, so nothing is amendable
    m_amendable = false;
    m_originalMessage.clear();

    m_pages->setCurrentIndex(0);
    m_signature->clear();
    m_signature->setVisible(false);
    m_message->clear();
    m_message->setReadOnly(true);
    m_amend->setVisible(false);
    const QString heading =
        label.isEmpty() ? QStringLiteral("%1..%2").arg(base, target) : label;
    m_message->setToolTip(QStringLiteral("Comparing %1").arg(heading));

    m_avatar->clear();
    m_author->setText(heading);
    m_when->clear();
    m_parent->clear();

    m_files->setFiles(files);
    refreshSummary();
    m_fileView->expandAll();
    m_files->setFolderFilter(QString());
    selectFirstFile();
}

QString CommitDetailsPanel::selectedPath() const {
    if (m_workingTree) {
        const QStringList paths = m_wip->selectedPaths();
        return paths.isEmpty() ? QString() : paths.front();
    }
    return m_files->pathOf(m_fileView->currentIndex());
}

QStringList CommitDetailsPanel::selectedPaths() const {
    if (m_workingTree) {
        return m_wip->selectedPaths();
    }
    QStringList out;
    if (!m_fileView || !m_fileView->selectionModel()) {
        return out;
    }
    const QModelIndexList selected = m_fileView->selectionModel()->selectedIndexes();
    for (const QModelIndex& index : selected) {
        if (!CommitFilesModel::isFile(index)) {
            continue;
        }
        const QString path = CommitFilesModel::pathOf(index);
        if (!path.isEmpty() && !out.contains(path)) {
            out.append(path);
        }
    }
    return out;
}

void CommitDetailsPanel::clearCommitMessage() {
    m_wip->clearCommitMessage();
    m_wip->resetAmend();
}

QString CommitDetailsPanel::commitMessage() const {
    return m_wip->commitMessage();
}

bool CommitDetailsPanel::signCommit() const {
    return m_wip->signCommit();
}

void CommitDetailsPanel::setSignCommit(bool on) {
    m_wip->setSignCommit(on);
}

void CommitDetailsPanel::setSignatureInfo(const CommitSignature& signature) {
    if (!m_signature) {
        return;
    }
    const QString status = QString::fromStdString(signature.status);
    const QString signer = QString::fromStdString(signature.signer);
    if (status.isEmpty() || status.startsWith(QLatin1Char('N'))) {
        m_signature->setText(QStringLiteral("Unsigned"));
        m_signature->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim().name()));
        m_signature->setVisible(true);
        return;
    }
    QString verdict;
    QColor colour = theme::text();
    switch (status.at(0).toLatin1()) {
    case 'G':
        verdict = QStringLiteral("good signature");
        colour = theme::success();
        break;
    case 'U':
        verdict = QStringLiteral("good signature, untrusted");
        colour = theme::warning();
        break;
    case 'X':
        verdict = QStringLiteral("expired signature");
        colour = theme::warning();
        break;
    case 'Y':
        verdict = QStringLiteral("expired key");
        colour = theme::warning();
        break;
    case 'R':
        verdict = QStringLiteral("revoked key");
        colour = theme::danger();
        break;
    case 'B':
        verdict = QStringLiteral("BAD signature");
        colour = theme::danger();
        break;
    case 'E':
        verdict = QStringLiteral("signature cannot be checked");
        colour = theme::warning();
        break;
    default:
        verdict = QStringLiteral("signed");
        break;
    }
    m_signature->setText(signer.isEmpty()
                             ? QStringLiteral("Signed · %1").arg(verdict)
                             : QStringLiteral("Signed by %1 · %2").arg(signer, verdict));
    m_signature->setStyleSheet(QStringLiteral("color: %1;").arg(colour.name()));
    m_signature->setVisible(true);
}

void CommitDetailsPanel::setCommitMessage(const QString& message) {
    m_wip->setCommitMessage(message);
}

void CommitDetailsPanel::focusCommitMessage() {
    m_wip->focusCommitMessage();
}


bool CommitDetailsPanel::amendRequested() const {
    return m_wip->amend();
}

bool CommitDetailsPanel::pushAfterCommit() const {
    return m_wip->pushAfterCommit();
}

void CommitDetailsPanel::clearStashMessage() {
    m_wip->clearStashMessage();
}

QStringList CommitDetailsPanel::stagedPaths() const {
    return m_wip->stagedPaths();
}

void CommitDetailsPanel::setMode(CommitFilesModel::Mode mode) {
    if (mode == CommitFilesModel::TreeMode) {
        m_treeMode->setChecked(true);
    } else {
        m_pathMode->setChecked(true);
    }
    m_wip->setMode(mode);
}

void CommitDetailsPanel::selectFirstFile() {
    if (m_workingTree) {
        m_wip->selectFirstFile();
        return;
    }
    // The first *file*: the first row of a tree is usually a folder.
    QModelIndex index = m_files->index(0, 0);
    while (index.isValid() && !CommitFilesModel::isFile(index)) {
        if (m_files->rowCount(index) > 0) {
            index = m_files->index(0, 0, index);
            continue;
        }
        index = m_files->index(index.row() + 1, 0, index.parent());
    }
    if (index.isValid()) {
        m_fileView->setCurrentIndex(index);
    }
}

QString CommitDetailsPanel::firstFilePath() const {
    if (m_workingTree) {
        return m_wip->firstFilePath();
    }
    QModelIndex index = m_files->index(0, 0);
    while (index.isValid() && !CommitFilesModel::isFile(index)) {
        if (m_files->rowCount(index) > 0) {
            index = m_files->index(0, 0, index);
            continue;
        }
        index = m_files->index(index.row() + 1, 0, index.parent());
    }
    return CommitFilesModel::pathOf(index);
}

bool CommitDetailsPanel::firstFileIsStaged() const {
    if (m_workingTree) {
        return m_wip->firstFileIsStaged();
    }
    QModelIndex index = m_files->index(0, 0);
    while (index.isValid() && !CommitFilesModel::isFile(index)) {
        if (m_files->rowCount(index) > 0) {
            index = m_files->index(0, 0, index);
            continue;
        }
        index = m_files->index(index.row() + 1, 0, index.parent());
    }
    return CommitFilesModel::isStaged(index);
}

bool CommitDetailsPanel::selectPath(const QString& path, bool staged) {
    if (m_workingTree) {
        return m_wip->selectPath(path, staged);
    }
    const std::function<QModelIndex(const QModelIndex&)> find =
        [&](const QModelIndex& parent) -> QModelIndex {
        for (int row = 0; row < m_files->rowCount(parent); ++row) {
            const QModelIndex index = m_files->index(row, 0, parent);
            if (CommitFilesModel::isFile(index) && CommitFilesModel::pathOf(index) == path &&
                CommitFilesModel::isStaged(index) == staged) {
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
    m_fileView->setCurrentIndex(index);
    m_fileView->scrollTo(index);
    return true;
}

void CommitDetailsPanel::refreshMessageState() {
    if (!m_amendable) {
        m_amend->setVisible(false);
        return;
    }
    m_amend->setVisible(m_message->toPlainText() != m_originalMessage);
}

void CommitDetailsPanel::refreshSummary() {
    auto* row = qobject_cast<QHBoxLayout*>(m_summary->layout());
    if (!row) {
        row = new QHBoxLayout(m_summary);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);
    }
    while (QLayoutItem* item = row->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const auto add = [&](Glyph glyph, const QColor& colour, int count, const QString& label) {
        if (count > 0) {
            row->addWidget(statusChip(glyph, colour, count, label));
        }
    };
    add(Glyph::Pencil, theme::modified(), m_files->countOf(ChangeKind::Modified),
        QStringLiteral("modified"));
    add(Glyph::Plus, theme::diffAddedFg(),
        m_files->countOf(ChangeKind::Added) + m_files->countOf(ChangeKind::Untracked),
        QStringLiteral("added"));
    add(Glyph::Minus, theme::diffRemovedFg(), m_files->countOf(ChangeKind::Deleted),
        QStringLiteral("deleted"));
    add(Glyph::Arrow, theme::accentAlt(), m_files->countOf(ChangeKind::Renamed),
        QStringLiteral("renamed"));
    row->addStretch(1);

    if (m_files->fileCount() == 0) {
        row->addWidget(new QLabel(QStringLiteral("no files"), m_summary));
    }
}

void CommitDetailsPanel::onFileSelectionChanged() {
    const QModelIndex index = m_fileView->currentIndex();
    if (!index.isValid()) {
        return;
    }
    if (m_viewAll->isChecked()) {
        m_files->setFolderFilter(QString());
        return;
    }
    if (CommitFilesModel::isFile(index)) {
        return;
    }
    // A folder is selected and the checkbox is off: the list narrows to it. That
    // is what the checkbox is for — seeing the whole commit at once instead.
    m_files->setFolderFilter(index.data(CommitFilesModel::PrefixRole).toString());
}

} // namespace gk
