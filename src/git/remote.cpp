#include "git/remote.h"

#include "git/cli.h"

#include <sstream>

namespace gk {
namespace {

SyncResult toSyncResult(const ProcessResult& process) {
    SyncResult result;
    result.ok = process.exitCode == 0;
    result.exitCode = process.exitCode;
    result.output = process.out;
    if (!process.err.empty()) {
        if (!result.output.empty() && result.output.back() != '\n') {
            result.output += '\n';
        }
        result.output += process.err;
    }
    return result;
}

bool parseRemoteLine(const std::string& line, RemoteInfo* info) {
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos) {
        return false;
    }
    std::string name = line.substr(0, tab);
    std::string rest = line.substr(tab + 1);

    bool isFetch = true;
    const std::string fetchSuffix = " (fetch)";
    const std::string pushSuffix = " (push)";
    if (rest.size() > pushSuffix.size() &&
        rest.compare(rest.size() - pushSuffix.size(), pushSuffix.size(), pushSuffix) == 0) {
        isFetch = false;
        rest.erase(rest.size() - pushSuffix.size());
    } else if (rest.size() > fetchSuffix.size() &&
               rest.compare(rest.size() - fetchSuffix.size(), fetchSuffix.size(), fetchSuffix) == 0) {
        rest.erase(rest.size() - fetchSuffix.size());
    } else {
        return false;
    }

    if (info->name.empty()) {
        info->name = name;
    }
    if (isFetch) {
        info->fetchUrl = rest;
    } else {
        info->pushUrl = rest;
    }
    return true;
}

} // namespace

RemoteOps::RemoteOps(std::string workDir) : m_workDir(std::move(workDir)) {}

std::vector<RemoteInfo> RemoteOps::remotes() const {
    std::vector<RemoteInfo> out;
    const ProcessResult process = runGit({"remote", "-v"}, m_workDir);
    if (process.exitCode != 0) {
        return out;
    }

    std::istringstream stream(process.out);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        RemoteInfo info;
        if (!parseRemoteLine(line, &info)) {
            continue;
        }
        // `git remote -v` prints the fetch line first, then the push line.
        if (!out.empty() && out.back().name == info.name) {
            out.back().pushUrl = info.pushUrl.empty() ? out.back().pushUrl : info.pushUrl;
            continue;
        }
        out.push_back(std::move(info));
    }
    return out;
}

SyncResult RemoteOps::fetch(const std::string& remote, bool prune) const {
    std::vector<std::string> args{"fetch"};
    if (prune) {
        args.push_back("--prune");
    }
    if (!remote.empty()) {
        args.push_back(remote);
    }
    return toSyncResult(runGit(args, m_workDir));
}

SyncResult RemoteOps::fetchAll(bool prune) const {
    std::vector<std::string> args{"fetch", "--all"};
    if (prune) {
        args.push_back("--prune");
    }
    return toSyncResult(runGit(args, m_workDir));
}

SyncResult RemoteOps::pull(const std::string& remote, const std::string& branch,
                           PullMode mode) const {
    std::vector<std::string> args{"pull"};
    if (mode == PullMode::FfOnly) {
        args.push_back("--ff-only");
    } else if (mode == PullMode::Rebase) {
        args.push_back("--rebase");
    } else {
        // Modern git refuses a divergent `git pull` until the reconciliation is
        // named; "merge" has to say so, or it exits 128 asking for configuration.
        args.push_back("--no-rebase");
    }
    if (!remote.empty()) {
        args.push_back(remote);
        if (!branch.empty()) {
            args.push_back(branch);
        }
    }
    return toSyncResult(runGit(args, m_workDir));
}

SyncResult RemoteOps::push(const std::string& remote, const std::string& branch,
                           bool setUpstream, PushMode mode) const {
    std::vector<std::string> args{"push"};
    if (mode == PushMode::ForceWithLease) {
        // No explicit expected value: git uses the remote-tracking ref as the
        // lease, and refuses if it has moved since we last fetched. That is the
        // whole difference from `--force`.
        args.push_back("--force-with-lease");
    }
    if (setUpstream) {
        args.push_back("--set-upstream");
    }
    args.push_back(remote.empty() ? std::string("origin") : remote);
    if (!branch.empty()) {
        args.push_back(branch);
    }
    return toSyncResult(runGit(args, m_workDir));
}

SyncResult RemoteOps::pushTag(const std::string& remote, const std::string& tag) const {
    const std::string target = remote.empty() ? std::string("origin") : remote;
    return toSyncResult(
        runGit({"push", target, "refs/tags/" + tag}, m_workDir));
}

SyncResult RemoteOps::pushTags(const std::string& remote) const {
    return toSyncResult(
        runGit({"push", remote.empty() ? std::string("origin") : remote, "--tags"}, m_workDir));
}

SyncResult RemoteOps::deleteRemoteTag(const std::string& remote, const std::string& tag) const {
    const std::string target = remote.empty() ? std::string("origin") : remote;
    return toSyncResult(runGit({"push", target, ":refs/tags/" + tag}, m_workDir));
}

} // namespace gk
