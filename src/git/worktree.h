#pragma once

#include "core/types.h"
#include "git/remote.h"

#include <string>
#include <vector>

namespace gk {

/// Worktrees, delegated to the `git` binary.
///
/// libgit2 has a worktree API, but it is narrower than git's here: creating with a
/// new branch, locking with a reason, and pruning entries whose directory is gone
/// are all `git worktree` behaviours.
class WorktreeOps {
public:
    explicit WorktreeOps(std::string workDir);

    /// `git worktree list --porcelain`, in git's order (the main worktree first).
    /// `currentPath` marks the entry the session was opened on; pass the
    /// repository's own work directory.
    std::vector<WorktreeEntry> list(const std::string& currentPath) const;

    /// `git worktree add`. With `createBranch` the branch is made first (`-b`) at
    /// `startPoint` (HEAD when empty); otherwise `branch` must already exist. An
    /// empty `branch` without `createBranch` makes a detached worktree.
    SyncResult add(const std::string& path, const std::string& branch, bool createBranch,
                   const std::string& startPoint) const;
    /// `git worktree remove`. git refuses a worktree with modifications unless
    /// `force` is set.
    SyncResult remove(const std::string& path, bool force) const;
    SyncResult lock(const std::string& path, const std::string& reason) const;
    SyncResult unlock(const std::string& path) const;
    /// `git worktree prune`: forgets entries whose directory is gone.
    SyncResult prune() const;

    const std::string& workDir() const { return m_workDir; }

private:
    std::string m_workDir;
};

} // namespace gk
