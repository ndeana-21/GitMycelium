#pragma once

#include "core/types.h"
#include "model/commit_files_model.h"

#include <QColor>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QTreeView;

namespace gk {

class Skeleton;

class CommitFilesModel;
class WorkingTreePanel;

class CommitDetailsPanel : public QWidget {
    Q_OBJECT

public:
    explicit CommitDetailsPanel(QWidget* parent = nullptr);

    void showCommit(const CommitInfo& commit, const std::vector<ChangedFile>& files,
                    bool amendable);
    /// Two revisions side by side: the files that differ between them, with no single
    /// commit to describe. Nothing here is amendable. `label` overrides the default
    /// `base..target` heading, for a three-dot range or a working-tree target.
    void showComparison(const QString& base, const QString& target,
                        const std::vector<ChangedFile>& files, const QString& label = {});
    void showWorkingTree(const std::vector<FileChange>& changes);
    void setFilesLoading(bool loading);
    bool filesLoading() const;
    void setWorkingTreeLoading(bool loading);
    bool workingTreeLoading() const;
    void setWorkingTreeBranch(const QString& name, const QColor& color);
    void clear();
    /// Reports the selected commit's signature. `status` is git's `%G?` letter;
    /// the panel turns it into a sentence. Cleared by the modes that have no commit.
    void setSignatureInfo(const CommitSignature& signature);
    void setMode(CommitFilesModel::Mode mode);
    bool showingWorkingTree() const { return m_workingTree; }
    bool comparing() const { return m_comparing; }

    void refreshTheme();

    QString selectedPath() const;
    QStringList selectedPaths() const;
    void clearCommitMessage();

    /// The message the reader has typed, trailers and all. The window reads it to
    /// build a co-author block and writes the result back, so the field always shows
    /// exactly what will be committed.
    QString commitMessage() const;
    void setCommitMessage(const QString& message);
    bool amendRequested() const;
    bool pushAfterCommit() const;
    void clearStashMessage();
    QStringList stagedPaths() const;
    bool signCommit() const;
    void setSignCommit(bool on);
    void focusCommitMessage();
    void selectFirstFile();
    QString firstFilePath() const;
    /// Whether that first row is a staged change; a working-tree path can sit in
    /// either group, and the diff has to know which one it is looking at.
    bool firstFileIsStaged() const;
    /// Selects the row for `path` on the given side, if it is still in the list.
    /// Returns false when a staging action moved it to the other group.
    bool selectPath(const QString& path, bool staged);

signals:
    void messageAmendRequested(QString message);
    /// A file was clicked: show its diff. `staged` says which side of the working
    /// tree the row came from; it is what decides between staging and unstaging.
    void fileActivated(QString path, bool staged);
    /// A file became the current row, by click or because a commit was picked.
    /// Rendering the diff for this one is right; opening the viewer for it is not —
    /// selecting a commit must not make the graph disappear under the reader.
    void fileSelected(QString path, bool staged);
    void commitActivated(QString commitId);

    void stageRequested(QStringList paths);
    void unstageRequested(QStringList paths);
    void discardRequested(QStringList paths);
    void stashRequested(QStringList paths);
    void stageAllRequested();
    void unstageAllRequested();
    void discardAllRequested();
    void commitRequested(QString message);
    void amendToggled(bool on);
    void stashAllRequested(QString message);
    void signChanged(bool on);
    void coauthorsRequested();
    /// The file context menu asked to ignore the selection. `scope` is "gitignore"
    /// or "exclude"; `kind` is "file", "ext" or "dir".
    void ignoreRequested(QStringList paths, QString scope, QString kind);

private:
    void buildUi();
    void wireWorkingTree();
    void refreshMessageState();
    void refreshSummary();
    void onFileSelectionChanged();
    void cycleSort();
    void applySort();

    CommitFilesModel* m_files = nullptr;
    QTreeView* m_fileView = nullptr;
    Skeleton* m_fileSkeleton = nullptr;

    QStackedWidget* m_pages = nullptr;
    WorkingTreePanel* m_wip = nullptr;

    QPlainTextEdit* m_message = nullptr;
    QPushButton* m_amend = nullptr;

    QLabel* m_avatar = nullptr;
    QLabel* m_author = nullptr;
    QLabel* m_when = nullptr;
    QLabel* m_parent = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_signature = nullptr;

    QToolButton* m_sort = nullptr;
    /// Which order the file list is in. Kept beside the button because the button is
    /// the only control: the model cannot be asked what it was told to do.
    CommitFilesModel::Sort m_sortOrder = CommitFilesModel::PathAscending;
    QToolButton* m_pathMode = nullptr;
    QToolButton* m_treeMode = nullptr;
    QCheckBox* m_viewAll = nullptr;

    QWidget* m_messageArea = nullptr;
    QWidget* m_metaArea = nullptr;
    /// Author block, file-list controls and the list itself, as one splitter child:
    /// the page has a single handle, above this, and nothing between the author and
    /// the files.
    QWidget* m_bodyArea = nullptr;
    QString m_originalMessage;
    QString m_commitId;
    Signature m_lastAuthor;
    bool m_amendable = false;
    bool m_workingTree = false;
    bool m_comparing = false;
};

} // namespace gk
