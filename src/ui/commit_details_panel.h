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

/// One commit, in full: what it says, who wrote it and when, how much it changed,
/// and the files it touched — as a flat list of paths or as a folder tree.
///
/// The message is editable only where editing it is honest: on the commit HEAD
/// points at, where a change is an amend. Rewording an older commit rebuilds
/// everything after it, and a text field that silently rewrites history is worse
/// than one that says it cannot.
///
/// In working-tree mode the panel becomes the Commit Panel (`WorkingTreePanel`): the
/// unstaged and staged lists, and the Commit and Stash forms. Both faces share one
/// stack, so the panel has one minimum size whichever is showing.
/// It never touches the repository itself — it says what the reader asked for and
/// the window, which owns the session, carries it out.
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
    /// The commit's header is known before its files are: the list shows skeleton
    /// rows until `showCommit` brings them.
    void setFilesLoading(bool loading);
    bool filesLoading() const;
    /// Skeleton rows in the Commit Panel while the status is being read.
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

    /// Re-applies the theme's sizes and colours to the bits that are not styled
    /// by the global stylesheet: the avatar's side and the dim labels. Called by
    /// the window after a theme or zoom change.
    void refreshTheme();

    /// Path of the selected file, or empty when a folder or nothing is selected.
    QString selectedPath() const;
    /// Every selected file row's path, for staging a multi-selection at once.
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
    /// The "Sign this commit" switch, and its value. The panel only reports it; the
    /// window persists the choice and reads it back when the commit is made.
    bool signCommit() const;
    void setSignCommit(bool on);
    /// Puts the caret in the message field, for the command palette's "Go to
    /// Commit Message" — a keyboard way to start writing without a mouse.
    void focusCommitMessage();
    /// Selects the first file, which is what the diff viewer should show when a
    /// commit is picked in the graph.
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
    /// The Stash form: stash every change, untracked files too, under `message`.
    void stashAllRequested(QString message);
    /// The reader toggled "Sign". The window writes `commit.gpgsign`, so the choice
    /// survives the commit and is visible in Preferences too.
    void signChanged(bool on);
    /// The reader asked to credit co-authors: the window opens the picker and writes
    /// the result back through setCommitMessage, so the panel stays free of git.
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
