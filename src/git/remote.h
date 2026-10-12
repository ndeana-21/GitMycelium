#pragma once

#include <string>
#include <vector>

namespace gk {

struct RemoteInfo {
    std::string name;
    std::string fetchUrl;
    std::string pushUrl;
};

/// Outcome of a network operation run through the `git` binary.
struct SyncResult {
    bool ok = false;
    int exitCode = -1;
    /// Merged stdout+stderr exactly as git printed it.
    std::string output;
};

/// How a pull reconciles a diverged branch.
///
/// `FfOnly` refuses rather than create a commit nobody asked for and is the
/// default the toolbar starts on; `Merge` is git's own default; `Rebase` replays
/// the local commits on top of the fetched ones.
enum class PullMode { FfOnly, Merge, Rebase };

/// How a push treats a remote that has moved.
///
/// `ForceWithLease` overwrites the remote branch, but only while its
/// remote-tracking ref still points where we last saw it — git refuses the moment
/// somebody else pushed, so it cannot silently discard work we never read. It is
/// deliberately not `--force`.
enum class PushMode { Normal, ForceWithLease };

/// Fetch / pull / push, delegated to the `git` binary rather than libgit2; the CLI
/// honours the user's credential helpers, SSH agent, proxy settings, hooks and LFS
/// filters, which libgit2 would silently ignore.
class RemoteOps {
public:
    explicit RemoteOps(std::string workDir);

    std::vector<RemoteInfo> remotes() const;

    SyncResult fetch(const std::string& remote, bool prune) const;
    /// Every remote in one call (`git fetch --all`).
    SyncResult fetchAll(bool prune) const;
    SyncResult pull(const std::string& remote, const std::string& branch, PullMode mode) const;
    SyncResult push(const std::string& remote, const std::string& branch, bool setUpstream,
                    PushMode mode = PushMode::Normal) const;
    /// `refs/tags/<tag>` explicitly: a bare `push <tag>` can be read as a branch
    /// name when a branch and a tag share it.
    SyncResult pushTag(const std::string& remote, const std::string& tag) const;
    SyncResult pushTags(const std::string& remote) const;
    /// `git push <remote> :refs/tags/<tag>` — the colon form deletes the ref on the
    /// remote.
    SyncResult deleteRemoteTag(const std::string& remote, const std::string& tag) const;

    const std::string& workDir() const { return m_workDir; }

private:
    std::string m_workDir;
};

} // namespace gk
