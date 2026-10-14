#pragma once

#include "git/remote.h"

#include <string>
#include <vector>

namespace gk {

/// One entry of the stash list.
///
/// `index` is the position it had when the list was read — `stash@{index}` — but
/// it is *not* what an operation should be addressed by: dropping one entry
/// renumbers every entry below it. `oid` is the anchor instead.
struct StashEntry {
    int index = 0;
    std::string oid;
    std::string message; ///< reflog subject, e.g. "WIP on main: ..."
};

class StashOps {
public:
    explicit StashOps(std::string workDir);

    std::vector<StashEntry> list() const;

    SyncResult push(const std::string& message, bool includeUntracked,
                    const std::vector<std::string>& paths) const;

    /// An empty `stash` means the top of the list (`stash@{0}`). A non-empty value
    /// is an oid from `list()`; it is resolved to the entry's *current* index at
    /// call time, because `git stash pop/drop` accept only `stash@{n}`.
    SyncResult apply(const std::string& stash) const;
    SyncResult pop(const std::string& stash) const;
    SyncResult drop(const std::string& stash) const;
    SyncResult branch(const std::string& stash, const std::string& name) const;

    const std::string& workDir() const { return m_workDir; }

private:
    /// The `stash@{n}` for an oid, resolved against the list as it is right now.
    /// Empty when no entry carries that oid. An input already written as
    /// `stash@{n}` is passed through.
    std::string selectorFor(const std::string& stash) const;

    std::string m_workDir;
};

} // namespace gk
