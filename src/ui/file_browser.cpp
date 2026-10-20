#include "ui/file_browser.h"

#include "core/unified_diff.h"
#include "repo/repo_session.h"
#include "theme/theme.h"
#include "ui/diff_view.h"
#include "ui/elided_label.h"
#include "ui/syntax_highlight.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStringDecoder>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace gk {
namespace {

constexpr int kPathRole = Qt::UserRole + 1;
constexpr int kDirectoryRole = Qt::UserRole + 2;
constexpr int kLoadedRole = Qt::UserRole + 3;
/// Past this the viewer shows the start of the file: a text widget holding hundreds
/// of megabytes is not a way to read anything, and the start is not editable.
constexpr std::size_t kShownBytes = 4 * 1024 * 1024;
/// Colouring every line of a larger file at once stalls the window.
constexpr qsizetype kHighlightedBytes = 1024 * 1024;

class LineNumbers : public QWidget {
public:
    explicit LineNumbers(CodeView* view) : QWidget(view), m_view(view) {}
    QSize sizeHint() const override { return QSize(m_view->lineNumberWidth(), 0); }

protected:
    void paintEvent(QPaintEvent* event) override { m_view->paintLineNumbers(event); }

private:
    CodeView* m_view;
};

/// A file as the viewer takes it: text with `\n` line ends, and whether it can be
/// edited and written back unchanged in what the editor does not touch.
struct LoadedFile {
    QString text;
    bool editable = false;
    bool crlf = false;
    qsizetype size = 0;
};

LoadedFile loadedFile(const std::string& bytes, bool workingTree) {
    LoadedFile file;
    file.size = static_cast<qsizetype>(bytes.size());
    const auto probe = static_cast<std::ptrdiff_t>(std::min<std::size_t>(bytes.size(), 8000));
    if (std::find(bytes.begin(), bytes.begin() + probe, '\0') != bytes.begin() + probe) {
        file.text = QStringLiteral("Binary file, %1 bytes.").arg(bytes.size());
        return file;
    }
    const std::size_t shown = std::min(bytes.size(), kShownBytes);
    QStringDecoder decoder(QStringDecoder::Utf8);
    file.text = decoder(QByteArrayView(bytes.data(), static_cast<qsizetype>(shown)));
    file.crlf = bytes.find("\r\n") != std::string::npos;
    if (file.crlf) {
        file.text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    }
    // Only what can be written back as it was: the whole file, valid UTF-8.
    file.editable = workingTree && shown == bytes.size() && !decoder.hasError();
    if (bytes.size() > kShownBytes) {
        file.text += QStringLiteral("\n\n… the first %1 MB of %2 bytes are shown.")
                         .arg(kShownBytes / (1024 * 1024))
                         .arg(bytes.size());
    }
    return file;
}

QString timeText(std::int64_t unix) {
    return QDateTime::fromSecsSinceEpoch(unix).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

} // namespace

/// The diff view's colouring, line by line, for a whole document: the block state a
/// line leaves (an open comment or string) travels in the block's user data.
class CodeHighlighter : public QSyntaxHighlighter {
public:
    using QSyntaxHighlighter::QSyntaxHighlighter;
    Language language = Language::None;

protected:
    struct State : QTextBlockUserData {
        HighlightState state;
    };

