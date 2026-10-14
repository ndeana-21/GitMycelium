#include "ui/conflict_view.h"

#include "core/conflict_auto.h"
#include "model/word_diff.h"
#include "theme/icons.h"
#include "theme/theme.h"
#include "ui/flow_layout.h"
#include "ui/syntax_highlight.h"
#include "ui/themed_icons.h"

#include "repo/repo_session.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextEdit>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace gk {
namespace {

/// The style inside every `<pre>` this pane builds. The monospace size follows
/// the theme so zoom reaches the merge editor, not only the diff viewer.
QString preStyle() {
    return QStringLiteral("font-family:'%2', monospace; font-size:%1px; margin:0; white-space:pre")
        .arg(theme::monoFontPixelSize())
        .arg(theme::uiFontFamily());
}

QTextEdit* makePane(QGroupBox* box, QWidget* parent) {
    auto* edit = new QTextEdit(parent);
    edit->setReadOnly(true);
    edit->setLineWrapMode(QTextEdit::NoWrap);
    edit->setFont(theme::monoFont());
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(edit);
    return edit;
}

QString absentNotice(const char* side) {
    return QStringLiteral("— the file does not exist on this side (%1 deleted it) —")
        .arg(QString::fromUtf8(side));
}

QString lineBody(const std::string& line) {
    QString text = QString::fromStdString(line);
    if (text.endsWith(QLatin1Char('\n'))) {
        text.chop(1);
    }
    if (text.endsWith(QLatin1Char('\r'))) {
        text.chop(1);
    }
    return text;
}

/// Splits text into display lines, dropping a trailing carriage return so a CRLF
/// file lines up with how it is compared. Shared by the panes and the emphasis, so
/// the two can never disagree about how many lines there are.
std::vector<QString> splitLines(const QString& text) {
    std::vector<QString> lines;
    const QStringList parts = text.split(QLatin1Char('\n'));
    lines.reserve(static_cast<std::size_t>(parts.size()));
    for (QString part : parts) {
        if (part.endsWith(QLatin1Char('\r'))) {
            part.chop(1);
        }
        lines.push_back(part);
    }
    return lines;
}

std::vector<QString> bodyLines(const std::vector<std::string>& lines) {
    std::vector<QString> out;
    out.reserve(lines.size());
    for (const std::string& line : lines) {
        out.push_back(lineBody(line));
    }
    return out;
}

/// Word emphasis for a region's two sides. Each is measured against the base when
/// there is one, and against the other side when there is not — an add/add has no
/// base, and comparing the two sides is then the only comparison available.
struct RegionEmphasis {
    std::vector<QVector<QPair<int, int>>> ours;
    std::vector<QVector<QPair<int, int>>> theirs;
};

RegionEmphasis regionEmphasis(const ConflictRegion& region) {
    const bool useBase = region.hasBase && !region.base.empty();
    const std::vector<std::string>& oursReference = useBase ? region.base : region.theirs;
    const std::vector<std::string>& theirsReference = useBase ? region.base : region.ours;
    RegionEmphasis emphasis;
    emphasis.ours = emphasisAgainstReference(bodyLines(region.ours), bodyLines(oursReference));
    emphasis.theirs =
        emphasisAgainstReference(bodyLines(region.theirs), bodyLines(theirsReference));
    return emphasis;
}

int emphasisedWordCount(const std::vector<QVector<QPair<int, int>>>& emphasis) {
    int count = 0;
    for (const QVector<QPair<int, int>>& line : emphasis) {
        count += line.size();
    }
    return count;
}

/// A read-only block of lines, syntax-coloured, with the words that differ from the
/// reference laid over a stronger background. Its own highlighter state: the three
/// sides of a region are alternatives, not one document.
QString renderLines(const std::vector<std::string>& lines, Language language,
                    const std::vector<QVector<QPair<int, int>>>& emphasis = {},
                    const QString& emphasisBackground = QString()) {
    HighlightState state;
    QString html = QStringLiteral("<pre style=\"%1\">").arg(preStyle());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const QVector<QPair<int, int>> ranges =
            i < emphasis.size() ? emphasis[i] : QVector<QPair<int, int>>{};
        html += renderHighlightedLine(lineBody(lines[i]), language, state, ranges,
                                      emphasisBackground) +
                QLatin1Char('\n');
    }
    html += QStringLiteral("</pre>");
    return html;
}

/// The same block, but every line is a target: clicking it takes or drops that
/// one line, which is the answer a whole-side chip cannot give. The marker and
/// the background carry the state, so a line's taken-ness is visible without a
/// second column of checkboxes.
QString renderSelectableLines(std::size_t region, bool oursSide,
                              const std::vector<std::string>& lines, Language language,
                              const std::vector<QVector<QPair<int, int>>>& emphasis,
                              const QString& emphasisBackground, const RegionLineTake& take) {
    HighlightState state;
    QString html = QStringLiteral("<pre style=\"%1\">").arg(preStyle());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const bool taken = oursSide ? (i < take.ours.size() && take.ours[i] != 0)
                                    : (i < take.theirs.size() && take.theirs[i] != 0);
        const QVector<QPair<int, int>> ranges =
            i < emphasis.size() ? emphasis[i] : QVector<QPair<int, int>>{};
        const QString marker = taken ? QStringLiteral("✔ ") : QStringLiteral("· ");
        const QString background =
            taken ? theme::success().name() : QStringLiteral("transparent");
        html += QStringLiteral("<a href=\"gk://regionline/%1/%2/%3\" "
                               "style=\"text-decoration:none; background:%4;\">%5%6</a>\n")
                    .arg(region)
                    .arg(oursSide ? QStringLiteral("ours") : QStringLiteral("theirs"))
                    .arg(i)
                    .arg(background, marker)
                    .arg(renderHighlightedLine(lineBody(lines[i]), language, state, ranges,
                                               emphasisBackground));
    }
    html += QStringLiteral("</pre>");
    return html;
}

/// What one region would become for each choice. `RegionUnanswered` yields
/// nothing because the caller keeps the marked bytes instead.
std::vector<std::string> chosenLines(const ConflictRegion& region, int choice,
                                     const RegionLineTake& lines) {
    return regionChosenLines(region, choice, lines);
}

QString sideLabel(int choice) {
    switch (choice) {
    case RegionOurs:
        return QStringLiteral("Ours");
    case RegionBase:
        return QStringLiteral("Base");
    case RegionTheirs:
        return QStringLiteral("Theirs");
    case RegionBoth:
        return QStringLiteral("Both (ours first)");
    case RegionBothReverse:
        return QStringLiteral("Both (theirs first)");
    case RegionLines:
        return QStringLiteral("Chosen lines");
    default:
        return QStringLiteral("unresolved");
    }
}

} // namespace

void ConflictView::setSession(RepoSession* session) {
    if (m_session == session) {
        return;
    }
    m_session = session;
    reload();
}

