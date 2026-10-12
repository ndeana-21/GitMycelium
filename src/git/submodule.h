#pragma once

#include "core/types.h"
#include "git/remote.h"

#include <string>
#include <vector>

namespace gk {

/// Submodules, delegated to the `git` binary.
///
/// libgit2 has a submodule API, but it is narrower than git's here: adding with a
/// name, syncing the URL into `.git/config`, deinit and the recursive update are all
/// `git submodule` behaviours, and a reimplementation would only approximate them.
class SubmoduleOps {
public:
    explicit SubmoduleOps(std::string workDir);

    /// `git submodule status --recursive`, joined with the `.gitmodules` section
    /// that names each path, ordered by path. Nested entries carry their parent.
    /// A repository without submodules yields an empty list, not an error.
    std::vector<SubmoduleEntry> list() const;

    /// `git submodule add`. `branch` empty leaves the tracked branch unset; an
    /// empty `path` lets git derive it from the URL.
    SyncResult add(const std::string& url, const std::string& path, const std::string& branch,
                   bool force) const;
    /// `git submodule deinit`. git refuses a submodule with local changes unless
    /// `force` is set.
    SyncResult deinit(const std::string& path, bool force) const;
    /// `git submodule update`. `initialize` is git's `--init`; `remote` is
    /// `--remote` and needs `submodule.<name>.branch`. An empty `paths` means every
    /// submodule.
    SyncResult update(const std::vector<std::string>& paths, bool initialize, bool remote,
                      bool recursive) const;
    /// `git submodule sync`: copies the `.gitmodules` URL into `.git/config`.
    SyncResult sync(const std::vector<std::string>& paths, bool recursive) const;

    const std::string& workDir() const { return m_workDir; }

private:
    std::string m_workDir;
};

} // namespace gk