    void highlightBlock(const QString& text) override {
        if (language == Language::None) {
            return;
        }
        HighlightState state;
        if (const QTextBlock previous = currentBlock().previous(); previous.isValid()) {
            if (const auto* data = static_cast<const State*>(previous.userData())) {
                state = data->state;
            }
        }
        for (const SyntaxRun& run : highlightLine(text, language, state)) {
            QTextCharFormat format;
            format.setForeground(run.colour);
            setFormat(run.begin, run.length, format);
        }
        auto* data = static_cast<State*>(currentBlockUserData());
        if (!data) {
            data = new State;
            setCurrentBlockUserData(data);
        }
        data->state = state;
        // A changed block state is what makes Qt colour the next line again.
        setCurrentBlockState(static_cast<int>(state.block) * 2 + (state.tripleQuoted ? 1 : 0) +
                             static_cast<int>(qHash(state.terminator) & 0xffff) * 8);
    }
};

CodeView::CodeView(QWidget* parent) : QPlainTextEdit(parent) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFont(theme::monoFont());
    m_numbers = new LineNumbers(this);
    m_highlighter = new CodeHighlighter(document());
    connect(this, &QPlainTextEdit::blockCountChanged, this,
            [this] { setViewportMargins(lineNumberWidth(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
        if (dy != 0) {
            m_numbers->scroll(0, dy);
        } else {
            m_numbers->update(0, rect.y(), m_numbers->width(), rect.height());
        }
    });
    setViewportMargins(lineNumberWidth(), 0, 0, 0);
}

void CodeView::setHighlighting(const QString& path, qsizetype size) {
    m_highlighter->language =
        size > kHighlightedBytes || path.isEmpty() ? Language::None : languageForPath(path);
}

int CodeView::lineNumberWidth() const {
    int digits = 1;
    for (int count = std::max(1, blockCount()); count >= 10; count /= 10) {
        ++digits;
    }
    return 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeView::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect area = contentsRect();
    m_numbers->setGeometry(QRect(area.left(), area.top(), lineNumberWidth(), area.height()));
}

void CodeView::paintLineNumbers(QPaintEvent* event) {
    QPainter painter(m_numbers);
    painter.fillRect(event->rect(), theme::panel());
    painter.setPen(theme::textDim());
    painter.setFont(font());
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    const int height = fontMetrics().height();
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible()) {
            painter.drawText(0, top, m_numbers->width() - 6, height, Qt::AlignRight,
                             QString::number(block.blockNumber() + 1));
        }
        top += qRound(blockBoundingRect(block).height());
        block = block.next();
    }
}

