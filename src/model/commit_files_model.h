#pragma once

#include "core/types.h"

#include <QStandardItemModel>
#include <QString>
#include <QStringList>

#include <vector>

namespace gk {

/// The data is kept once and rebuilt into rows on every shape switch: deriving a
/// commit's few hundred files again is cheaper than keeping two models in step.
class CommitFilesModel : public QStandardItemModel {
    Q_OBJECT

public:
    enum ItemType { FileItem = 0, FolderItem, GroupItem };
    enum Mode { PathMode, TreeMode };
    enum Sort { PathAscending, PathDescending, ByStatus };
    enum Role {
        PathRole = Qt::UserRole, ///< for a folder, its prefix
        StatusRole,
        ItemTypeRole,
        PrefixRole,
        StagedRole,              ///< working tree only
        SubmoduleRole,           ///< true when the row is a gitlink (a pointer change)
        SubmoduleTipRole,        ///< recorded → checked-out, for the tooltip
    };

    explicit CommitFilesModel(QObject* parent = nullptr);

    void setFiles(const std::vector<ChangedFile>& files);
    /// The files that are not committed yet, each tagged with whether it is staged.
    /// Listed under Staged / Unstaged: a working tree has no folder shape worth
    /// drawing, and the question about it is always "did I stage that".
    void setWorkingTree(const std::vector<FileChange>& changes);
    /// One side of the working tree, with no group rows, so it takes the Path and
    /// Tree shapes like a commit does. The Commit Panel shows each side in its own
    /// list.
    void setWorkingTreeSide(const std::vector<FileChange>& changes, bool staged);
    bool workingTree() const { return m_workingTree; }
    void setMode(Mode mode);
    void setSort(Sort sort);
    void setFolderFilter(const QString& prefix);
    QString folderFilter() const { return m_folderFilter; }

    Mode mode() const { return m_mode; }
    Sort sort() const { return m_sort; }

    int countOf(ChangeKind kind) const;
    int fileCount() const { return static_cast<int>(m_files.size()); }
    bool hasUnstaged() const;

    static bool isFile(const QModelIndex& index);
    static QString pathOf(const QModelIndex& index);
    static ChangeKind statusOf(const QModelIndex& index);
    static bool isStaged(const QModelIndex& index);

private:
    void assignWorkingTree(const std::vector<FileChange>& changes);
    void rebuild();

    std::vector<ChangedFile> m_files;
    std::vector<bool> m_staged; ///< parallel to m_files, working tree only
    /// Parallel to m_files: true when the row is a submodule pointer change, and the
    /// recorded → checked-out line for its tooltip. A gitlink is not a file, and the
    /// two oids are the whole point of showing it.
    std::vector<bool> m_submodule;
    std::vector<QString> m_submoduleTip;
    bool m_workingTree = false;
    bool m_grouped = false;
    QString m_folderFilter;
    Mode m_mode = PathMode;
    Sort m_sort = PathAscending;
};

} // namespace gk
