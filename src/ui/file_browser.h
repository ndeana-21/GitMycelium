#pragma once

#include "core/types.h"

#include <QPlainTextEdit>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <map>
#include <vector>

class QModelIndex;
class QStackedWidget;
class QStandardItem;
class QStandardItemModel;
class QToolButton;
class QTreeView;
class QTreeWidget;

namespace gk {

class CodeHighlighter;
class DiffView;
class ElidedLabel;
class RepoSession;

/// A text pane with line numbers and syntax colours: a file's content, read-only
/// unless the file is the working tree's.
class CodeView : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit CodeView(QWidget* parent = nullptr);

    /// Picks the colours by the path's language; past a megabyte the text stays plain.
    void setHighlighting(const QString& path, qsizetype size);
    int lineNumberWidth() const;
    void paintLineNumbers(QPaintEvent* event);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    QWidget* m_numbers = nullptr;
    CodeHighlighter* m_highlighter = nullptr;
};

/// The repository as a tree of files, the place to read and edit a file and follow its
/// history: every commit that touched the file (across renames), or only the commits
/// that touched the lines selected in it (`git log -L`). Choosing a commit shows the
/// change it made in the diff view, with its split mode.
///
/// The tree is either one commit's, read-only, or the working tree's, whose text files
/// can be edited and saved. Every read is a session read on a worker; an answer for a
/// revision or a file the reader has moved on from is dropped.
class FileBrowser : public QWidget {
    Q_OBJECT

public:
    explicit FileBrowser(QWidget* parent = nullptr);

    void setSession(RepoSession* session);
    /// The commit whose tree is shown; `label` names it for the reader.
    void setRevision(const QString& revision, const QString& label);
    /// The files on disk, editable. Read again on every call, unless the open file has
    /// unsaved edits.
    void setWorkingTree(const QString& label);
    bool workingTree() const { return m_workingTree; }
    QString revision() const { return m_revision; }
    /// Headless: leaving an edited file saves it instead of asking.
    void setHeadless(bool headless) { m_headless = headless; }

    void openFile(const QString& path);
    QString openPath() const { return m_openPath; }
    /// The path the tree's current row stands for.
    QString selectedTreePath() const;
    /// The history of lines `first`..`last` (1-based) of the open file.
    void showLineHistory(int first, int last);
    void showFileHistory();
    void showHistoryEntry(int row);
    void showFileContent();
    /// Writes the edited text back to the working tree. False when there is nothing to
    /// save or the write failed.
    bool save();
    bool editable() const;
    bool modified() const;

    /// For headless checks.
    bool busy() const { return m_pending > 0; }
    QStringList treeRowsForDiagnostics() const;
    QStringList historyRowsForDiagnostics() const;
    QString viewerTitle() const;
    QString viewerText() const;
    bool showingChanges() const;
    CodeView* editor() const { return m_viewer; }
    DiffView* changesView() const { return m_changes; }
    void expandForDiagnostics(const QString& directory);

signals:
    /// Double-click on a history entry: show that commit in the graph.
    void commitActivated(QString commitId);
    void fileSaved(QString path);

private:
    void reload();
    void loadDirectory(QStandardItem* parent, const QString& directory);
    void fillDirectory(QStandardItem* parent, const QString& directory,
                       const std::vector<TreeEntry>& entries);
    void onExpanded(const QModelIndex& index);
    /// Opens the folders down to the open file and selects it, reading a folder that
    /// has not been read yet and carrying on when it arrives.
    void revealOpenFile();
    void onActivated(const QModelIndex& index);
    void fillHistory(const std::vector<FileHistoryEntry>& entries, const QString& title);
    void showViewerMenu(const QPoint& pos);
    void setContent(const QString& title, const QString& text, bool editable);
    void updateSaveState();
    /// Asks what to do with unsaved edits before the file goes; false keeps it open.
    bool confirmLeave();
    void track(bool started);

    RepoSession* m_session = nullptr;
    QString m_revision;
    QString m_revisionLabel;
    bool m_workingTree = false;
    bool m_headless = false;
    QString m_openPath;
    QString m_revealPath;
    QString m_contentTitle;
    quint64 m_generation = 0;
    int m_pending = 0;
    bool m_crlf = false;
    bool m_editable = false;
    std::vector<FileHistoryEntry> m_entries;
    /// The working tree's folders, read once per load: `ls-files` answers for the
    /// whole tree at once, not a folder at a time.
    std::map<QString, std::vector<TreeEntry>> m_workingFolders;
    bool m_workingFoldersRead = false;

    ElidedLabel* m_title = nullptr;
    QTreeView* m_tree = nullptr;
    QStandardItemModel* m_model = nullptr;
    QStackedWidget* m_viewerStack = nullptr;
    ElidedLabel* m_viewerTitle = nullptr;
    QToolButton* m_save = nullptr;
    CodeView* m_viewer = nullptr;
    DiffView* m_changes = nullptr;
    ElidedLabel* m_historyTitle = nullptr;
    QTreeWidget* m_history = nullptr;
};

} // namespace gk
