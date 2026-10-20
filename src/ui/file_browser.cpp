#include "ui/file_browser.h"

#include "repo/repo_session.h"
#include "theme/theme.h"
#include "ui/elided_label.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QSplitter>
#include <QStandardItemModel>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace gk {
namespace {

constexpr int kPathRole = Qt::UserRole + 1;
constexpr int kDirectoryRole = Qt::UserRole + 2;
constexpr int kLoadedRole = Qt::UserRole + 3;
/// Past this the viewer shows the start of the file: a text widget holding hundreds
/// of megabytes is not a way to read anything.
constexpr std::size_t kShownBytes = 4 * 1024 * 1024;

class LineNumbers : public QWidget {
public:
    explicit LineNumbers(CodeView* view) : QWidget(view), m_view(view) {}
    QSize sizeHint() const override { return QSize(m_view->lineNumberWidth(), 0); }

protected:
    void paintEvent(QPaintEvent* event) override { m_view->paintLineNumbers(event); }

private:
    CodeView* m_view;
};

QString shownText(const std::string& bytes) {
    if (std::find(bytes.begin(), bytes.begin() + std::min<std::size_t>(bytes.size(), 8000), '\0') !=
        bytes.begin() + std::min<std::size_t>(bytes.size(), 8000)) {
        return QStringLiteral("Binary file, %1 bytes.").arg(bytes.size());
    }
    QString text = QString::fromUtf8(bytes.data(),
                                     static_cast<qsizetype>(std::min(bytes.size(), kShownBytes)));
    if (bytes.size() > kShownBytes) {
        text += QStringLiteral("\n\n… the first %1 MB of %2 bytes are shown.")
                    .arg(kShownBytes / (1024 * 1024))
                    .arg(bytes.size());
    }
    return text;
}

} // namespace

/// Colours a change the way the diff view does: added and removed lines, hunk headers.
class PatchHighlighter : public QSyntaxHighlighter {
public:
    using QSyntaxHighlighter::QSyntaxHighlighter;
    bool active = false;

protected:
    void highlightBlock(const QString& text) override {
        if (!active || text.isEmpty()) {
            return;
        }
        QTextCharFormat format;
        if (text.startsWith(QLatin1String("+++")) || text.startsWith(QLatin1String("---")) ||
            text.startsWith(QLatin1String("diff ")) || text.startsWith(QLatin1String("index "))) {
            format.setForeground(theme::textDim());
        } else if (text.startsWith(QLatin1Char('+'))) {
            format.setForeground(theme::diffAddedFg());
            format.setBackground(theme::diffAddedBg());
        } else if (text.startsWith(QLatin1Char('-'))) {
            format.setForeground(theme::diffRemovedFg());
            format.setBackground(theme::diffRemovedBg());
        } else if (text.startsWith(QLatin1String("@@"))) {
            format.setForeground(theme::diffHeader());
        } else {
            return;
        }
        setFormat(0, static_cast<int>(text.size()), format);
    }
};

CodeView::CodeView(QWidget* parent) : QPlainTextEdit(parent) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFont(theme::monoFont());
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_numbers = new LineNumbers(this);
    m_highlighter = new PatchHighlighter(document());
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

void CodeView::setPatchMode(bool patch) {
    m_highlighter->active = patch;
    m_highlighter->rehighlight();
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
    auto* viewerPane = new QWidget(right);
    auto* viewerLayout = new QVBoxLayout(viewerPane);
    viewerLayout->setContentsMargins(0, 0, 0, 0);
    viewerLayout->setSpacing(0);
    auto* viewerBar = new QHBoxLayout;
    viewerBar->setContentsMargins(8, 2, 8, 2);
    m_viewerTitle = new ElidedLabel(viewerPane);
    m_backToFile = new QToolButton(viewerPane);
    m_backToFile->setText(QStringLiteral("Show file"));
    m_backToFile->setVisible(false);
    connect(m_backToFile, &QToolButton::clicked, this, &FileBrowser::showFileContent);
    viewerBar->addWidget(m_viewerTitle, 1);
    viewerBar->addWidget(m_backToFile);
    viewerLayout->addLayout(viewerBar);
    m_viewer = new CodeView(viewerPane);
    m_viewer->setContextMenuPolicy(Qt::CustomContextMenu);
    m_viewer->setToolTip(QStringLiteral(
        "Select lines and right-click to see the commits that changed them."));
    connect(m_viewer, &QWidget::customContextMenuRequested, this, &FileBrowser::showViewerMenu);
    viewerLayout->addWidget(m_viewer, 1);

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
    layout->addWidget(split, 1);
    setViewer(QStringLiteral("Choose a file"), QString(), false);
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
    ++m_generation;
    m_model->clear();
    m_history->clear();
    m_entries.clear();
    m_openPath.clear();
    setViewer(QStringLiteral("Choose a file"), QString(), false);
}