ConflictView::ConflictView(RepoSession* session, QWidget* parent)
    : QWidget(parent), m_session(session) {
    m_header = new QLabel(this);
    m_status = new QLabel(this);
    m_regionLabel = new QLabel(this);
    m_regionLabel->setStyleSheet(QStringLiteral("color:%1").arg(theme::textDim().name()));

    m_findField = new QLineEdit(this);
    m_findField->setPlaceholderText(QStringLiteral("Find in panes…"));
    m_findField->setClearButtonEnabled(true);
    m_findCase = new QCheckBox(QStringLiteral("Aa"), this);
    m_findCase->setToolTip(QStringLiteral("Match case"));
    auto* findPrev = new QToolButton(this);
    findPrev->setText(QStringLiteral("▲"));
    findPrev->setToolTip(QStringLiteral("Previous match"));
    auto* findNextButton = new QToolButton(this);
    findNextButton->setText(QStringLiteral("▼"));
    findNextButton->setToolTip(QStringLiteral("Next match"));
    connect(findPrev, &QToolButton::clicked, this, &ConflictView::findPrevious);
    connect(findNextButton, &QToolButton::clicked, this, &ConflictView::findNext);
    connect(m_findField, &QLineEdit::returnPressed, this, &ConflictView::findNext);

    auto* findBar = new QHBoxLayout;
    findBar->addWidget(new QLabel(QStringLiteral("Find:"), this));
    findBar->addWidget(m_findField, 1);
    findBar->addWidget(m_findCase);
    findBar->addWidget(findPrev);
    findBar->addWidget(findNextButton);

    m_fileList = new QTreeWidget(this);
    m_fileList->setColumnCount(1);
    m_fileList->setHeaderLabels({QStringLiteral("Conflicted files")});
    m_fileList->setRootIsDecorated(false);
    m_fileList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_fileList, &QTreeWidget::itemSelectionChanged, this, &ConflictView::onFileSelected);
    connect(m_fileList, &QWidget::customContextMenuRequested, this,
            &ConflictView::onFileContextMenu);

    m_resolvedList = new QTreeWidget(this);
    m_resolvedList->setObjectName(QStringLiteral("resolvedFiles"));
    m_resolvedList->setColumnCount(1);
    m_resolvedList->setRootIsDecorated(false);
    m_resolvedList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_resolvedList->setToolTip(
        QStringLiteral("Resolved in this operation — right-click to mark one as conflicted again"));
    m_resolvedList->setVisible(false);
    connect(m_resolvedList, &QWidget::customContextMenuRequested, this,
            &ConflictView::onResolvedContextMenu);

    auto* oursBox = new QGroupBox(QStringLiteral("Ours (current branch)"), this);
    auto* baseBox = new QGroupBox(QStringLiteral("Base (common ancestor)"), this);
    auto* theirsBox = new QGroupBox(QStringLiteral("Theirs (incoming)"), this);
    m_ours = makePane(oursBox, this);
    m_base = makePane(baseBox, this);
    m_theirs = makePane(theirsBox, this);

    auto* stages = new QSplitter(Qt::Horizontal, this);
    stages->addWidget(oursBox);
    stages->addWidget(baseBox);
    stages->addWidget(theirsBox);

    auto* resultBox = new QGroupBox(QStringLiteral("Result — pick a side for each conflict, "
                                                   "then save"), this);
    m_result = new QTextBrowser(resultBox);
    m_result->setOpenLinks(false);
    m_result->setOpenExternalLinks(false);
    m_result->setLineWrapMode(QTextEdit::NoWrap);
    m_result->setFont(theme::monoFont());
    m_resultEdit = new QTextEdit(resultBox);
    m_resultEdit->setLineWrapMode(QTextEdit::NoWrap);
    m_resultEdit->setFont(theme::monoFont());
    m_resultEdit->hide();
    auto* resultLayout = new QVBoxLayout(resultBox);
    resultLayout->setContentsMargins(4, 4, 4, 4);
    resultLayout->addWidget(m_result);
    resultLayout->addWidget(m_resultEdit);

    auto* right = new QSplitter(Qt::Vertical, this);
    right->addWidget(stages);
    right->addWidget(resultBox);
    right->setStretchFactor(0, 1);
    right->setStretchFactor(1, 1);

    auto* rightColumn = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightColumn);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(right, 1);

    auto* files = new QSplitter(Qt::Vertical, this);
    files->addWidget(m_fileList);
    files->addWidget(m_resolvedList);
    files->setStretchFactor(0, 3);
    files->setStretchFactor(1, 1);

    auto* body = new QSplitter(Qt::Horizontal, this);
    body->addWidget(files);
    body->addWidget(rightColumn);
    body->setStretchFactor(0, 1);
    body->setStretchFactor(1, 4);

    auto* takeOurs = new QPushButton(QStringLiteral("Take ours"), this);
    auto* takeBase = new QPushButton(QStringLiteral("Take base"), this);
    auto* takeTheirs = new QPushButton(QStringLiteral("Take theirs"), this);
    auto* takeBoth = new QPushButton(QStringLiteral("Take both"), this);
    m_takeBothButton = takeBoth;
    auto* clearChoicesButton = new QPushButton(QStringLiteral("Clear choices"), this);
    clearChoicesButton->setObjectName(QStringLiteral("clearChoices"));
    clearChoicesButton->setToolTip(
        QStringLiteral("Forget every side and line picked in this file; nothing is written"));
    m_manualButton = new QPushButton(QStringLiteral("Edit text manually"), this);
    m_manualButton->setCheckable(true);
    m_rerereButton = new QPushButton(QStringLiteral("Reuse recorded resolution"), this);
    m_rerereButton->setToolTip(QStringLiteral("Apply the resolution rerere remembered for this file"));
    m_smartButton = new QPushButton(QStringLiteral("Smart JSON merge"), this);
    m_smartButton->setToolTip(
        QStringLiteral("Merge the conflict key by key, leaving only keys that disagree"));
    m_stageWorkdirButton = new QPushButton(QStringLiteral("Stage working-tree file"), this);
    m_stageWorkdirButton->setToolTip(
        QStringLiteral("Stage the file as it is on disk, after resolving it elsewhere"));
    m_externalButton = new QPushButton(QStringLiteral("External merge tool"), this);
    m_externalButton->setToolTip(
        QStringLiteral("Open this file in the configured external merge tool (merge.tool)"));
    m_deleteButton = new QPushButton(QStringLiteral("Delete the file"), this);
    m_deleteButton->setObjectName(QStringLiteral("deleteModifyDelete"));
    m_deleteButton->setToolTip(
        QStringLiteral("Keep the deletion: remove the file from the index and the working tree"));
    m_keepModifiedButton = new QPushButton(QStringLiteral("Keep the changed file"), this);
    m_keepModifiedButton->setObjectName(QStringLiteral("deleteModifyKeepModified"));
    m_keepBaseButton = new QPushButton(QStringLiteral("Keep the base version"), this);
    m_keepBaseButton->setObjectName(QStringLiteral("deleteModifyKeepBase"));
    m_keepBaseButton->setToolTip(
        QStringLiteral("Undo the change as well: keep the file as both sides had it before"));
    m_keepDiskButton = new QPushButton(QStringLiteral("Keep the file on disk"), this);
    m_keepDiskButton->setObjectName(QStringLiteral("deleteModifyKeepDisk"));
    m_keepDiskButton->setToolTip(
        QStringLiteral("Stage the file as the working tree holds it now, with any edits made "
                       "since the merge stopped"));
    m_deleteModifyText = new QLabel(this);
    m_deleteModifyText->setWordWrap(true);
    m_deleteModifyPanel = new QFrame(this);
    m_deleteModifyPanel->setObjectName(QStringLiteral("deleteModifyPanel"));
    m_deleteModifyPanel->setProperty("role", QStringLiteral("messageBox"));
    auto* deleteModifyLayout = new QVBoxLayout(m_deleteModifyPanel);
    deleteModifyLayout->addWidget(m_deleteModifyText);
    auto* deleteModifyButtons = new FlowLayout;
    deleteModifyButtons->addWidget(m_keepModifiedButton);
    deleteModifyButtons->addWidget(m_keepBaseButton);
    deleteModifyButtons->addWidget(m_keepDiskButton);
    deleteModifyButtons->addWidget(m_deleteButton);
    deleteModifyLayout->addLayout(deleteModifyButtons);
    m_deleteModifyPanel->setVisible(false);
    rightLayout->insertWidget(0, m_deleteModifyPanel);
    m_saveDraftButton = new QPushButton(QStringLiteral("Save draft"), this);
    m_saveDraftButton->setToolTip(
        QStringLiteral("Write your partial resolution to the file with its markers kept. "
                       "It is not staged: the conflict stays until every marker is gone."));
    m_saveButton = new QPushButton(QStringLiteral("Save resolution"), this);
    m_abortButton = new QPushButton(QStringLiteral("Abort merge"), this);
    m_completeButton = new QPushButton(QStringLiteral("Complete merge"), this);
    m_saveButton->setDefault(true);
    m_syncScroll = new QCheckBox(QStringLiteral("Sync scroll"), this);
    m_syncScroll->setChecked(true);

    connect(takeOurs, &QPushButton::clicked, this, [this] { applyChoiceToAll(1); });
    connect(takeBase, &QPushButton::clicked, this, [this] { applyChoiceToAll(2); });
    connect(takeTheirs, &QPushButton::clicked, this, [this] { applyChoiceToAll(3); });
    connect(takeBoth, &QPushButton::clicked, this, [this] { applyChoiceToAll(4); });
    connect(clearChoicesButton, &QPushButton::clicked, this, [this] {
        clearChoices();
        setStatus(QStringLiteral("Cleared every choice in %1.").arg(m_path));
    });
    connect(m_manualButton, &QPushButton::toggled, this, &ConflictView::onToggleManual);
    connect(m_rerereButton, &QPushButton::clicked, this, &ConflictView::onRerereClicked);
    connect(m_smartButton, &QPushButton::clicked, this, &ConflictView::onSmartMerge);
    connect(m_stageWorkdirButton, &QPushButton::clicked, this, &ConflictView::onStageWorkingTree);
    connect(m_externalButton, &QPushButton::clicked, this, [this] {
        if (!m_path.isEmpty()) {
            emit externalMergeRequested(m_path);
        }
    });
    const auto answerDeleteModify = [this](const QString& answer) {
        const QString path = m_path;
        QString error;
        if (!resolveDeleteModify(answer, &error)) {
            setStatus(error);
            return;
        }
        setStatus(answer == QLatin1String("delete")
                      ? QStringLiteral("Deleted %1 — the conflict is settled.").arg(path)
                      : QStringLiteral("Kept %1 — the conflict is settled.").arg(path));
    };
    connect(m_deleteButton, &QPushButton::clicked, this,
            [answerDeleteModify] { answerDeleteModify(QStringLiteral("delete")); });
    connect(m_keepModifiedButton, &QPushButton::clicked, this,
            [answerDeleteModify] { answerDeleteModify(QStringLiteral("modified")); });
    connect(m_keepBaseButton, &QPushButton::clicked, this,
            [answerDeleteModify] { answerDeleteModify(QStringLiteral("base")); });
    connect(m_keepDiskButton, &QPushButton::clicked, this,
            [answerDeleteModify] { answerDeleteModify(QStringLiteral("disk")); });
    connect(m_saveDraftButton, &QPushButton::clicked, this, [this] {
        QString error;
        if (!saveDraft(&error)) {
            setStatus(error);
            return;
        }
        setStatus(QStringLiteral("Saved a draft of %1 — unresolved markers are kept, nothing staged.")
                      .arg(m_path));
    });
    connect(m_saveButton, &QPushButton::clicked, this, &ConflictView::onSaveClicked);
    connect(m_abortButton, &QPushButton::clicked, this, &ConflictView::abortMerge);
    connect(m_completeButton, &QPushButton::clicked, this, &ConflictView::completeMerge);
    connect(m_result, &QTextBrowser::anchorClicked, this, &ConflictView::onRegionClicked);
    connect(m_syncScroll, &QCheckBox::toggled, this, &ConflictView::onSyncScrollToggled);

    // A flow, not a row: fifteen controls in one line held the window 1600 px wide
    // even while this page was hidden behind the graph.
    auto* buttons = new FlowLayout;
    buttons->addWidget(takeOurs);
    buttons->addWidget(takeBase);
    buttons->addWidget(takeTheirs);
    buttons->addWidget(takeBoth);
    buttons->addWidget(clearChoicesButton);
    buttons->addWidget(m_manualButton);
    buttons->addWidget(m_rerereButton);
    buttons->addWidget(m_smartButton);
    buttons->addWidget(m_stageWorkdirButton);
    buttons->addWidget(m_externalButton);
    buttons->addWidget(m_saveDraftButton);
    buttons->addWidget(m_syncScroll);
    buttons->addWidget(m_saveButton);
    buttons->addWidget(m_abortButton);
    buttons->addWidget(m_completeButton);

    auto* headerRow = new QHBoxLayout;
    headerRow->addWidget(m_header, 1);
    headerRow->addWidget(m_regionLabel);

    // Leaving the merge editor is a page switch, and the page belongs to the window:
    // the view asks and the window answers. It sits at the right end of the header,
    // where the way out of a pane is looked for, and it is not a resolution — the
    // operation stays open and the reader can come back to it from the Branch menu.
    m_close = new QToolButton(this);
    m_close->setObjectName(QStringLiteral("conflictClose"));
    setThemedIcon(m_close, Icon::Close);
    m_close->setToolTip(QStringLiteral("Close the merge editor (the operation stays open)"));
    connect(m_close, &QToolButton::clicked, this, &ConflictView::closeRequested);
    headerRow->addWidget(m_close);

    auto* root = new QVBoxLayout(this);
    root->addLayout(headerRow);
    root->addLayout(findBar);
    root->addLayout(buttons);
    root->addWidget(body, 1);
    root->addWidget(m_status);

    // The panes scroll together; a conflict is a set of lines to compare, and
    // reading it in only one pane at a time is how they stop lining up.
    for (QTextEdit* pane : {m_ours, m_base, m_theirs, m_resultEdit, static_cast<QTextEdit*>(m_result)}) {
        connect(pane->verticalScrollBar(), &QScrollBar::valueChanged, this, [this, pane](int value) {
            if (m_syncing || !m_syncScroll->isChecked()) {
                return;
            }
            m_syncing = true;
            const int sourceMax = pane->verticalScrollBar()->maximum();
            for (QTextEdit* other :
                 {m_ours, m_base, m_theirs, m_resultEdit, static_cast<QTextEdit*>(m_result)}) {
                if (other == pane) {
                    continue;
                }
                const int targetMax = other->verticalScrollBar()->maximum();
                const int target =
                    sourceMax > 0 ? qRound(static_cast<double>(value) / sourceMax * targetMax) : 0;
                other->verticalScrollBar()->setValue(target);
            }
            m_syncing = false;
        });
    }

    auto* nextShortcut = new QShortcut(QKeySequence(Qt::Key_F8), this);
    nextShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(nextShortcut, &QShortcut::activated, this, &ConflictView::goToNextRegion);
    auto* prevShortcut = new QShortcut(QKeySequence(Qt::Key_F7), this);
    prevShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(prevShortcut, &QShortcut::activated, this, &ConflictView::goToPreviousRegion);
    auto* findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QShortcut::activated, this, [this] {
        m_findField->setFocus();
        m_findField->selectAll();
    });

    updateButtonState();
}