FileBrowser::FileBrowser(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_title = new ElidedLabel(this);
    m_title->setObjectName(QStringLiteral("filesTitle"));
    m_title->setContentsMargins(8, 4, 8, 4);
    layout->addWidget(m_title);

    auto* split = new QSplitter(Qt::Horizontal, this);
    m_tree = new QTreeView(split);
    m_model = new QStandardItemModel(this);
    m_tree->setModel(m_model);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_tree, &QTreeView::expanded, this, &FileBrowser::onExpanded);
    connect(m_tree, &QTreeView::clicked, this, &FileBrowser::onActivated);
    connect(m_tree, &QTreeView::activated, this, &FileBrowser::onActivated);

    auto* right = new QSplitter(Qt::Vertical, split);
    m_viewerStack = new QStackedWidget(right);

    auto* contentPane = new QWidget(m_viewerStack);
    auto* contentLayout = new QVBoxLayout(contentPane);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    auto* contentBar = new QHBoxLayout;
    contentBar->setContentsMargins(8, 2, 8, 2);
    m_viewerTitle = new ElidedLabel(contentPane);
    m_save = new QToolButton(contentPane);
    m_save->setText(QStringLiteral("Save"));
    m_save->setToolTip(QStringLiteral("Write the file to the working tree (Ctrl+S)"));
    m_save->setVisible(false);
    connect(m_save, &QToolButton::clicked, this, [this] { save(); });
    contentBar->addWidget(m_viewerTitle, 1);
    contentBar->addWidget(m_save);
    contentLayout->addLayout(contentBar);
    m_viewer = new CodeView(contentPane);
    m_viewer->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_viewer, &QWidget::customContextMenuRequested, this, &FileBrowser::showViewerMenu);
    connect(m_viewer->document(), &QTextDocument::modificationChanged, this,
            [this] { updateSaveState(); });
    contentLayout->addWidget(m_viewer, 1);
    m_viewerStack->addWidget(contentPane);

    m_changes = new DiffView(m_viewerStack);
    connect(m_changes, &DiffView::closeRequested, this,
            [this] { m_viewerStack->setCurrentIndex(0); });
    connect(m_changes, &DiffView::commitActivated, this, &FileBrowser::commitActivated);
    connect(m_changes, &DiffView::blameRequested, this, [this](const QString& path) {
        if (!m_session) {
            return;
        }
        const QString revision = m_changes->document().contentLabel;
        track(true);
        m_session->readAsync<std::vector<BlameLine>>(
            this, [path, revision](RepoSession& session) { return session.blame(path, revision); },
            [this](std::vector<BlameLine> lines) {
                track(false);
                m_changes->setBlame(lines);
            });
    });
    m_viewerStack->addWidget(m_changes);

    auto* saveKey = new QShortcut(QKeySequence::Save, this);
    saveKey->setContext(Qt::WidgetWithChildrenShortcut);
    connect(saveKey, &QShortcut::activated, this, [this] { save(); });

    auto* historyPane = new QWidget(right);
    auto* historyLayout = new QVBoxLayout(historyPane);
    historyLayout->setContentsMargins(0, 0, 0, 0);
    historyLayout->setSpacing(0);
    m_historyTitle = new ElidedLabel(historyPane);
    m_historyTitle->setContentsMargins(8, 4, 8, 4);
    historyLayout->addWidget(m_historyTitle);
    m_history = new QTreeWidget(historyPane);
    m_history->setColumnCount(4);
    m_history->setHeaderLabels({QStringLiteral("Commit"), QStringLiteral("Message"),
                                QStringLiteral("Author"), QStringLiteral("Date")});
    m_history->setRootIsDecorated(false);
    m_history->setUniformRowHeights(true);
    m_history->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_history->header()->setStretchLastSection(false);
    connect(m_history, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        showHistoryEntry(m_history->indexOfTopLevelItem(item));
    });
    connect(m_history, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        const int row = m_history->indexOfTopLevelItem(item);
        if (row >= 0 && row < static_cast<int>(m_entries.size())) {
            emit commitActivated(QString::fromStdString(m_entries[static_cast<std::size_t>(row)].id));
        }
    });
    historyLayout->addWidget(m_history, 1);

    // The elided labels size to their text alone; their margins need the room too.
    for (ElidedLabel* label : {m_title, m_viewerTitle, m_historyTitle}) {
        label->setMinimumHeight(label->fontMetrics().height() + 10);
    }
    right->setStretchFactor(0, 3);
    right->setStretchFactor(1, 2);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    split->setSizes({260, 780});
    layout->addWidget(split, 1);
    setContent(QStringLiteral("Choose a file"), QString(), false);
}

void FileBrowser::track(bool started) {
    m_pending += started ? 1 : -1;
}

void FileBrowser::setSession(RepoSession* session) {
    if (m_session == session) {
        return;
    }
    m_session = session;
    m_revision.clear();
    m_workingTree = false;
    ++m_generation;
    m_model->clear();
    m_history->clear();
    m_entries.clear();
    m_openPath.clear();
    setContent(QStringLiteral("Choose a file"), QString(), false);
}

void FileBrowser::setRevision(const QString& revision, const QString& label) {
    if (!m_workingTree && revision == m_revision && m_model->rowCount() > 0) {
        return;
    }
    if (!confirmLeave()) {
        return;
    }
    m_workingTree = false;
    m_revision = revision;
    m_revisionLabel = label;
    m_title->setFullText(QStringLiteral("Files at %1 · read-only").arg(label));
    reload();
}

void FileBrowser::setWorkingTree(const QString& label) {
    // The working tree changes under the browser, so it is read again each time it is
    // asked for, unless that would throw away an edit.
    if (m_workingTree && modified()) {
        return;
    }
    m_workingTree = true;
    m_revision.clear();
    m_revisionLabel = label;
    m_title->setFullText(QStringLiteral("Working tree — %1 · files can be edited").arg(label));
    reload();
}

