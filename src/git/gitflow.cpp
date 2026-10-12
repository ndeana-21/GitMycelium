#include "git/gitflow.h"

#include "git/cli.h"

#include <utility>
#include <vector>

namespace gk {
namespace {

SyncResult toResult(const ProcessResult& process) {
    SyncResult result;
    result.ok = process.exitCode == 0;
    result.exitCode = process.exitCode;
    result.output = process.out + process.err;
    return result;
}

/// Appends `next` to `into`, keeping `ok` false once anything failed. The workflow
/// stops at the first failure, so a later step never runs on a tree the earlier one
/// did not prepare.
void append(SyncResult& into, const SyncResult& next) {
    into.output += next.output;
    into.exitCode = next.exitCode;
    into.ok = into.ok && next.ok;
}

} // namespace

const char* gitFlowKindName(GitFlowKind kind) {
    switch (kind) {
    case GitFlowKind::Feature: return "feature";
    case GitFlowKind::Release: return "release";
    case GitFlowKind::Hotfix: return "hotfix";
    case GitFlowKind::Bugfix: return "bugfix";
    case GitFlowKind::Support: return "support";
    }
    return "feature";
}

bool gitFlowKindFromName(const std::string& name, GitFlowKind* kind) {
    if (name == "feature") {
        *kind = GitFlowKind::Feature;
        return true;
    }
    if (name == "release") {
        *kind = GitFlowKind::Release;
        return true;
    }
    if (name == "hotfix") {
        *kind = GitFlowKind::Hotfix;
        return true;
    }
    if (name == "bugfix") {
        *kind = GitFlowKind::Bugfix;
        return true;
    }
    if (name == "support") {
        *kind = GitFlowKind::Support;
        return true;
    }
    return false;
}

GitFlowOps::GitFlowOps(std::string workDir) : m_workDir(std::move(workDir)) {}

std::string GitFlowOps::configValue(const std::string& key, const std::string& fallback) const {
    const ProcessResult run = runGit({"config", "--get", key}, m_workDir);
    if (run.exitCode != 0) {
        return fallback;
    }
    std::string value = run.out;
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value.empty() ? fallback : value;
}

bool GitFlowOps::initialized() const {
    const ProcessResult run = runGit({"config", "--get", "gitflow.branch.master"}, m_workDir);
    return run.exitCode == 0;
}

GitFlowConfig GitFlowOps::config() const {
    GitFlowConfig out;
    out.master = configValue("gitflow.branch.master", out.master);
    out.develop = configValue("gitflow.branch.develop", out.develop);
    out.prefixFeature = configValue("gitflow.prefix.feature", out.prefixFeature);
    out.prefixRelease = configValue("gitflow.prefix.release", out.prefixRelease);
    out.prefixHotfix = configValue("gitflow.prefix.hotfix", out.prefixHotfix);
    out.prefixBugfix = configValue("gitflow.prefix.bugfix", out.prefixBugfix);
    out.prefixSupport = configValue("gitflow.prefix.support", out.prefixSupport);
    return out;
}

SyncResult GitFlowOps::init(const GitFlowConfig& config) const {
    SyncResult result;
    result.ok = true;
    const std::vector<std::pair<std::string, std::string>> settings{
        {"gitflow.branch.master", config.master},
        {"gitflow.branch.develop", config.develop},
        {"gitflow.prefix.feature", config.prefixFeature},
        {"gitflow.prefix.release", config.prefixRelease},
        {"gitflow.prefix.hotfix", config.prefixHotfix},
        {"gitflow.prefix.bugfix", config.prefixBugfix},
        {"gitflow.prefix.support", config.prefixSupport},
    };
    for (const auto& [key, value] : settings) {
        append(result, toResult(runGit({"config", key, value}, m_workDir)));
        if (!result.ok) {
            return result;
        }
    }
    // The develop branch is the whole point of the initialisation; create it from
    // the master branch when it is missing rather than leaving a half-configured
    // repository.
    const ProcessResult exists =
        runGit({"rev-parse", "--verify", "--quiet", "refs/heads/" + config.develop}, m_workDir);
    if (exists.exitCode != 0) {
        append(result, toResult(runGit({"branch", config.develop, config.master}, m_workDir)));
    }
    return result;
}

std::string GitFlowOps::branchName(GitFlowKind kind, const std::string& name) const {
    const GitFlowConfig cfg = config();
    switch (kind) {
    case GitFlowKind::Feature: return cfg.prefixFeature + name;
    case GitFlowKind::Release: return cfg.prefixRelease + name;
    case GitFlowKind::Hotfix: return cfg.prefixHotfix + name;
    case GitFlowKind::Bugfix: return cfg.prefixBugfix + name;
    case GitFlowKind::Support: return cfg.prefixSupport + name;
    }
    return name;
}

SyncResult GitFlowOps::start(GitFlowKind kind, const std::string& name) const {
    if (kind == GitFlowKind::Support) {
        return SyncResult{false, -1, "gitflow support branches have no start/finish here\n"};
    }
    const GitFlowConfig cfg = config();
    const std::string base = kind == GitFlowKind::Hotfix ? cfg.master : cfg.develop;
    const std::string branch = branchName(kind, name);
    return toResult(runGit({"checkout", "-b", branch, base}, m_workDir));
}

SyncResult GitFlowOps::finish(GitFlowKind kind, const std::string& name,
                              const std::string& version) const {
    if (kind == GitFlowKind::Support) {
        return SyncResult{false, -1, "gitflow support branches have no start/finish here\n"};
    }
    const GitFlowConfig cfg = config();
    const std::string branch = branchName(kind, name);

    SyncResult result;
    result.ok = true;
    const auto step = [&](const std::vector<std::string>& args) {
        if (!result.ok) {
            return;
        }
        append(result, toResult(runGit(args, m_workDir)));
    };

    // Feature and bugfix land in develop only; release and hotfix land in the
    // master branch first, get their tag, and are then merged back into develop.
    const bool toMaster = kind == GitFlowKind::Release || kind == GitFlowKind::Hotfix;
    if (toMaster) {
        step({"checkout", cfg.master});
        step({"merge", "--no-ff", "--no-edit", branch});
        if (result.ok && !version.empty()) {
            step({"tag", "-a", version, "-m", "Release " + version});
        }
    }
    step({"checkout", cfg.develop});
    step({"merge", "--no-ff", "--no-edit", branch});
    if (result.ok) {
        step({"branch", "-d", branch});
    }
    return result;
}

} // namespace gk
