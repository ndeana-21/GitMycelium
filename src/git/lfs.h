#pragma once

#include "git/remote.h"

#include <string>
#include <vector>

namespace gk {

struct LfsFile {
    /// The sha256 the pointer names. Short (10 hex) by default; the parser keeps
    /// whatever git-lfs printed.
    std::string oid;
    std::string path;
    /// `*` — the object is present locally; `-` — only the pointer is.
    bool objectPresent = true;
};

struct LfsPattern {
    std::string pattern;
    /// ".gitattributes", or `.git/info/attributes`.
    std::string source;
};

enum class LfsTask { Fetch, Pull, Push, PushAll, Prune, Checkout, PruneDryRun };

const char* lfsTaskName(LfsTask task);
bool lfsTaskFromName(const std::string& name, LfsTask* task);

/// Git LFS, delegated to the `git-lfs` binary.
///
/// The content of an LFS file is substituted by external filters, and the transfer
/// speaks the LFS protocol through the same credential helpers, SSH agent and proxy
/// the ordinary fetch does. A client that read the blob directly would show the
/// pointer — the "file looks empty" bug.
///
/// `git-lfs` is optional. Every method tolerates it being absent and returns git's
/// own "not a git command" output rather than throwing.
///
/// None of these move a ref, so they go through `guarded`, never the operation log.
class GitLfsOps {
public:
    explicit GitLfsOps(std::string workDir);

    /// True when `git lfs version` succeeds.
    bool available() const;
    /// The first line of `git lfs version`, or empty when it is not installed.
    std::string version() const;
    /// True while the LFS clean filter is configured.
    bool configured() const;

    std::vector<LfsPattern> trackedPatterns() const;
    std::vector<LfsFile> files() const;
    /// `git lfs status`, verbatim.
    std::string statusReport() const;

    /// `git lfs install --local`: writes `filter.lfs.*` into the repository's config
    /// and the hooks into `.git/hooks`. Local on purpose — a client should not
    /// rewrite the user's global git config.
    SyncResult install() const;
    /// `git lfs track <pattern>`: appends the pattern to `.gitattributes`. The file
    /// is left unstaged; committing it is the reader's.
    SyncResult track(const std::string& pattern) const;
    SyncResult untrack(const std::string& pattern) const;
    /// `git lfs checkout`: materialises the content of the tracked files from
    /// `.git/lfs/objects`.
    SyncResult checkout() const;
    /// `git lfs prune`: removes unreferenced objects from `.git/lfs/objects`.
    SyncResult prune(bool dryRun) const;

    SyncResult fetch(const std::string& remote) const;
    SyncResult pull(const std::string& remote) const;
    SyncResult push(const std::string& remote, const std::string& ref) const;
    SyncResult pushAll(const std::string& remote) const;

    const std::string& workDir() const { return m_workDir; }

private:
    std::string m_workDir;
};

/// Parses `git lfs ls-files` output: `<oid> <status> <path>`, one per line, where
/// status is `*` (object present) or `-` (pointer only). The path may contain
/// spaces, so only the first two fields are split off.
std::vector<LfsFile> parseLfsLsFiles(const std::string& output);

/// Parses `git lfs track` output, which lists tracked patterns followed by excluded
/// ones under two headers.
std::vector<LfsPattern> parseLfsTracked(const std::string& output);

} // namespace gk