void FileBrowser::reload() {
    ++m_generation;
    m_model->clear();
    m_workingFolders.clear();
    m_workingFoldersRead = false;
    if (!m_session) {
        return;
    }
    if (!m_workingTree) {
        loadDirectory(m_model->invisibleRootItem(), QString());
    } else {
        const quint64 generation = m_generation;
        track(true);
        m_session->readAsync<std::map<QString, std::vector<TreeEntry>>>(
            this,
            [](RepoSession& session) {
                // Every folder's entries from the one flat list: folders first, then by
                // name, as a commit's tree is shown.
                std::map<QString, std::map<QString, bool>> folders;
                for (const std::string& file : session.workingTreeFiles()) {
                    const QStringList parts = QString::fromStdString(file).split(QLatin1Char('/'));
                    QString parent;
                    for (qsizetype i = 0; i < parts.size(); ++i) {
                        bool& directory = folders[parent][parts[i]];
                        directory = directory || i + 1 < parts.size();
                        parent = parent.isEmpty() ? parts[i] : parent + QLatin1Char('/') + parts[i];
                    }
                }
                std::map<QString, std::vector<TreeEntry>> out;
                for (const auto& [folder, names] : folders) {
                    std::vector<TreeEntry>& entries = out[folder];
                    for (const auto& [name, directory] : names) {
                        TreeEntry entry;
                        entry.name = name.toStdString();
                        entry.directory = directory;
                        entries.push_back(std::move(entry));
                    }
                    std::stable_sort(entries.begin(), entries.end(),
                                     [](const TreeEntry& a, const TreeEntry& b) {
                                         return a.directory > b.directory;
                                     });
                }
                return out;
            },
            [this, generation](std::map<QString, std::vector<TreeEntry>> folders) {
                track(false);
                if (generation != m_generation) {
                    return;
                }
                m_workingFolders = std::move(folders);
                m_workingFoldersRead = true;
                fillDirectory(m_model->invisibleRootItem(), QString(),
                              m_workingFolders[QString()]);
            });
    }
    if (!m_openPath.isEmpty()) {
        showFileContent();
        showFileHistory();
    }
}

void FileBrowser::fillDirectory(QStandardItem* parent, const QString& directory,
                                const std::vector<TreeEntry>& entries) {
    parent->removeRows(0, parent->rowCount());
    parent->setData(true, kLoadedRole);
    for (const TreeEntry& entry : entries) {
        const QString name = QString::fromStdString(entry.name);
        auto* item =
            new QStandardItem(name + (entry.directory ? QStringLiteral("/") : QString()));
        item->setData(directory.isEmpty() ? name : directory + QLatin1Char('/') + name,
                      kPathRole);
        item->setData(entry.directory, kDirectoryRole);
        if (entry.submodule) {
            item->setForeground(theme::textDim());
            item->setToolTip(QStringLiteral("Submodule"));
        }
        if (entry.directory) {
            // A placeholder makes the folder expandable before it is read.
            item->appendRow(new QStandardItem(QStringLiteral("…")));
        }
        parent->appendRow(item);
    }
}

void FileBrowser::loadDirectory(QStandardItem* parent, const QString& directory) {
    if (!m_session) {
        return;
    }
    if (m_workingTree) {
        if (m_workingFoldersRead) {
            fillDirectory(parent, directory, m_workingFolders[directory]);
        }
        return;
    }
    const quint64 generation = m_generation;
    const QString revision = m_revision;
    const QPersistentModelIndex anchor = parent->index();
    const bool top = !anchor.isValid();
    track(true);
    m_session->readAsync<std::vector<TreeEntry>>(
        this,
        [revision, directory](RepoSession& session) {
            return session.treeEntries(revision, directory);
        },
        [this, generation, anchor, top, directory](std::vector<TreeEntry> entries) {
            track(false);
            if (generation != m_generation || (!top && !anchor.isValid())) {
                return;
            }
            fillDirectory(top ? m_model->invisibleRootItem() : m_model->itemFromIndex(anchor),
                          directory, entries);
        });
}

