#pragma once

#include "git/remote.h"

#include <string>
#include <vector>

namespace gk {

/// rerere, delegated to the `git` binary.
///
/// The feature is entirely a matter of the binary's own state (`.git/rr-cache`,
/// the operation's metadata under `.git`), which libgit2 has no access to.
///
/// `status` and `remaining` are the two halves of one answer: `status` lists the
/// conflicted paths rerere tracks, `remaining` the ones it did *not* resolve from
/// memory.
class RerereOps {
public:
    explicit RerereOps(std::string workDir);

    /// `git config --bool --get rerere.enabled`. Off unless it says true.
    bool enabled() const;
    bool autoupdate() const;
    void setEnabled(bool on) const;
    void setAutoupdate(bool on) const;

    /// Paths of the current conflicts that rerere knows about. Empty when rerere
    /// is disabled — `git rerere status` refuses to answer then.
    std::vector<std::string> statusPaths() const;
    /// Conflicted paths rerere has *not* resolved from memory. A path that appears
    /// in `statusPaths()` but not here was applied from a recorded resolution.
    std::vector<std::string> remainingPaths() const;

    /// `git rerere`: applies every recorded resolution it can to the working tree.
    SyncResult apply() const;
    /// `git rerere forget <path>`: drops the recorded resolution for a path, so the
    /// next conflict in it is presented fresh.
    SyncResult forget(const std::string& path) const;

    const std::string& workDir() const { return m_workDir; }

private:
    std::vector<std::string> linesFor(const std::vector<std::string>& args) const;

    std::string m_workDir;
};

} // namespace gk