void ConflictView::refreshTheme() {
    m_regionLabel->setStyleSheet(QStringLiteral("color:%1").arg(theme::textDim().name()));
    if (m_current >= 0) {
        showEntry(m_current);
    }
}

void ConflictView::reloadResolved() {
    m_resolvedList->clear();
    m_resolvedPaths.clear();
    // The resolve-undo record outlives the commit that ends an operation, so it is
    // only read while one is open (or something is conflicted, as after a stash pop).
    const RepoInfo info = m_session->info();
    if (info.operation != RepoOperation::None || m_session->squashInProgress() ||
        !m_conflicts.empty()) {
        m_resolvedPaths = m_session->resolvedConflictPaths();
    }
    for (const QString& path : m_resolvedPaths) {
        auto* item = new QTreeWidgetItem(m_resolvedList, {path});
        item->setData(0, Qt::UserRole, path);
    }
    m_resolvedList->setHeaderLabels(
        {QStringLiteral("Resolved files (%1)").arg(m_resolvedPaths.size())});
    m_resolvedList->setVisible(!m_resolvedPaths.isEmpty());
}

bool ConflictView::markConflicted(const QString& path, QString* errorOut) {
    if (!m_session->markConflicted(path, errorOut)) {
        return false;
    }
    emit repositoryChanged();
    reload();
    selectFile(path);
    return true;
}

void ConflictView::onResolvedContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_resolvedList->itemAt(pos);
    if (!item) {
        return;
    }
    const QString path = item->data(0, Qt::UserRole).toString();
    QMenu menu(this);
    QAction* reopen = menu.addAction(QStringLiteral("Mark as conflicted"));
    if (menu.exec(m_resolvedList->viewport()->mapToGlobal(pos)) != reopen) {
        return;
    }
    QString error;
    if (!markConflicted(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("Cannot mark as conflicted"), error);
        return;
    }
    setStatus(QStringLiteral("%1 is conflicted again — the file on disk is unchanged.").arg(path));
}

void ConflictView::reload() {
    m_conflicts = m_session->conflicts();
    m_fileList->clear();
    m_current = -1;

    for (const ConflictEntry& entry : m_conflicts) {
        auto* item = new QTreeWidgetItem(
            m_fileList, {QStringLiteral("%1  [%2]").arg(QString::fromStdString(entry.path),
                                                        QString::fromUtf8(toString(entry.kind)))});
        item->setData(0, Qt::UserRole, QString::fromStdString(entry.path));
    }

    // An operation can be stopped with nothing conflicted — a step that turned out
    // to carry nothing, or a rebase waiting at a point where the reader has already
    // resolved everything. An empty screen headed "Merge is complete" tells them
    // nothing about why it is empty or what to do next.
    if (m_conflicts.empty()) {
        const bool rebasing = m_session->rebaseInProgress();
        const bool squashing = m_session->squashInProgress();
        const bool merging = m_session->mergeInProgress();
        if (rebasing) {
            m_header->setText(QStringLiteral("Rebase is stopped — nothing is conflicted. "
                                             "Continue or abort."));
        } else if (squashing) {
            m_header->setText(QStringLiteral("Squash is staged — nothing is conflicted. "
                                             "Commit the squashed result or abort."));
        } else if (merging) {
            m_header->setText(QStringLiteral("Merge is ready — nothing is conflicted. "
                                             "Complete it or abort."));
        } else {
            m_header->setText(QStringLiteral("Nothing is conflicted"));
        }
    } else {
        const bool rebasing = m_session->rebaseInProgress();
        const bool squashing = m_session->squashInProgress();
        m_header->setText(QStringLiteral("%1 — %2 conflicted file(s)")
                              .arg(rebasing ? QStringLiteral("Resolving rebase")
                                            : (squashing ? QStringLiteral("Resolving squash")
                                                         : QStringLiteral("Resolving merge")))
                              .arg(m_conflicts.size()));
    }

    if (!m_conflicts.empty()) {
        m_fileList->setCurrentItem(m_fileList->topLevelItem(0));
    } else {
        m_current = -1;
        m_document = ConflictDocument{};
        resetRegionChoices();
        m_currentRegion = -1;
        m_path.clear();
        m_deleteModify = false;
        m_deleteModifyPanel->setVisible(false);
        m_ours->clear();
        m_base->clear();
        m_theirs->clear();
        m_result->clear();
        m_resultEdit->clear();
    }
    reloadResolved();
    updateButtonState();
}

void ConflictView::onFileSelected() {
    const auto selected = m_fileList->selectedItems();
    if (selected.isEmpty()) {
        m_current = -1;
        return;
    }
    const QString path = selected.first()->data(0, Qt::UserRole).toString();
    for (std::size_t i = 0; i < m_conflicts.size(); ++i) {
        if (QString::fromStdString(m_conflicts[i].path) == path) {
            showEntry(static_cast<int>(i));
            return;
        }
    }
}

void ConflictView::setPaneText(QTextEdit* pane, const QString& text,
                               const std::vector<QVector<QPair<int, int>>>& emphasis,
                               const QString& emphasisBackground) {
    // The path is what tells the highlighter which language this is. A file nobody
    // taught it still renders — word emphasis needs no language, only offsets.
    const Language language = languageForPath(m_path);
    const std::vector<QString> lines = splitLines(text);

    HighlightState state;
    QString html = QStringLiteral("<pre style=\"%1\">").arg(preStyle());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const QVector<QPair<int, int>> ranges =
            i < emphasis.size() ? emphasis[i] : QVector<QPair<int, int>>{};
        html += renderHighlightedLine(lines[i], language, state, ranges, emphasisBackground) +
                QLatin1Char('\n');
    }
    html += QStringLiteral("</pre>");
    pane->setHtml(html);
}

