#include "git/stash.h"

#include "git/cli.h"

#include <cstdlib>
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

int indexFromSelector(const std::string& selector, int fallback) {
    const std::size_t open = selector.find('{');
    const std::size_t close = selector.find('}');
    if (open == std::string::npos || close == std::string::npos || close < open + 2) {
        return fallback;
    }
    const std::string digits = selector.substr(open + 1, close - open - 1);
    char* end = nullptr;
    const long value = std::strtol(digits.c_str(), &end, 10);
    if (end == digits.c_str() || value < 0) {
        return fallback;
    }
    return static_cast<int>(value);
}

/// The result of asking for a stash that is no longer in the list. Reported as a
/// failure rather than run, so the caller sees the reason instead of git's
/// confusing "not a stash reference".
SyncResult goneResult() {
    SyncResult result;
    result.ok = false;
    result.exitCode = 1;
    result.output = "the stash is no longer in the list";
    return result;
}

} // namespace

StashOps::StashOps(std::string workDir) : m_workDir(std::move(workDir)) {}

std::vector<StashEntry> StashOps::list() const {
    std::vector<StashEntry> out;
    // %gd is "stash@{n}" and %H is hex — neither contains a space — so the first two
    // spaces split the fields and the rest is the message, spaces and all.
    const ProcessResult process = runGit({"stash", "list", "--format=%gd %H %gs"}, m_workDir);
    if (process.exitCode != 0) {
        return out;
    }

    std::istringstream stream(process.out);
    std::string line;
    int fallback = 0;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        const std::size_t first = line.find(' ');
        if (first == std::string::npos) {
            continue;
        }
        const std::size_t second = line.find(' ', first + 1);
        if (second == std::string::npos) {
            continue;
        }
        StashEntry entry;
        const std::string selector = line.substr(0, first);
        entry.oid = line.substr(first + 1, second - first - 1);
        entry.message = line.substr(second + 1);
        entry.index = indexFromSelector(selector, fallback);
        ++fallback;
        out.push_back(std::move(entry));
    }
    return out;
}

SyncResult StashOps::push(const std::string& message, bool includeUntracked,
                          const std::vector<std::string>& paths) const {
    std::vector<std::string> args{"stash", "push"};
    if (includeUntracked) {
        args.push_back("--include-untracked");
    }
    if (!message.empty()) {
        args.push_back("-m");
        args.push_back(message);
    }
    if (!paths.empty()) {
        args.push_back("--");
        for (const std::string& path : paths) {
            args.push_back(path);
        }
    }
    return toSyncResult(runGit(args, m_workDir));
}

std::string StashOps::selectorFor(const std::string& stash) const {
    if (stash.empty()) {
        return "stash@{0}";
    }
    if (stash.rfind("stash@{", 0) == 0) {
        return stash;
    }
    for (const StashEntry& entry : list()) {
        if (entry.oid == stash) {
            return "stash@{" + std::to_string(entry.index) + "}";
        }
    }
    return {};
}

SyncResult StashOps::apply(const std::string& stash) const {
    const std::string selector = selectorFor(stash);
    if (selector.empty()) {
        return goneResult();
    }
    return toSyncResult(runGit({"stash", "apply", selector}, m_workDir));
}

SyncResult StashOps::pop(const std::string& stash) const {
    const std::string selector = selectorFor(stash);
    if (selector.empty()) {
        return goneResult();
    }
    return toSyncResult(runGit({"stash", "pop", selector}, m_workDir));
}

SyncResult StashOps::drop(const std::string& stash) const {
    const std::string selector = selectorFor(stash);
    if (selector.empty()) {
        return goneResult();
    }
    return toSyncResult(runGit({"stash", "drop", selector}, m_workDir));
}

SyncResult StashOps::branch(const std::string& stash, const std::string& name) const {
    const std::string selector = selectorFor(stash);
    if (selector.empty()) {
        return goneResult();
    }
    return toSyncResult(runGit({"stash", "branch", name, selector}, m_workDir));
}

} // namespace gk
