#pragma once

#include <string>
#include <vector>

namespace gk {

struct RemoteInfo {
    std::string name;
    std::string fetchUrl;
    std::string pushUrl;
};

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

/// How a push treats a remote that has moved. `Force` is plain `--force`: the
/// remote branch becomes ours whatever it held. `--force-with-lease` refused in most
/// real cases (a fetch moves the lease without anyone reading the work), so the
/// window asks once and overwrites.
enum class PushMode { Normal, Force };

/// Fetch / pull / push, delegated to the `git` binary rather than libgit2; the CLI
/// honours the user's credential helpers, SSH agent, proxy settings, hooks and LFS
/// filters, which libgit2 would silently ignore.
class RemoteOps {
public:
    explicit RemoteOps(std::string workDir);

    std::vector<RemoteInfo> remotes() const;

    SyncResult fetch(const std::string& remote, bool prune) const;
    SyncResult fetchAll(bool prune) const;
    SyncResult pull(const std::string& remote, const std::string& branch, PullMode mode) const;
    SyncResult push(const std::string& remote, const std::string& branch, bool setUpstream,
                    PushMode mode = PushMode::Normal) const;
    /// `refs/tags/<tag>` explicitly: a bare `push <tag>` can be read as a branch
    /// name when a branch and a tag share it.
    SyncResult pushTag(const std::string& remote, const std::string& tag) const;
    SyncResult pushTags(const std::string& remote) const;
    SyncResult deleteRemoteTag(const std::string& remote, const std::string& tag) const;

    const std::string& workDir() const { return m_workDir; }

private:
    std::string m_workDir;
};

} // namespace gk