void ConflictView::showEntry(int index) {
    if (index < 0 || index >= static_cast<int>(m_conflicts.size())) {
        return;
    }
    m_current = index;
    const ConflictEntry& entry = m_conflicts[static_cast<std::size_t>(index)];
    m_path = QString::fromStdString(entry.path);

    const auto stageText = [this](const Oid& oid, const char* side) {
        if (oid.empty()) {
            return absentNotice(side);
        }
        return m_session->conflictFileContent(QString::fromStdString(oid));
    };

    // Whether there is text to merge decides everything below, so ask first.
    bool textAvailable = false;
    const QString marked = m_session->conflictMarkedContent(m_path, &textAvailable);
    m_binary = !textAvailable;
    // A delete/modify conflict: one side removed the file, so the "keep that side"
    // answer is a deletion, not an empty file. The editor says so explicitly.
    m_deleteModify = entry.kind == ConflictKind::DeletedByUs ||
                     entry.kind == ConflictKind::DeletedByThem;
    showDeleteModifyQuestion(entry);
    m_document = (textAvailable && !marked.isEmpty())
                     ? parseConflictDocument(marked.toStdString())
                     : ConflictDocument{};
    resetRegionChoices();
    m_currentRegion = m_document.regions.empty() ? -1 : 0;
    m_manualMode = false;
    if (m_manualButton->isChecked()) {
        const QSignalBlocker block(m_manualButton);
        m_manualButton->setChecked(false);
    }

    if (m_binary) {
        // Nothing to compare as text: show each side's size and keep one whole.
        m_ours->setPlainText(binarySideNotice(entry.ours, "ours"));
        m_base->setPlainText(binarySideNotice(entry.ancestor, "base"));
        m_theirs->setPlainText(binarySideNotice(entry.theirs, "theirs"));
    } else {
        const QString oursText = stageText(entry.ours, "ours");
        const QString baseText = stageText(entry.ancestor, "base");
        const QString theirsText = stageText(entry.theirs, "theirs");

        // Word emphasis needs the base to compare against, and both sides to
        // compare; a side absent from the index (a deletion) has nothing to
        // underline.
        const bool canEmphasise =
            !entry.ancestor.empty() && !entry.ours.empty() && !entry.theirs.empty();
        const auto oursEmphasis =
            canEmphasise ? emphasisAgainstReference(splitLines(oursText), splitLines(baseText))
                         : std::vector<QVector<QPair<int, int>>>{};
        const auto theirsEmphasis =
            canEmphasise ? emphasisAgainstReference(splitLines(theirsText), splitLines(baseText))
                         : std::vector<QVector<QPair<int, int>>>{};
        const QString changed = theme::changedWordBg().name();
        setPaneText(m_ours, oursText, oursEmphasis, changed);
        setPaneText(m_base, baseText);
        setPaneText(m_theirs, theirsText, theirsEmphasis, changed);
    }

    // The file on disk is what a manual edit starts from, and the fallback when the
    // stages hold no text.
    const QString workDir = QString::fromStdString(m_session->info().workDir);
    QFile file(QDir(workDir).filePath(m_path));
    m_workdirMarked =
        file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();

    // rerere answers two questions about this file. `status` lists the conflicts
    // it is tracking (it has a preimage to forget); `remaining` lists those it has
    // NOT resolved from memory. A conflicted file that is in neither — with no
    // markers left on disk — is one rerere already applied and nobody staged.
    const bool rerereOn = m_session->rerereEnabled();
    const QStringList tracked = rerereOn ? m_session->rerereTrackedPaths() : QStringList{};
    const QStringList remaining = rerereOn ? m_session->rerereRemainingPaths() : QStringList{};
    m_rerereRemembered = rerereOn && tracked.contains(m_path);
    m_rerereApplied = rerereOn && !m_rerereRemembered && !remaining.contains(m_path) &&
                      !hasConflictMarkers(m_workdirMarked.toStdString());

    if (!m_binary && m_document.regions.empty()) {
        // A file that no longer carries markers: no cards to offer, so the editor
        // is the only way in.
        if (m_workdirMarked.isEmpty()) {
            m_resultEdit->setPlainText(stageText(entry.ours, "ours"));
        } else {
            m_resultEdit->setPlainText(m_workdirMarked);
        }
    } else if (!m_binary) {
        m_resultEdit->setPlainText(assembledText());
        refreshResult();
    }
    updateResultMode();
    updateButtonState();

    const QString kindLabel = QString::fromUtf8(toString(entry.kind));
    QString note = m_binary
                       ? QStringLiteral("%1 (%2) is binary — keep one whole side.").arg(m_path, kindLabel)
                       : QStringLiteral("Editing %1 (%2) — pick a side for each conflict, then save.")
                             .arg(m_path, kindLabel);
    if (m_rerereApplied) {
        note = QStringLiteral("%1 — rerere applied a remembered resolution; stage it.").arg(m_path);
    } else if (m_rerereRemembered) {
        note = QStringLiteral("%1 — rerere remembers a resolution for this file.").arg(m_path);
    }
    setStatus(note);
}

void ConflictView::updateResultMode() {
    const bool hasRegions = !m_document.regions.empty();
    if (m_binary) {
        // A binary file has no text to compare or merge; the result area explains
        // that and the whole-side buttons above are the way through it.
        m_manualMode = false;
        if (m_manualButton->isChecked()) {
            const QSignalBlocker block(m_manualButton);
            m_manualButton->setChecked(false);
        }
        m_manualButton->setEnabled(false);
        m_manualButton->setText(QStringLiteral("Edit text manually"));
        m_resultEdit->hide();
        m_result->show();
        m_result->setHtml(QStringLiteral(
            "<p><b>Binary file.</b> There is no text to compare or merge — keep "
            "one whole side above.</p>"));
        if (m_takeBothButton) {
            m_takeBothButton->setEnabled(false);
        }
        return;
    }
    if (m_takeBothButton) {
        m_takeBothButton->setEnabled(true);
    }
    m_manualButton->setEnabled(hasRegions && m_current >= 0);
    if (!hasRegions) {
        m_manualMode = true;
        if (!m_manualButton->isChecked()) {
            const QSignalBlocker block(m_manualButton);
            m_manualButton->setChecked(true);
        }
    }
    if (m_manualMode) {
        m_result->hide();
        m_resultEdit->show();
        m_manualButton->setText(hasRegions ? QStringLiteral("Use conflict cards")
                                           : QStringLiteral("Edit text manually"));
    } else {
        m_resultEdit->hide();
        m_result->show();
        m_manualButton->setText(QStringLiteral("Edit text manually"));
    }
}

void ConflictView::onToggleManual(bool manual) {
    if (m_document.regions.empty()) {
        // There are no cards to return to; the toggle is inert for this file.
        updateResultMode();
        return;
    }
    m_manualMode = manual;
    if (manual) {
        m_resultEdit->setPlainText(assembledText());
        setStatus(QStringLiteral("Editing the result by hand — Save writes it."));
    } else {
        // A hand edit is text, not a set of choices: re-read it and offer cards
        // only for the conflicts that are still marked.
        m_document = parseConflictDocument(m_resultEdit->toPlainText().toStdString());
        resetRegionChoices();
        m_currentRegion = m_document.regions.empty() ? -1 : 0;
        if (!m_document.regions.empty()) {
            refreshResult();
        }
        setStatus(QStringLiteral("Back to conflict cards."));
    }
    updateResultMode();
    updateButtonState();
}

int ConflictView::unresolvedRegionCount() const {
    int count = 0;
    for (std::size_t i = 0; i < m_document.regions.size(); ++i) {
        const int choice = i < m_choices.size() ? m_choices[i] : RegionUnanswered;
        const RegionLineTake& lines = i < m_lineTake.size() ? m_lineTake[i] : RegionLineTake{};
        if (!regionResolved(m_document.regions[i], choice, lines)) {
            ++count;
        }
    }
    return count;
}

void ConflictView::clearChoices() {
    resetRegionChoices();
    refreshResult();
    updateButtonState();
}

void ConflictView::resetRegionChoices() {
    m_choices.assign(m_document.regions.size(), RegionUnanswered);
    m_lineTake.assign(m_document.regions.size(), RegionLineTake{});
    for (std::size_t i = 0; i < m_document.regions.size(); ++i) {
        m_lineTake[i].resize(m_document.regions[i]);
    }
}

QString ConflictView::renderCards() const {
    const Language language = languageForPath(m_path);
    HighlightState state;
    QString html;
    for (const ConflictPiece& piece : m_document.pieces) {
        if (!piece.isRegion) {
            for (const std::string& line : piece.lines) {
                html += renderHighlightedLine(lineBody(line), language, state) + QLatin1Char('\n');
            }
            continue;
        }
        html += regionHtml(piece.region, m_document.regions[piece.region]);
    }
    return html;
}