void FileBrowser::onExpanded(const QModelIndex& index) {
    QStandardItem* item = m_model->itemFromIndex(index);
    if (item && item->data(kDirectoryRole).toBool() && !item->data(kLoadedRole).toBool()) {
        loadDirectory(item, item->data(kPathRole).toString());
    }
}

void FileBrowser::onActivated(const QModelIndex& index) {
    if (!index.data(kDirectoryRole).toBool() && !index.data(kPathRole).toString().isEmpty()) {
        openFile(index.data(kPathRole).toString());
    }
}

bool FileBrowser::confirmLeave() {
    if (!modified()) {
        return true;
    }
    if (m_headless) {
        return save();
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Unsaved changes"),
        QStringLiteral("Save the changes to %1?").arg(m_openPath),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) {
        return save();
    }
    if (answer == QMessageBox::Discard) {
        m_viewer->document()->setModified(false);
        return true;
    }
    return false;
}

void FileBrowser::openFile(const QString& path) {
    if (!m_session || path.isEmpty()) {
        return;
    }
    if (path != m_openPath && !confirmLeave()) {
        return;
    }
    m_openPath = path;
    showFileContent();
    showFileHistory();
}

void FileBrowser::showFileContent() {
    m_viewerStack->setCurrentIndex(0);
    if (!m_session || m_openPath.isEmpty()) {
        return;
    }
    const quint64 generation = m_generation;
    const QString revision = m_revision;
    const bool workingTree = m_workingTree;
    const QString path = m_openPath;
    const QString title = workingTree ? QStringLiteral("%1 — working tree").arg(path)
                                      : QStringLiteral("%1 at %2").arg(path, m_revisionLabel);
    setContent(title, QStringLiteral("Loading…"), false);
    track(true);
    m_session->readAsync<LoadedFile>(
        this,
        [revision, path, workingTree](RepoSession& session) {
            std::string bytes;
            QString error;
            const bool read = workingTree ? session.workingFileContent(path, &bytes, &error)
                                          : session.fileAtRevision(revision, path, &bytes, &error);
            if (!read) {
                LoadedFile missing;
                missing.text = QStringLiteral("Cannot read %1: %2").arg(path, error);
                return missing;
            }
            return loadedFile(bytes, workingTree);
        },
        [this, generation, path, title](LoadedFile file) {
            track(false);
            if (generation != m_generation || path != m_openPath) {
                return;
            }
            m_crlf = file.crlf;
            m_viewer->setHighlighting(path, file.size);
            setContent(file.editable ? title : title + QStringLiteral(" · read-only"), file.text,
                       file.editable);
        });
}

void FileBrowser::setContent(const QString& title, const QString& text, bool editable) {
    m_contentTitle = title;
    m_editable = editable;
    m_viewer->setPlainText(text);
    m_viewer->setReadOnly(!editable);
    m_viewer->setTextInteractionFlags(editable ? Qt::TextEditorInteraction
                                               : Qt::TextSelectableByMouse |
                                                     Qt::TextSelectableByKeyboard);
    m_viewer->document()->setModified(false);
    updateSaveState();
}

void FileBrowser::updateSaveState() {
    const bool dirty = modified();
    m_viewerTitle->setFullText(dirty ? m_contentTitle + QStringLiteral(" · modified")
                                     : m_contentTitle);
    m_save->setVisible(m_editable);
    m_save->setEnabled(dirty);
}

bool FileBrowser::editable() const {
    return m_editable;
}

bool FileBrowser::modified() const {
    return m_editable && m_viewer->document()->isModified();
}

