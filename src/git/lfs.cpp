#include "git/lfs.h"

#include "git/cli.h"

#include <sstream>
#include <utility>

namespace gk {
namespace {

SyncResult toResult(const ProcessResult& process) {
    SyncResult result;
    result.ok = process.exitCode == 0;
    result.exitCode = process.exitCode;
    result.output = process.out + process.err;
    return result;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(line);
    }
    return out;
}

std::string firstLine(const std::string& text) {
    const std::size_t nl = text.find('\n');
    return text.substr(0, nl == std::string::npos ? text.size() : nl);
}

std::string trim(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

} // namespace

std::vector<LfsFile> parseLfsLsFiles(const std::string& output) {
    std::vector<LfsFile> files;
    for (const std::string& line : splitLines(output)) {
        if (line.empty()) {
            continue;
        }
        const std::size_t firstSpace = line.find(' ');
        if (firstSpace == std::string::npos || firstSpace + 2 >= line.size()) {
            continue;
        }
        LfsFile file;
        file.oid = line.substr(0, firstSpace);
        file.objectPresent = line[firstSpace + 1] != '-';
        // The status is a single character followed by a space; everything after is
        // the path, which may itself contain spaces.
        file.path = line.substr(firstSpace + 3);
        if (!file.path.empty()) {
            files.push_back(std::move(file));
        }
    }
    return files;
}

std::vector<LfsPattern> parseLfsTracked(const std::string& output) {
    std::vector<LfsPattern> patterns;
    bool inTracked = false;
    for (const std::string& line : splitLines(output)) {
        const std::string trimmed = trim(line);
        if (trimmed == "Listing tracked patterns") {
            inTracked = true;
            continue;
        }
        if (trimmed == "Listing excluded patterns") {
            inTracked = false;
            continue;
        }
        if (!inTracked || trimmed.empty()) {
            continue;
        }
        // `<pattern> (<source>)`; the source is the last parenthesised token.
        const std::size_t open = trimmed.rfind(" (");
        LfsPattern entry;
        if (open != std::string::npos && trimmed.back() == ')') {
            entry.pattern = trimmed.substr(0, open);
            entry.source = trimmed.substr(open + 2, trimmed.size() - open - 3);
        } else {
            entry.pattern = trimmed;
        }
        if (!entry.pattern.empty()) {
            patterns.push_back(std::move(entry));
        }
    }
    return patterns;
}

const char* lfsTaskName(LfsTask task) {
    switch (task) {
    case LfsTask::Fetch: return "fetch";
    case LfsTask::Pull: return "pull";
    case LfsTask::Push: return "push";
    case LfsTask::PushAll: return "push-all";
    case LfsTask::Prune: return "prune";
    case LfsTask::Checkout: return "checkout";
    case LfsTask::PruneDryRun: return "prune-dry-run";
    }
    return "fetch";
}

bool lfsTaskFromName(const std::string& name, LfsTask* task) {
    if (name == "fetch") {
        *task = LfsTask::Fetch;
    } else if (name == "pull") {
        *task = LfsTask::Pull;
    } else if (name == "push") {
        *task = LfsTask::Push;
    } else if (name == "push-all") {
        *task = LfsTask::PushAll;
    } else if (name == "prune") {
        *task = LfsTask::Prune;
    } else if (name == "checkout") {
        *task = LfsTask::Checkout;
    } else if (name == "prune-dry-run") {
        *task = LfsTask::PruneDryRun;
    } else {
        return false;
    }
    return true;
}

GitLfsOps::GitLfsOps(std::string workDir) : m_workDir(std::move(workDir)) {}

bool GitLfsOps::available() const {
    return runGit({"lfs", "version"}, m_workDir).exitCode == 0;
}

std::string GitLfsOps::version() const {
    const ProcessResult run = runGit({"lfs", "version"}, m_workDir);
    if (run.exitCode != 0) {
        return {};
    }
    return firstLine(run.out);
}

bool GitLfsOps::configured() const {
    return runGit({"config", "--get", "filter.lfs.clean"}, m_workDir).exitCode == 0;
}

std::vector<LfsPattern> GitLfsOps::trackedPatterns() const {
    const ProcessResult run = runGit({"lfs", "track"}, m_workDir);
    if (run.exitCode != 0) {
        return {};
    }
    return parseLfsTracked(run.out);
}

std::vector<LfsFile> GitLfsOps::files() const {
    const ProcessResult run = runGit({"lfs", "ls-files"}, m_workDir);
    if (run.exitCode != 0) {
        return {};
    }
    return parseLfsLsFiles(run.out);
}

std::string GitLfsOps::statusReport() const {
    const ProcessResult run = runGit({"lfs", "status"}, m_workDir);
    return run.exitCode == 0 ? run.out : (run.out + run.err);
}

SyncResult GitLfsOps::install() const {
    // --local: the filter config and hooks land in this repository, not in the
    // reader's global git config.
    return toResult(runGit({"lfs", "install", "--local"}, m_workDir));
}

SyncResult GitLfsOps::track(const std::string& pattern) const {
    return toResult(runGit({"lfs", "track", pattern}, m_workDir));
}

SyncResult GitLfsOps::untrack(const std::string& pattern) const {
    return toResult(runGit({"lfs", "untrack", pattern}, m_workDir));
}

SyncResult GitLfsOps::checkout() const {
    return toResult(runGit({"lfs", "checkout"}, m_workDir));
}

SyncResult GitLfsOps::prune(bool dryRun) const {
    if (dryRun) {
        return toResult(runGit({"lfs", "prune", "--dry-run"}, m_workDir));
    }
    // --force: a connected client asks once in its own dialog; git-lfs has nobody
    // to type "y" here, and a prune that waits for input waits forever.
    return toResult(runGit({"lfs", "prune", "--force"}, m_workDir));
}

SyncResult GitLfsOps::fetch(const std::string& remote) const {
    return toResult(runGit({"lfs", "fetch", remote}, m_workDir));
}

SyncResult GitLfsOps::pull(const std::string& remote) const {
    return toResult(runGit({"lfs", "pull", remote}, m_workDir));
}

SyncResult GitLfsOps::push(const std::string& remote, const std::string& ref) const {
    // `git lfs push <remote>` alone is an error ("at least one ref must be
    // supplied"); with no ref asked for, all of them is the honest reading.
    if (ref.empty()) {
        return pushAll(remote);
    }
    return toResult(runGit({"lfs", "push", remote, ref}, m_workDir));
}

SyncResult GitLfsOps::pushAll(const std::string& remote) const {
    return toResult(runGit({"lfs", "push", "--all", remote}, m_workDir));
}

} // namespace gk