QString ConflictView::regionHtml(std::size_t index, const ConflictRegion& region) const {
    const Language language = languageForPath(m_path);
    const int choice = index < m_choices.size() ? m_choices[index] : RegionUnanswered;
    const RegionLineTake& take =
        index < m_lineTake.size() ? m_lineTake[index] : RegionLineTake{};
    const bool resolved = regionResolved(region, choice, take);
    const bool perLine = choice == RegionLines && region.balanced;
    // A balanced region nobody answered, or one being answered line by line, shows
    // both sides with every line clickable.
    const bool showSides = region.balanced && (choice == RegionUnanswered || perLine);

    QString html = QStringLiteral("<a name=\"region-%1\"></a>").arg(index);
    html += QStringLiteral("<div style=\"border:1px solid %1; border-left:3px solid %2; "
                           "background:%3; margin:6px 0; padding:6px;\">")
                .arg(theme::border().name(),
                     resolved ? theme::success().name() : theme::warning().name(),
                     theme::surfaceAlt().name());
    html += QStringLiteral("<div style=\"color:%1\">Conflict %2/%3 — %4</div>")
                .arg(theme::textDim().name())
                .arg(index + 1)
                .arg(m_document.regions.size())
                .arg(resolved ? sideLabel(choice) : QStringLiteral("choose a side or lines"));
    html += QStringLiteral("<div>");
    html += chipHtml(index, RegionOurs, QStringLiteral("Ours"));
    html += chipHtml(index, RegionBase, QStringLiteral("Base"));
    html += chipHtml(index, RegionTheirs, QStringLiteral("Theirs"));
    html += chipHtml(index, RegionBoth, QStringLiteral("Both ↕ ours first"));
    html += chipHtml(index, RegionBothReverse, QStringLiteral("Both ↕ theirs first"));
    html += chipHtml(index, RegionLines, QStringLiteral("Lines"));
    html += QStringLiteral("</div>");

    if (showSides) {
        // Take a line by clicking it. The words each side changed against the base
        // are still picked out, so the choice is informed rather than blind.
        const RegionEmphasis emphasis = regionEmphasis(region);
        const QString changed = theme::changedWordBg().name();
        html += QStringLiteral("<div style=\"color:%1\">Ours — click a line to take it</div>")
                    .arg(theme::textDim().name());
        html += renderSelectableLines(index, true, region.ours, language, emphasis.ours, changed,
                                      take);
        if (region.hasBase) {
            html += QStringLiteral("<div style=\"color:%1\">Base</div>")
                        .arg(theme::textDim().name());
            html += renderLines(region.base, language);
        }
        html += QStringLiteral("<div style=\"color:%1\">Theirs — click a line to take it</div>")
                    .arg(theme::textDim().name());
        html += renderSelectableLines(index, false, region.theirs, language, emphasis.theirs,
                                      changed, take);
    } else {
        html += renderLines(chosenLines(region, choice, take), language);
    }

    html += QStringLiteral("</div>");
    return html;
}

QString ConflictView::chipHtml(std::size_t index, int choice, const QString& label) const {
    const bool active = index < m_choices.size() && m_choices[index] == choice;
    const QString bg = active ? theme::accent().name() : theme::surface().name();
    const QString fg = active ? theme::background().name() : theme::text().name();
    return QStringLiteral(" <a href=\"gk://region/%1/%2\" style=\"color:%3; background:%4; "
                          "text-decoration:none; padding:1px 6px;\">%5%6</a>")
        .arg(index)
        .arg(choice)
        .arg(fg, bg, active ? QStringLiteral("✓ ") : QString(), label.toHtmlEscaped());
}

void ConflictView::refreshResult() {
    if (m_manualMode || !m_result) {
        updateRegionLabel();
        return;
    }
    const int keep = m_currentRegion;
    m_result->setHtml(renderCards());
    updateRegionLabel();
    if (keep >= 0) {
        scrollToRegion(keep);
    }
}

void ConflictView::scrollToRegion(int index) {
    if (!m_result || index < 0) {
        return;
    }
    m_result->scrollToAnchor(QStringLiteral("region-%1").arg(index));
}

void ConflictView::applyChoice(int region, int choice) {
    if (region < 0 || region >= static_cast<int>(m_document.regions.size())) {
        return;
    }
    if (choice < RegionUnanswered || choice > RegionLines) {
        return;
    }
    m_choices[static_cast<std::size_t>(region)] = choice;
    m_currentRegion = region;
    refreshResult();
    updateButtonState();
    setStatus(choice == RegionUnanswered
                  ? QStringLiteral("Region %1 cleared.").arg(region + 1)
                  : QStringLiteral("Region %1: %2.").arg(region + 1).arg(sideLabel(choice)));
}

bool ConflictView::setRegionChoice(int region, const QString& choice) {
    if (region < 0 || region >= static_cast<int>(m_document.regions.size())) {
        return false;
    }
    const QString text = choice.toLower();
    int value = -1;
    if (text == QStringLiteral("clear") || text == QStringLiteral("none") || text == QStringLiteral("0")) {
        value = 0;
    } else if (text == QStringLiteral("ours") || text == QStringLiteral("1")) {
        value = 1;
    } else if (text == QStringLiteral("base") || text == QStringLiteral("2")) {
        value = 2;
    } else if (text == QStringLiteral("theirs") || text == QStringLiteral("3")) {
        value = 3;
    } else if (text == QStringLiteral("both") || text == QStringLiteral("4")) {
        value = 4;
    } else if (text == QStringLiteral("both-rev") || text == QStringLiteral("both-reverse") ||
               text == QStringLiteral("5")) {
        value = RegionBothReverse;
    } else if (text == QStringLiteral("lines") || text == QStringLiteral("6")) {
        value = RegionLines;
    }
    if (value < 0) {
        return false;
    }
    applyChoice(region, value);
    return true;
}

bool ConflictView::setRegionLine(int region, const QString& side, int line, bool take) {
    if (region < 0 || region >= static_cast<int>(m_lineTake.size())) {
        return false;
    }
    const bool oursSide = side.compare(QStringLiteral("ours"), Qt::CaseInsensitive) == 0;
    const bool theirsSide = side.compare(QStringLiteral("theirs"), Qt::CaseInsensitive) == 0;
    if (!oursSide && !theirsSide) {
        return false;
    }
    const ConflictRegion& conflict = m_document.regions[static_cast<std::size_t>(region)];
    const std::size_t count = oursSide ? conflict.ours.size() : conflict.theirs.size();
    if (line < 0 || line >= static_cast<int>(count)) {
        return false;
    }
    RegionLineTake& mask = m_lineTake[static_cast<std::size_t>(region)];
    if (oursSide) {
        mask.ours[static_cast<std::size_t>(line)] = take ? 1 : 0;
    } else {
        mask.theirs[static_cast<std::size_t>(line)] = take ? 1 : 0;
    }
    // A line take is a per-line answer; taking every line away leaves the region
    // unanswered again, which is what the reader sees.
    m_choices[static_cast<std::size_t>(region)] =
        mask.anyTaken() ? RegionLines : RegionUnanswered;
    m_currentRegion = region;
    refreshResult();
    updateButtonState();
    setStatus(QStringLiteral("Region %1: %2 line(s) taken.")
                  .arg(region + 1)
                  .arg(regionLineTakeCount(region, QStringLiteral("ours")) +
                       regionLineTakeCount(region, QStringLiteral("theirs"))));
    return true;
}

bool ConflictView::takeAllRegionLines(int region, const QString& side, bool take) {
    if (region < 0 || region >= static_cast<int>(m_lineTake.size())) {
        return false;
    }
    const bool oursSide = side.compare(QStringLiteral("ours"), Qt::CaseInsensitive) == 0;
    if (!oursSide && side.compare(QStringLiteral("theirs"), Qt::CaseInsensitive) != 0) {
        return false;
    }
    RegionLineTake& mask = m_lineTake[static_cast<std::size_t>(region)];
    mask.takeAll(oursSide, take);
    m_choices[static_cast<std::size_t>(region)] =
        mask.anyTaken() ? RegionLines : RegionUnanswered;
    m_currentRegion = region;
    refreshResult();
    updateButtonState();
    setStatus(QStringLiteral("Region %1: %2 lines %3.")
                  .arg(region + 1)
                  .arg(oursSide ? QStringLiteral("ours") : QStringLiteral("theirs"),
                       take ? QStringLiteral("taken") : QStringLiteral("dropped")));
    return true;
}

bool ConflictView::setRegionLineOrder(int region, bool oursFirst) {
    if (region < 0 || region >= static_cast<int>(m_lineTake.size())) {
        return false;
    }
    m_lineTake[static_cast<std::size_t>(region)].oursFirst = oursFirst;
    refreshResult();
    return true;
}

int ConflictView::regionLineTakeCount(int region, const QString& side) const {
    if (region < 0 || region >= static_cast<int>(m_lineTake.size())) {
        return 0;
    }
    const RegionLineTake& mask = m_lineTake[static_cast<std::size_t>(region)];
    const std::vector<char>& lines =
        side.compare(QStringLiteral("ours"), Qt::CaseInsensitive) == 0 ? mask.ours : mask.theirs;
    int taken = 0;
    for (const char flag : lines) {
        if (flag) {
            ++taken;
        }
    }
    return taken;
}