bool FileBrowser::save() {
    if (!m_session || !modified()) {
        return false;
    }
    QString text = m_viewer->toPlainText();
    if (m_crlf) {
        text.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    }
    const QByteArray bytes = text.toUtf8();
    QString error;
    if (!m_session->writeWorkingFile(
            m_openPath, std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())),
            &error)) {
        if (!m_headless) {
            QMessageBox::warning(this, QStringLiteral("Cannot save"), error);
        }
        return false;
    }
    m_viewer->document()->setModified(false);
    emit fileSaved(m_openPath);
    return true;
}

void FileBrowser::showFileHistory() {
    if (!m_session || m_openPath.isEmpty()) {
        return;
    }
    const quint64 generation = m_generation;
    const QString revision = m_revision;
    const QString path = m_openPath;
    m_historyTitle->setFullText(QStringLiteral("History of %1 — reading…").arg(path));
    track(true);
    m_session->readAsync<std::vector<FileHistoryEntry>>(
        this,
        [revision, path](RepoSession& session) { return session.fileHistory(path, revision); },
        [this, generation, path](std::vector<FileHistoryEntry> entries) {
            track(false);
            if (generation == m_generation && path == m_openPath) {
                fillHistory(entries, QStringLiteral("History of %1").arg(path));
            }
        });
}

void FileBrowser::showLineHistory(int first, int last) {
    if (!m_session || m_openPath.isEmpty() || first < 1 || last < first) {
        return;
    }
    const quint64 generation = m_generation;
    // The working tree's lines are asked of HEAD: `git log -L` reads committed text.
    const QString revision = m_workingTree ? QStringLiteral("HEAD") : m_revision;
    const QString path = m_openPath;
    QString title = first == last ? QStringLiteral("History of line %1 of %2").arg(first).arg(path)
                                  : QStringLiteral("History of lines %1–%2 of %3")
                                        .arg(first)
                                        .arg(last)
                                        .arg(path);
    if (m_workingTree) {
        title += QStringLiteral(" (as committed in HEAD)");
    }
    m_historyTitle->setFullText(title + QStringLiteral(" — reading…"));
    track(true);
    m_session->readAsync<std::vector<FileHistoryEntry>>(
        this,
        [revision, path, first, last](RepoSession& session) {
            return session.lineHistory(revision, path, first, last);
        },
        [this, generation, path, title](std::vector<FileHistoryEntry> entries) {
            track(false);
            if (generation == m_generation && path == m_openPath) {
                fillHistory(entries, title);
            }
        });
}

void FileBrowser::fillHistory(const std::vector<FileHistoryEntry>& entries, const QString& title) {
    m_entries = entries;
    m_history->clear();
    for (const FileHistoryEntry& entry : entries) {
        auto* item = new QTreeWidgetItem(m_history);
        item->setText(0, QString::fromStdString(entry.id).left(8));
        item->setText(1, QString::fromStdString(entry.summary));
        item->setText(2, QString::fromStdString(entry.author));
        item->setText(3, timeText(entry.whenUnix));
        item->setToolTip(1, QString::fromStdString(entry.summary));
        item->setFont(0, theme::monoFont());
    }
    m_history->resizeColumnToContents(0);
    m_history->resizeColumnToContents(3);
    m_historyTitle->setFullText(QStringLiteral("%1 · %2 commit(s)").arg(title).arg(entries.size()));
}

void FileBrowser::showHistoryEntry(int row) {
    if (!m_session || row < 0 || row >= static_cast<int>(m_entries.size())) {
        return;
    }
    const FileHistoryEntry entry = m_entries[static_cast<std::size_t>(row)];
    const quint64 generation = m_generation;
    const QString path = m_openPath;
    m_viewerStack->setCurrentIndex(1);
    m_changes->setLoading(true);
    track(true);
    m_session->readAsync<DiffView::Document>(
        this,
        [entry](RepoSession& session) {
            const QString commit = QString::fromStdString(entry.id);
            const QString atPath = QString::fromStdString(entry.path);
            DiffView::Document document;
            document.path = atPath;
            if (!entry.patch.empty()) {
                // A line range's history carries its own change, to those lines only.
                const std::vector<FileDiff> parsed = parseUnifiedDiff(entry.patch);
                if (!parsed.empty()) {
                    document.diff = parsed.front();
                }
            } else {
                document.diff = session.commitFileDiff(commit, atPath);
            }
            std::string bytes;
            if (session.fileAtRevision(commit, atPath, &bytes, nullptr)) {
                document.revisionContent = std::move(bytes);
                document.hasRevision = true;
            }
            document.contentLabel = commit.left(7);
            return document;
        },
        [this, generation, path](DiffView::Document document) {
            track(false);
            if (generation == m_generation && path == m_openPath) {
                m_changes->show(document);
                m_changes->setLoading(false);
            }
        });
}