void FileBrowser::setRevision(const QString& revision, const QString& label) {
    if (revision == m_revision && m_model->rowCount() > 0) {
        return;
    }
    m_revision = revision;
    m_revisionLabel = label;
    ++m_generation;
    m_title->setFullText(QStringLiteral("Files at %1").arg(label));
    m_model->clear();
    loadDirectory(m_model->invisibleRootItem(), QString());
    if (!m_openPath.isEmpty()) {
        openFile(m_openPath);
    }
}

void FileBrowser::loadDirectory(QStandardItem* parent, const QString& directory) {
    if (!m_session) {
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
            QStandardItem* parent = top ? m_model->invisibleRootItem()
                                        : m_model->itemFromIndex(anchor);
            parent->removeRows(0, parent->rowCount());
            parent->setData(true, kLoadedRole);
            for (const TreeEntry& entry : entries) {
                const QString name = QString::fromStdString(entry.name);
                auto* item = new QStandardItem(name + (entry.directory ? QStringLiteral("/")
                                                                       : QString()));
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

void FileBrowser::openFile(const QString& path) {
    if (!m_session) {
        return;
    }
    m_openPath = path;
    showFileContent();
    showFileHistory();
}

void FileBrowser::showFileContent() {
    if (!m_session || m_openPath.isEmpty()) {
        return;
    }
    const quint64 generation = m_generation;
    const QString revision = m_revision;
    const QString path = m_openPath;
    setViewer(QStringLiteral("%1 at %2").arg(path, m_revisionLabel), QStringLiteral("Loading…"),
              false);
    track(true);
    m_session->readAsync<QString>(
        this,
        [revision, path](RepoSession& session) {
            std::string bytes;
            QString error;
            if (!session.fileAtRevision(revision, path, &bytes, &error)) {
                return QStringLiteral("Cannot read %1: %2").arg(path, error);
            }
            return shownText(bytes);
        },
        [this, generation, path](QString text) {
            track(false);
            if (generation == m_generation && path == m_openPath) {
                setViewer(QStringLiteral("%1 at %2").arg(path, m_revisionLabel), text, false);
            }
        });
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
    const QString revision = m_revision;
    const QString path = m_openPath;
    const QString title = first == last
                              ? QStringLiteral("History of line %1 of %2").arg(first).arg(path)
                              : QStringLiteral("History of lines %1–%2 of %3")
                                    .arg(first)
                                    .arg(last)
                                    .arg(path);
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
        item->setText(3, QDateTime::fromSecsSinceEpoch(entry.whenUnix)
                             .toString(QStringLiteral("yyyy-MM-dd HH:mm")));
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
    const QString title = QStringLiteral("%1 in %2 — %3")
                              .arg(QString::fromStdString(entry.path),
                                   QString::fromStdString(entry.id).left(8),
                                   QString::fromStdString(entry.summary));
    if (!entry.patch.empty()) {
        setViewer(title, QString::fromStdString(entry.patch), true);
        return;
    }
    const quint64 generation = m_generation;
    const QString path = m_openPath;
    setViewer(title, QStringLiteral("Loading…"), true);
    track(true);
    m_session->readAsync<QString>(
        this,
        [entry](RepoSession& session) {
            return session.commitFilePatch(QString::fromStdString(entry.id),
                                           QString::fromStdString(entry.path));
        },
        [this, generation, path, title](QString patch) {
            track(false);
            if (generation == m_generation && path == m_openPath) {
                setViewer(title, patch.isEmpty() ? QStringLiteral("No change to show.") : patch,
                          true);
            }
        });
}

void FileBrowser::setViewer(const QString& title, const QString& text, bool patch) {
    m_viewerTitle->setFullText(title);
    m_viewer->setPatchMode(patch);
    m_viewer->setPlainText(text);
    m_backToFile->setVisible(patch && !m_openPath.isEmpty());
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
    const bool content = !m_backToFile->isVisible() && !m_openPath.isEmpty();
    QAction* lines = menu.addAction(
        first == last ? QStringLiteral("History of line %1").arg(first)
                      : QStringLiteral("History of lines %1–%2").arg(first).arg(last));
    lines->setEnabled(content);
    QAction* whole = menu.addAction(QStringLiteral("History of the whole file"));
    whole->setEnabled(!m_openPath.isEmpty());
    menu.addSeparator();
    QAction* copy = menu.addAction(QStringLiteral("Copy"));
    copy->setEnabled(cursor.hasSelection());
    QAction* chosen = menu.exec(m_viewer->viewport()->mapToGlobal(pos));
    if (chosen == lines) {
        showLineHistory(first, last);
    } else if (chosen == whole) {
        showFileHistory();
    } else if (chosen == copy) {
        m_viewer->copy();
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

void FileBrowser::expandForDiagnostics(const QString& directory) {
    const QModelIndexList found = m_model->match(m_model->index(0, 0), kPathRole, directory, 1,
                                                 Qt::MatchExactly | Qt::MatchRecursive);
    if (!found.isEmpty()) {
        m_tree->expand(found.front());
        onExpanded(found.front());
    }
}

} // namespace gk