void ConflictView::applyChoiceToAll(int choice) {
    if (m_binary) {
        keepCurrentSide(choice);
        return;
    }
    if (m_document.regions.empty()) {
        // No cards: the whole-file buttons fill the editor with the chosen side.
        const QString text = choice == 1 ? m_ours->toPlainText()
                             : choice == 2 ? m_base->toPlainText()
                             : choice == 3 ? m_theirs->toPlainText()
                                           : m_ours->toPlainText() + m_theirs->toPlainText();
        m_resultEdit->setPlainText(text);
        setStatus(QStringLiteral("Result set to %1.").arg(sideLabel(choice)));
        return;
    }
    for (int& value : m_choices) {
        value = choice;
    }
    refreshResult();
    updateButtonState();
    setStatus(QStringLiteral("Every conflict set to %1.").arg(sideLabel(choice)));
}

void ConflictView::keepCurrentSide(int choice) {
    const QString kept = m_path;
    const QString side = choice == 1   ? QStringLiteral("ours")
                         : choice == 2 ? QStringLiteral("base")
                         : choice == 3 ? QStringLiteral("theirs")
                                       : QString();
    if (side.isEmpty()) {
        setStatus(QStringLiteral("\"Both\" has no meaning for a binary file."));
        return;
    }
    QString error;
    if (!keepSide(side, &error)) {
        QMessageBox::warning(this, QStringLiteral("Cannot keep that side"), error);
        return;
    }
    setStatus(QStringLiteral("Kept %1 for %2.").arg(sideLabel(choice), kept));
}

bool ConflictView::keepSide(const QString& side, QString* errorOut) {
    const int choice = side == QLatin1String("ours")     ? 1
                       : side == QLatin1String("base")   ? 2
                       : side == QLatin1String("theirs") ? 3
                                                         : 0;
    if (choice == 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("side must be ours, base or theirs");
        }
        return false;
    }
    if (m_current < 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("no conflicted file is selected");
        }
        return false;
    }
    const ConflictSide which = choice == 1 ? ConflictSide::Ours
                              : choice == 2 ? ConflictSide::Base
                                            : ConflictSide::Theirs;
    if (!m_session->keepConflictSide(m_path, which, errorOut)) {
        return false;
    }
    emit repositoryChanged();
    reload();
    return true;
}

QString ConflictView::binarySideNotice(const Oid& oid, const char* side) const {
    if (oid.empty()) {
        return absentNotice(side);
    }
    const long long bytes = m_session->blobSize(QString::fromStdString(oid));
    return QStringLiteral("— binary, %1 bytes —").arg(bytes);
}

void ConflictView::onSmartMerge() {
    QString error;
    if (!smartMerge(&error)) {
        setStatus(error);
    }
}

bool ConflictView::smartMerge(QString* errorOut) {
    if (m_current < 0 || m_current >= static_cast<int>(m_conflicts.size())) {
        if (errorOut) {
            *errorOut = QStringLiteral("no conflicted file is selected");
        }
        return false;
    }
    if (m_binary) {
        if (errorOut) {
            *errorOut = QStringLiteral("a binary file has no JSON to merge");
        }
        return false;
    }

    const RepoSession::SemanticJsonMerge merged = m_session->semanticMergeJson(m_path);
    if (!merged.applicable) {
        if (errorOut) {
            *errorOut = QStringLiteral("not a JSON object conflict — resolve the regions");
        }
        return false;
    }

    if (merged.hasConflicts) {
        // The remaining disagreements are exactly the regions now: the cards show
        // only those, and saving assembles from them as usual.
        m_document = parseConflictDocument(merged.merged.toStdString());
        resetRegionChoices();
        m_currentRegion = m_document.regions.empty() ? -1 : 0;
        m_manualMode = false;
        if (m_manualButton->isChecked()) {
            const QSignalBlocker block(m_manualButton);
            m_manualButton->setChecked(false);
        }
        m_resultEdit->setPlainText(merged.merged);
        refreshResult();
        updateResultMode();
        updateButtonState();
        setStatus(QStringLiteral("Smart merge: %1 key(s) still disagree.").arg(merged.conflicts));
        return true;
    }

    // Every key was decided: the merged document is what a save would write.
    m_document = ConflictDocument{};
    resetRegionChoices();
    m_currentRegion = -1;
    m_manualMode = true;
    if (!m_manualButton->isChecked()) {
        const QSignalBlocker block(m_manualButton);
        m_manualButton->setChecked(true);
    }
    m_resultEdit->setPlainText(merged.merged);
    updateResultMode();
    updateButtonState();
    setStatus(QStringLiteral("Smart merge resolved the JSON conflict — Save writes it."));
    return true;
}

void ConflictView::onStageWorkingTree() {
    const QString staged = m_path;
    QString error;
    if (!stageWorkingTree(&error)) {
        setStatus(error);
        return;
    }
    emit repositoryChanged();
    reload();
    setStatus(QStringLiteral("Staged the working-tree file for %1.").arg(staged));
}

QString ConflictView::workingTreePath(const QString& path) const {
    return QDir(QString::fromStdString(m_session->info().workDir)).filePath(path);
}

bool ConflictView::stagePath(const QString& path, QString* errorOut) {
    // Read it fresh: the reader resolved it somewhere else, so the copy the editor
    // loaded is not the copy to stage.
    QFile file(workingTreePath(path));
    const QString current =
        file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    if (current.isEmpty() && !QFileInfo::exists(workingTreePath(path))) {
        if (errorOut) {
            *errorOut = QStringLiteral("the file is not in the working tree");
        }
        return false;
    }
    if (hasConflictMarkers(current.toStdString())) {
        if (errorOut) {
            *errorOut = QStringLiteral("the working tree still carries conflict markers");
        }
        return false;
    }
    QString error;
    if (!m_session->resolveConflict(path, current, &error)) {
        if (errorOut) {
            *errorOut = error;
        }
        return false;
    }
    return true;
}

bool ConflictView::stageWorkingTree(QString* errorOut) {
    if (m_current < 0 || m_current >= static_cast<int>(m_conflicts.size())) {
        if (errorOut) {
            *errorOut = QStringLiteral("no conflicted file is selected");
        }
        return false;
    }
    return stagePath(m_path, errorOut);
}

bool ConflictView::markAllResolved(QStringList* skippedOut, QString* errorOut) {
    if (m_conflicts.empty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("there are no conflicted files");
        }
        return false;
    }
    QStringList skipped;
    for (const ConflictEntry& entry : m_conflicts) {
        const QString path = QString::fromStdString(entry.path);
        QString error;
        if (!stagePath(path, &error)) {
            // A file still carrying markers is not a failure of the operation; it is
            // a file nobody has resolved yet, and it is named so the reader knows
            // which ones are left.
            skipped << path;
        }
    }
    if (skippedOut) {
        *skippedOut = skipped;
    }
    if (!skipped.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("%1 file(s) still carry conflict markers")
                            .arg(skipped.size());
        }
    }
    return true;
}

QString ConflictView::currentKind() const {
    if (m_current < 0 || m_current >= static_cast<int>(m_conflicts.size())) {
        return {};
    }
    return QString::fromUtf8(toString(m_conflicts[static_cast<std::size_t>(m_current)].kind));
}

QString ConflictView::assembledText() const {
    QString out;
    for (const ConflictPiece& piece : m_document.pieces) {
        if (!piece.isRegion) {
            for (const std::string& line : piece.lines) {
                out += QString::fromStdString(line);
            }
            continue;
        }
        const std::size_t index = piece.region;
        const ConflictRegion& region = m_document.regions[index];
        const int choice = index < m_choices.size() ? m_choices[index] : RegionUnanswered;
        const RegionLineTake& take =
            index < m_lineTake.size() ? m_lineTake[index] : RegionLineTake{};
        if (!regionResolved(region, choice, take)) {
            // Keep the original bytes: a save must never lose what it did not use.
            for (const std::string& line : region.raw) {
                out += QString::fromStdString(line);
            }
        } else {
            for (const std::string& line : chosenLines(region, choice, take)) {
                out += QString::fromStdString(line);
            }
        }
    }
    return out;
}

QString ConflictView::resultText() const {
    return m_manualMode ? m_resultEdit->toPlainText() : assembledText();
}

QStringList ConflictView::describeRegions() const {
    QStringList out;
    for (std::size_t i = 0; i < m_document.regions.size(); ++i) {
        const ConflictRegion& region = m_document.regions[i];
        const int choice = i < m_choices.size() ? m_choices[i] : RegionUnanswered;
        const RegionLineTake& take = i < m_lineTake.size() ? m_lineTake[i] : RegionLineTake{};
        const RegionEmphasis emphasis = regionEmphasis(region);
        out << QStringLiteral("region %1 base=%2 ours=%3 theirs=%4 choice=%5 balanced=%6 "
                              "resolved=%7 takenOurs=%8 takenTheirs=%9 oursEmph=%10 theirsEmph=%11")
                   .arg(i)
                   .arg(region.hasBase ? QStringLiteral("yes") : QStringLiteral("no"))
                   .arg(region.ours.size())
                   .arg(region.theirs.size())
                   .arg(sideLabel(choice))
                   .arg(region.balanced ? QStringLiteral("yes") : QStringLiteral("no"))
                   .arg(regionResolved(region, choice, take) ? QStringLiteral("yes")
                                                             : QStringLiteral("no"))
                   .arg(regionLineTakeCount(static_cast<int>(i), QStringLiteral("ours")))
                   .arg(regionLineTakeCount(static_cast<int>(i), QStringLiteral("theirs")))
                   .arg(emphasisedWordCount(emphasis.ours))
                   .arg(emphasisedWordCount(emphasis.theirs));
    }
    return out;
}