void FileBrowser::showViewerMenu(const QPoint& pos) {
    QMenu menu(this);
    const QTextCursor cursor = m_viewer->textCursor();
    QTextCursor startCursor(m_viewer->document());
    startCursor.setPosition(cursor.selectionStart());
    QTextCursor endCursor(m_viewer->document());
    endCursor.setPosition(cursor.selectionEnd());
    const int first = startCursor.blockNumber() + 1;
    int last = endCursor.blockNumber() + 1;
    // A selection that ends at the start of a line does not take that line.
    if (cursor.hasSelection() && endCursor.positionInBlock() == 0 && last > first) {
        --last;
    }
    QAction* lines = menu.addAction(
        first == last ? QStringLiteral("History of line %1").arg(first)
                      : QStringLiteral("History of lines %1–%2").arg(first).arg(last));
    lines->setEnabled(!m_openPath.isEmpty());
    QAction* whole = menu.addAction(QStringLiteral("History of the whole file"));
    whole->setEnabled(!m_openPath.isEmpty());
    menu.addSeparator();
    QAction* copy = menu.addAction(QStringLiteral("Copy"));
    copy->setEnabled(cursor.hasSelection());
    QAction* saveAction = nullptr;
    if (m_editable) {
        saveAction = menu.addAction(QStringLiteral("Save"));
        saveAction->setEnabled(modified());
    }
    QAction* chosen = menu.exec(m_viewer->viewport()->mapToGlobal(pos));
    if (chosen == lines) {
        showLineHistory(first, last);
    } else if (chosen == whole) {
        showFileHistory();
    } else if (chosen == copy) {
        m_viewer->copy();
    } else if (chosen && chosen == saveAction) {
        save();
    }
}

QStringList FileBrowser::treeRowsForDiagnostics() const {
    QStringList rows;
    std::function<void(const QStandardItem*, int)> walk = [&](const QStandardItem* parent,
                                                              int depth) {
        for (int row = 0; row < parent->rowCount(); ++row) {
            const QStandardItem* item = parent->child(row);
            rows << QString(depth * 2, QLatin1Char(' ')) + item->text();
            walk(item, depth + 1);
        }
    };
    walk(m_model->invisibleRootItem(), 0);
    return rows;
}

QStringList FileBrowser::historyRowsForDiagnostics() const {
    QStringList rows;
    rows << m_historyTitle->fullText();
    for (int row = 0; row < m_history->topLevelItemCount(); ++row) {
        const QTreeWidgetItem* item = m_history->topLevelItem(row);
        rows << QStringLiteral("%1 %2").arg(item->text(0), item->text(1));
    }
    return rows;
}

QString FileBrowser::viewerTitle() const {
    return m_viewerTitle->fullText();
}

QString FileBrowser::viewerText() const {
    return m_viewer->toPlainText();
}

bool FileBrowser::showingChanges() const {
    return m_viewerStack->currentIndex() == 1;
}

void FileBrowser::expandForDiagnostics(const QString& directory) {
    const QModelIndexList found = m_model->match(m_model->index(0, 0), kPathRole, directory, 1,
                                                 Qt::MatchExactly | Qt::MatchRecursive);
    if (!found.isEmpty()) {
        m_tree->expand(found.front());
        onExpanded(found.front());
    }
}

} // namespace gk
