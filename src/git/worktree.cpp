#include "git/worktree.h"

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

/// The last path component, without a trailing separator. The display name of a
/// worktree is its directory, because that is the part a reader recognises.
std::string baseName(const std::string& path) {
    std::string trimmed = path;
    while (trimmed.size() > 1 && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    const std::size_t slash = trimmed.find_last_of('/');
    return slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
}

/// Compares two directories without falling over a trailing separator. Both sides
/// come from git (absolute), so a raw normalisation is enough; the window may
/// pass a canonical path it already resolved.
std::string normalisePath(const std::string& path) {
    std::string out = path;
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

} // namespace

WorktreeOps::WorktreeOps(std::string workDir) : m_workDir(std::move(workDir)) {}

std::vector<WorktreeEntry> WorktreeOps::list(const std::string& currentPath) const {
    std::vector<WorktreeEntry> out;
    const ProcessResult process = runGit({"worktree", "list", "--porcelain"}, m_workDir);
    if (process.exitCode != 0) {
        return out;
    }

    const std::string current = normalisePath(currentPath);
    std::istringstream stream(process.out);
    std::string line;
    WorktreeEntry entry;
    bool started = false;
    const auto flush = [&] {
        if (started) {
            out.push_back(std::move(entry));
            entry = WorktreeEntry{};
            started = false;
        }
    };

    // Blank line between entries; each entry is `key` or `key value`. Values are
    // the rest of the line, because a path or a lock reason can contain spaces and
    // there is nothing after it to get in the way.
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            flush();
            continue;
        }
        const std::size_t space = line.find(' ');
        const std::string key = space == std::string::npos ? line : line.substr(0, space);
        const std::string value = space == std::string::npos ? std::string() : line.substr(space + 1);

        if (key == "worktree") {
            flush();
            entry.path = value;
            entry.name = baseName(value);
            entry.current = !current.empty() && normalisePath(value) == current;
            started = true;
        } else if (key == "HEAD") {
            entry.head = value;
        } else if (key == "branch") {
            entry.branch = value;
        } else if (key == "detached") {
            entry.detached = true;
        } else if (key == "bare") {
            entry.bare = true;
        } else if (key == "locked") {
            entry.locked = true;
            entry.lockReason = value;
        } else if (key == "prunable") {
            entry.prunable = true;
        }
    }
    flush();
    return out;
}

SyncResult WorktreeOps::add(const std::string& path, const std::string& branch,
                            bool createBranch, const std::string& startPoint) const {
    std::vector<std::string> args{"worktree", "add"};
    if (createBranch) {
        args.push_back("-b");
        args.push_back(branch);
        args.push_back(path);
        if (!startPoint.empty()) {
            args.push_back(startPoint);
        }
    } else {
        args.push_back(path);
        if (!branch.empty()) {
            args.push_back(branch);
        }
    }
    return toSyncResult(runGit(args, m_workDir));
}

SyncResult WorktreeOps::remove(const std::string& path, bool force) const {
    std::vector<std::string> args{"worktree", "remove"};
    if (force) {
        args.push_back("--force");
    }
    args.push_back(path);
    return toSyncResult(runGit(args, m_workDir));
}

SyncResult WorktreeOps::lock(const std::string& path, const std::string& reason) const {
    std::vector<std::string> args{"worktree", "lock"};
    if (!reason.empty()) {
        args.push_back("--reason");
        args.push_back(reason);
    }
    args.push_back(path);
    return toSyncResult(runGit(args, m_workDir));
}

SyncResult WorktreeOps::unlock(const std::string& path) const {
    return toSyncResult(runGit({"worktree", "unlock", path}, m_workDir));
}

SyncResult WorktreeOps::prune() const {
    return toSyncResult(runGit({"worktree", "prune"}, m_workDir));
}

} // namespace gk