bool ConflictView::selectFile(const QString& path) {
    for (int i = 0; i < m_fileList->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_fileList->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == path) {
            m_fileList->setCurrentItem(item);
            return true;
        }
    }
    return false;
}

void ConflictView::goToNextRegion() {
    if (m_document.regions.empty()) {
        return;
    }
    const int count = static_cast<int>(m_document.regions.size());
    m_currentRegion = m_currentRegion < 0 ? 0 : (m_currentRegion + 1) % count;
    refreshResult();
    updateButtonState();
    setStatus(QStringLiteral("Conflict %1/%2.").arg(m_currentRegion + 1).arg(count));
}

void ConflictView::goToPreviousRegion() {
    if (m_document.regions.empty()) {
        return;
    }
    const int count = static_cast<int>(m_document.regions.size());
    m_currentRegion = m_currentRegion < 0 ? count - 1 : (m_currentRegion - 1 + count) % count;
    refreshResult();
    updateButtonState();
    setStatus(QStringLiteral("Conflict %1/%2.").arg(m_currentRegion + 1).arg(count));
}

void ConflictView::setManualMode(bool manual) {
    if (m_document.regions.empty() && !manual) {
        updateResultMode();
        return;
    }
    if (m_manualButton->isChecked() != manual) {
        const QSignalBlocker block(m_manualButton);
        m_manualButton->setChecked(manual);
    }
    onToggleManual(manual);
}

bool ConflictView::deleteCurrentFile(QString* errorOut) {
    if (m_current < 0 || m_current >= static_cast<int>(m_conflicts.size())) {
        if (errorOut) {
            *errorOut = QStringLiteral("no conflicted file is selected");
        }
        return false;
    }
    if (!m_deleteModify) {
        if (errorOut) {
            *errorOut = QStringLiteral(
                "only a delete/modify conflict is settled by deleting the file");
        }
        return false;
    }
    return m_session->deleteConflictFile(m_path, errorOut);
}

QString ConflictView::deleteModifyLivingSide() const {
    if (!m_deleteModify || m_current < 0 || m_current >= static_cast<int>(m_conflicts.size())) {
        return QString();
    }
    return m_conflicts[static_cast<std::size_t>(m_current)].kind == ConflictKind::DeletedByUs
               ? QStringLiteral("theirs")
               : QStringLiteral("ours");
}

void ConflictView::showDeleteModifyQuestion(const ConflictEntry& entry) {
    m_deleteModifyPanel->setVisible(m_deleteModify);
    if (!m_deleteModify) {
        return;
    }
    const bool deletedByUs = entry.kind == ConflictKind::DeletedByUs;
    m_deleteModifyText->setText(
        QStringLiteral("<b>%1</b> was deleted on %2 and changed on %3. Which file should the "
                       "merge keep?")
            .arg(m_path.toHtmlEscaped(),
                 deletedByUs ? QStringLiteral("the current branch (ours)")
                             : QStringLiteral("the incoming side (theirs)"),
                 deletedByUs ? QStringLiteral("the incoming side (theirs)")
                             : QStringLiteral("the current branch (ours)")));
    m_keepModifiedButton->setToolTip(
        QStringLiteral("Keep the file with the changes made on %1")
            .arg(deletedByUs ? QStringLiteral("theirs") : QStringLiteral("ours")));
    m_keepBaseButton->setEnabled(!entry.ancestor.empty());
    m_keepDiskButton->setEnabled(QFileInfo::exists(workingTreePath(m_path)));
}

bool ConflictView::resolveDeleteModify(const QString& answer, QString* errorOut) {
    const auto refuse = [errorOut](const QString& why) {
        if (errorOut) {
            *errorOut = why;
        }
        return false;
    };
    if (m_current < 0 || m_current >= static_cast<int>(m_conflicts.size())) {
        return refuse(QStringLiteral("no conflicted file is selected"));
    }
    if (!m_deleteModify) {
        return refuse(QStringLiteral("%1 is not a delete/modify conflict").arg(m_path));
    }
    bool ok = false;
    if (answer == QLatin1String("modified")) {
        ok = keepSide(deleteModifyLivingSide(), errorOut);
    } else if (answer == QLatin1String("base")) {
        ok = keepSide(QStringLiteral("base"), errorOut);
    } else if (answer == QLatin1String("disk")) {
        if (!QFileInfo::exists(workingTreePath(m_path))) {
            return refuse(QStringLiteral("%1 is not in the working tree").arg(m_path));
        }
        ok = stageWorkingTree(errorOut);
    } else if (answer == QLatin1String("delete")) {
        ok = deleteCurrentFile(errorOut);
    } else {
        return refuse(QStringLiteral("answer must be modified, base, disk or delete"));
    }
    if (!ok) {
        return false;
    }
    // keepSide reloads on its own; the other two leave the list as it was, and a
    // settled file must not stay on it.
    if (answer == QLatin1String("disk") || answer == QLatin1String("delete")) {
        emit repositoryChanged();
        reload();
    }
    return true;
}

bool ConflictView::saveDraft(QString* errorOut) {
    if (m_current < 0 || m_current >= static_cast<int>(m_conflicts.size())) {
        if (errorOut) {
            *errorOut = QStringLiteral("no conflicted file is selected");
        }
        return false;
    }
    if (m_binary) {
        if (errorOut) {
            *errorOut = QStringLiteral("a binary conflict has no text to save as a draft");
        }
        return false;
    }
    // resultText() keeps the raw bytes of every region nobody has answered, so a
    // draft saved here loses nothing the reader has not decided to drop.
    return m_session->writeConflictDraft(m_path, resultText(), errorOut);
}

bool ConflictView::trySaveResolution(QString* errorOut) {
    if (m_current < 0 || m_current >= static_cast<int>(m_conflicts.size())) {
        if (errorOut) {
            *errorOut = QStringLiteral("no conflicted file is selected");
        }
        return false;
    }
    if (m_binary) {
        if (errorOut) {
            *errorOut = QStringLiteral(
                "a binary conflict is resolved by keeping a whole side, not by saving text");
        }
        return false;
    }
    const QString text = resultText();
    if (hasConflictMarkers(text.toStdString())) {
        if (errorOut) {
            *errorOut = QStringLiteral("every conflict must be settled — or the markers edited "
                                       "out — before the file can be staged");
        }
        return false;
    }

    // A delete/modify conflict resolved to nothing is a deletion: writing the empty
    // side as an empty file would stage a file git never had. `git rm` both settles
    // the conflict and stages the removal, which is what "keep the deletion" means.
    if (m_deleteModify && text.isEmpty()) {
        return deleteCurrentFile(errorOut);
    }

    QString error;
    if (!m_session->resolveConflict(m_path, text, &error)) {
        if (errorOut) {
            *errorOut = error;
        }
        return false;
    }
    return true;
}

void ConflictView::onSaveClicked() {
    QString error;
    if (!trySaveResolution(&error)) {
        QMessageBox::warning(this, QStringLiteral("Cannot save resolution"), error);
        return;
    }
    const QString saved = m_path;
    emit repositoryChanged();
    reload();
    setStatus(m_conflicts.empty()
                   ? QStringLiteral("All conflicts resolved — complete the merge.")
                   : QStringLiteral("Saved %1.").arg(saved));
}

void ConflictView::findNext() {
    findInPanes(m_findField->text(), true, m_findCase->isChecked());
}

void ConflictView::findPrevious() {
    findInPanes(m_findField->text(), false, m_findCase->isChecked());
}

bool ConflictView::findInPanes(const QString& text, bool forward, bool caseSensitive) {
    if (text.isEmpty()) {
        return false;
    }
    QWidget* focus = QApplication::focusWidget();
    QTextEdit* target = nullptr;
    const std::vector<QTextEdit*> panes = {m_ours, m_base, m_theirs, m_resultEdit,
                                           static_cast<QTextEdit*>(m_result)};
    for (QTextEdit* pane : panes) {
        if (pane == focus) {
            target = pane;
            break;
        }
    }
    if (!target) {
        target = m_manualMode ? m_resultEdit : static_cast<QTextEdit*>(m_result);
    }
    QTextDocument::FindFlags flags;
    if (!forward) {
        flags |= QTextDocument::FindBackward;
    }
    if (caseSensitive) {
        flags |= QTextDocument::FindCaseSensitively;
    }
    return target->find(text, flags);
}

