#include "git/sparse.h"

#include "git/cli.h"

#include <algorithm>
#include <sstream>

namespace gk {
namespace {

SyncResult toResult(const ProcessResult& process) {
    SyncResult result;
    result.ok = process.exitCode == 0;
    result.exitCode = process.exitCode;
    result.output = process.out + process.err;
    return result;
}

std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    return out;
}

} // namespace

std::vector<std::string> parseSparseList(const std::string& output) {
    std::vector<std::string> out;
    for (std::string entry : linesOf(output)) {
        // Cone mode prints the cone's directories with a leading slash; the
        // pattern form does not. Drop it either way, so the list is the same shape
        // whether the reader drives cone mode or not.
        if (entry.size() >= 2 && entry.front() == '/' && entry.back() == '/') {
            entry = entry.substr(1, entry.size() - 2);
        } else if (entry.size() >= 2 && entry.front() == '/' && entry.back() != '/') {
            entry = entry.substr(1);
        }
        if (!entry.empty() && entry != "/" && entry != "!" && entry.front() != '!') {
            out.push_back(entry);
        }
    }
    return out;
}

SparseOps::SparseOps(std::string workDir) : m_workDir(std::move(workDir)) {}

bool SparseOps::enabled() const {
    const ProcessResult run = runGit({"config", "--bool", "core.sparseCheckout"}, m_workDir);
    if (run.exitCode != 0) {
        return false;
    }
    std::string value = run.out;
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value == "true";
}

std::vector<std::string> SparseOps::list() const {
    const ProcessResult run = runGit({"sparse-checkout", "list"}, m_workDir);
    if (run.exitCode != 0) {
        // Not an error: sparse checkout that was never enabled has an empty list.
        return {};
    }
    return parseSparseList(run.out);
}

std::vector<std::string> SparseOps::topLevelDirectories() const {
    // HEAD's tree, not the working directory: a directory the cone currently
    // excludes has no files on disk, and the picker still has to offer it.
    const ProcessResult run = runGit({"ls-tree", "-d", "--name-only", "HEAD"}, m_workDir);
    if (run.exitCode != 0) {
        return {};
    }
    std::vector<std::string> dirs = linesOf(run.out);
    std::sort(dirs.begin(), dirs.end());
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    return dirs;
}

SyncResult SparseOps::set(const std::vector<std::string>& dirs) const {
    if (dirs.empty()) {
        return disable();
    }
    std::vector<std::string> args{"sparse-checkout", "set", "--cone"};
    args.insert(args.end(), dirs.begin(), dirs.end());
    return toResult(runGit(args, m_workDir));
}

SyncResult SparseOps::disable() const {
    return toResult(runGit({"sparse-checkout", "disable"}, m_workDir));
}

} // namespace gk
