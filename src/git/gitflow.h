#pragma once

#include "git/remote.h"

#include <string>

namespace gk {

/// The branch families Gitflow knows. `Support` exists in the gitflow config but
/// has no start/finish automation here, so it is refused with a sentence rather
/// than half-done.
enum class GitFlowKind { Feature, Release, Hotfix, Bugfix, Support };

const char* gitFlowKindName(GitFlowKind kind);
bool gitFlowKindFromName(const std::string& name, GitFlowKind* kind);

struct GitFlowConfig {
    std::string master = "master";
    std::string develop = "develop";
    std::string prefixFeature = "feature/";
    std::string prefixRelease = "release/";
    std::string prefixHotfix = "hotfix/";
    std::string prefixBugfix = "bugfix/";
    std::string prefixSupport = "support/";
};

/// The gitflow workflow, built on the `git` binary.
///
/// `finish` is a sequence of checkouts, `--no-ff` merges and a branch deletion, and
/// the `--no-ff` matters: a fast-forward would erase the branch point the workflow
/// exists to keep.
///
/// These move refs and switch the working tree, but they are a *sequence*: the
/// operation log restores one pointer change set, and a multi-step workflow that
/// stopped halfway is better left to the reflog, so they go through `guarded`, never
/// recorded.
class GitFlowOps {
public:
    explicit GitFlowOps(std::string workDir);

    /// True while `gitflow.branch.master` is set, which is what `git flow init` writes.
    bool initialized() const;
    /// The configured names and prefixes, with gitflow's own defaults for the keys
    /// that are unset.
    GitFlowConfig config() const;

    /// Writes the config and creates `develop` from the master branch when it does
    /// not exist yet. Starting from a repository that already used gitflow is not an
    /// error: the config is just rewritten.
    SyncResult init(const GitFlowConfig& config) const;

    std::string branchName(GitFlowKind kind, const std::string& name) const;

    /// Creates the topic branch from the right base and checks it out. Feature,
    /// release and bugfix branch from `develop`; hotfix from the master branch.
    SyncResult start(GitFlowKind kind, const std::string& name) const;

    /// Merges the topic branch back, `--no-ff`, into the branches the kind finishes
    /// into, tags it when a version is given, and deletes it. Feature and bugfix
    /// finish into `develop`; release and hotfix into the master branch *and*
    /// `develop`.
    SyncResult finish(GitFlowKind kind, const std::string& name, const std::string& version) const;

    const std::string& workDir() const { return m_workDir; }

private:
    std::string configValue(const std::string& key, const std::string& fallback) const;

    std::string m_workDir;
};

} // namespace gk