void ConflictView::onRegionClicked(const QUrl& url) {
    if (url.scheme() != QStringLiteral("gk")) {
        return;
    }
    const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (url.host() == QStringLiteral("regionline") && parts.size() == 3) {
        bool ok = false;
        const int region = parts.at(0).toInt(&ok);
        if (!ok) {
            return;
        }
        bool lineOk = false;
        const int line = parts.at(2).toInt(&lineOk);
        if (!lineOk) {
            return;
        }
        // A click toggles; the anchor names the side, not the desired state, so a
        // second click on a taken line drops it.
        const bool oursSide = parts.at(1) == QStringLiteral("ours");
        const bool taken = region >= 0 && region < static_cast<int>(m_lineTake.size()) &&
                           m_lineTake[static_cast<std::size_t>(region)].isTaken(oursSide,
                                                                               static_cast<std::size_t>(line));
        setRegionLine(region, parts.at(1), line, !taken);
        return;
    }
    if (url.host() != QStringLiteral("region") || parts.size() != 2) {
        return;
    }
    bool ok = false;
    const int region = parts.at(0).toInt(&ok);
    if (!ok) {
        return;
    }
    const int choice = parts.at(1).toInt(&ok);
    if (ok) {
        applyChoice(region, choice);
    }
}

void ConflictView::onSyncScrollToggled(bool on) {
    Q_UNUSED(on);
    m_syncing = false;
}

void ConflictView::completeMerge() {
    if (!m_conflicts.empty()) {
        QMessageBox::information(this, QStringLiteral("Conflicts remain"),
                                 QStringLiteral("Resolve every file before completing."));
        return;
    }

    // The same three panes serve every operation; only the "finish" call and
    // the wording differ.
    const bool rebasing = m_session->rebaseInProgress();
    QString error;

    if (m_session->squashInProgress()) {
        // A squash ends as an ordinary single-parent commit: there is no MERGE_HEAD
        // to read and no second parent to name. git removes SQUASH_MSG on its own
        // commit; commitSquash removes it for us.
        if (!m_session->commitSquash(QString(), nullptr, &error)) {
            QMessageBox::warning(this, QStringLiteral("Cannot finish the squash"), error);
            return;
        }
        emit mergeCompleted();
        return;
    }

    if (rebasing) {
        RebaseResult result;
        if (!m_session->rebaseContinue(&result, &error)) {
            QMessageBox::warning(this, QStringLiteral("Cannot continue the rebase"), error);
            return;
        }
        if (result.status == RebaseStatus::Conflicted) {
            reload();
            setStatus(QStringLiteral("Another commit conflicts — resolve it and continue again."));
            return;
        }
        emit mergeCompleted();
        return;
    }

    if (!m_session->commitMerge(QStringLiteral("Merge completed in GitMycelium"), &error)) {
        QMessageBox::warning(this, QStringLiteral("Cannot complete the merge"), error);
        return;
    }
    emit mergeCompleted();
}

void ConflictView::abortMerge() {
    const bool rebasing = m_session->rebaseInProgress();
    const QString what = rebasing ? QStringLiteral("rebase")
                                  : (m_session->squashInProgress() ? QStringLiteral("squash")
                                                                   : QStringLiteral("merge"));
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Abort %1").arg(what),
        QStringLiteral("Discard the in-progress %1 and reset the working tree to HEAD?").arg(what));
    if (answer != QMessageBox::Yes) {
        return;
    }

    QString error;
    const bool ok = rebasing ? m_session->rebaseAbort(&error) : m_session->abortMerge(&error);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Cannot abort"), error);
        return;
    }
    emit mergeAborted();
}

bool ConflictView::stageRecordedResolution(QString* errorOut) {
    if (m_current < 0 || m_current >= static_cast<int>(m_conflicts.size())) {
        if (errorOut) {
            *errorOut = QStringLiteral("no conflicted file is selected");
        }
        return false;
    }
    if (hasConflictMarkers(m_workdirMarked.toStdString())) {
        if (errorOut) {
            *errorOut = QStringLiteral("the working tree still carries conflict markers — "
                                       "apply the recorded resolution first");
        }
        return false;
    }
    QString error;
    if (!m_session->resolveConflict(m_path, m_workdirMarked, &error)) {
        if (errorOut) {
            *errorOut = error;
        }
        return false;
    }
    return true;
}

void ConflictView::onRerereClicked() {
    if (m_rerereApplied) {
        // rerere already resolved the working tree; staging is all that is left.
        QString error;
        if (!stageRecordedResolution(&error)) {
            QMessageBox::warning(this, QStringLiteral("Cannot stage the resolution"), error);
            return;
        }
        emit repositoryChanged();
        reload();
        setStatus(QStringLiteral("Staged the resolution rerere remembered."));
        return;
    }

    QString error;
    if (!m_session->applyRerere(&error)) {
        QMessageBox::warning(this, QStringLiteral("Cannot reuse recorded resolutions"), error);
        return;
    }
    reload();
    if (m_rerereApplied) {
        setStatus(QStringLiteral("rerere applied a remembered resolution — stage it."));
    } else {
        setStatus(QStringLiteral("rerere had no recorded resolution to apply."));
    }
}

void ConflictView::onFileContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_fileList->itemAt(pos);
    if (!item) {
        return;
    }
    const QString path = item->data(0, Qt::UserRole).toString();
    QMenu menu(this);
    // Resolving is the common action, so it leads; the rerere entry is a footnote
    // and is only there when rerere is on.
    QAction* markResolved = menu.addAction(QStringLiteral("Mark as resolved"));
    QAction* markAll = menu.addAction(QStringLiteral("Mark all resolved"));
    markAll->setEnabled(!m_conflicts.empty());
    QAction* forget = nullptr;
    if (m_session->rerereEnabled()) {
        menu.addSeparator();
        forget = menu.addAction(QStringLiteral("Forget recorded resolution"));
        forget->setEnabled(m_session->rerereTrackedPaths().contains(path));
    }
    QAction* chosen = menu.exec(m_fileList->viewport()->mapToGlobal(pos));
    if (!chosen) {
        return;
    }
    if (chosen == markResolved) {
        QString error;
        if (!stagePath(path, &error)) {
            QMessageBox::warning(this, QStringLiteral("Cannot mark as resolved"), error);
            return;
        }
        reload();
        setStatus(QStringLiteral("Marked %1 resolved.").arg(path));
    } else if (chosen == markAll) {
        QStringList skipped;
        QString error;
        markAllResolved(&skipped, &error);
        reload();
        if (skipped.isEmpty()) {
            setStatus(QStringLiteral("Marked every conflicted file resolved."));
        } else {
            setStatus(QStringLiteral("%1 file(s) still carry markers: %2")
                          .arg(skipped.size())
                          .arg(skipped.join(QStringLiteral(", "))));
        }
    } else if (forget && chosen == forget) {
        QString error;
        if (!m_session->forgetRerere(path, &error)) {
            QMessageBox::warning(this, QStringLiteral("Cannot forget the resolution"), error);
            return;
        }
        reload();
        setStatus(QStringLiteral("Forgot the recorded resolution for %1.").arg(path));
    }
}

void ConflictView::updateRegionLabel() {
    if (!m_regionLabel) {
        return;
    }
    if (m_document.regions.empty()) {
        m_regionLabel->clear();
        return;
    }
    const int total = static_cast<int>(m_document.regions.size());
    const int current = m_currentRegion >= 0 ? m_currentRegion + 1 : 0;
    m_regionLabel->setText(QStringLiteral("conflict %1/%2 — %3 unresolved")
                               .arg(current)
                               .arg(total)
                               .arg(unresolvedRegionCount()));
}

void ConflictView::updateButtonState() {
    const bool resolved = m_conflicts.empty();
    m_completeButton->setEnabled(resolved);
    m_abortButton->setEnabled(true);

    const bool hasFile = m_current >= 0 && m_current < static_cast<int>(m_conflicts.size());
    m_saveButton->setEnabled(hasFile && !m_binary);
    m_manualButton->setEnabled(hasFile && !m_document.regions.empty() && !m_binary);
    if (m_smartButton) {
        m_smartButton->setEnabled(hasFile && !m_binary);
    }
    if (m_externalButton) {
        // A binary conflict has no text for a diff tool to open, but a merge tool
        // can still handle it — so this one stays available wherever there is a file.
        m_externalButton->setEnabled(hasFile);
    }
    if (m_stageWorkdirButton) {
        const bool workdirReady = hasFile && !m_workdirMarked.isEmpty() &&
                                  !hasConflictMarkers(m_workdirMarked.toStdString());
        m_stageWorkdirButton->setEnabled(workdirReady);
    }

    if (m_rerereButton) {
        const bool hasFile = m_current >= 0 && m_current < static_cast<int>(m_conflicts.size());
        const bool useful = hasFile && (m_rerereApplied || m_rerereRemembered);
        m_rerereButton->setEnabled(useful);
        m_rerereButton->setText(m_rerereApplied
                                    ? QStringLiteral("Stage recorded resolution")
                                    : QStringLiteral("Reuse recorded resolution"));
    }

    const bool rebasing = m_session->rebaseInProgress();
    const bool squashing = m_session->squashInProgress();
    m_completeButton->setText(rebasing ? QStringLiteral("Continue rebase")
                                       : (squashing ? QStringLiteral("Finish squash (commit)")
                                                    : QStringLiteral("Complete merge")));
    m_abortButton->setText(rebasing ? QStringLiteral("Abort rebase")
                                    : (squashing ? QStringLiteral("Abort squash")
                                                 : QStringLiteral("Abort merge")));
    updateRegionLabel();
}

void ConflictView::setStatus(const QString& text) {
    m_status->setText(text);
}

} // namespace gk
