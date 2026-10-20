#pragma once

#include "core/types.h"

#include <QPlainTextEdit>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

class QModelIndex;
class QStandardItem;
class QStandardItemModel;
class QToolButton;
class QTreeView;
class QTreeWidget;

namespace gk {

class ElidedLabel;
class RepoSession;

/// A read-only text pane with line numbers: a file's content, or a change to it.
class CodeView : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit CodeView(QWidget* parent = nullptr);

    void setPatchMode(bool patch);
    int lineNumberWidth() const;
    void paintLineNumbers(QPaintEvent* event);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    QWidget* m_numbers = nullptr;
    class PatchHighlighter* m_highlighter = nullptr;
};

/// The repository as a tree of files at one commit, the place to read a file and
/// follow its history: every commit that touched the file (across renames), or only
/// the commits that touched the lines selected in it (`git log -L`). Choosing a
/// commit in the history shows the change it made.
///
/// Every read is a session read on a worker; an answer for a revision or a file the
/// reader has moved on from is dropped.
class FileBrowser : public QWidget {
    Q_OBJECT

public:
    explicit FileBrowser(QWidget* parent = nullptr);

    void setSession(RepoSession* session);
    /// The commit whose tree is shown; `label` names it for the reader.
    void setRevision(const QString& revision, const QString& label);
    QString revision() const { return m_revision; }

    void openFile(const QString& path);
    /// The history of lines `first`..`last` (1-based) of the open file.
    void showLineHistory(int first, int last);
    void showFileHistory();
    void showHistoryEntry(int row);
    void showFileContent();

    /// For headless checks.
    bool busy() const { return m_pending > 0; }
    QStringList treeRowsForDiagnostics() const;
    QStringList historyRowsForDiagnostics() const;
    QString viewerTitle() const;
    QString viewerText() const;
    void expandForDiagnostics(const QString& directory);

signals:
    /// Double-click on a history entry: show that commit in the graph.
    void commitActivated(QString commitId);

private:
    void loadDirectory(QStandardItem* parent, const QString& directory);
    void onExpanded(const QModelIndex& index);
    void onActivated(const QModelIndex& index);
    void fillHistory(const std::vector<FileHistoryEntry>& entries, const QString& title);
    void showViewerMenu(const QPoint& pos);
    void setViewer(const QString& title, const QString& text, bool patch);
    void track(bool started);

    RepoSession* m_session = nullptr;
    QString m_revision;
    QString m_revisionLabel;
    QString m_openPath;
    quint64 m_generation = 0;
    int m_pending = 0;
    std::vector<FileHistoryEntry> m_entries;

    ElidedLabel* m_title = nullptr;
    QTreeView* m_tree = nullptr;
    QStandardItemModel* m_model = nullptr;
    ElidedLabel* m_viewerTitle = nullptr;
    QToolButton* m_backToFile = nullptr;
    CodeView* m_viewer = nullptr;
    ElidedLabel* m_historyTitle = nullptr;
    QTreeWidget* m_history = nullptr;
};

} // namespace gk
