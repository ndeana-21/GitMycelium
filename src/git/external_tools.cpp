#include "git/external_tools.h"

#include "git/cli.h"

#include <algorithm>
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

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

} // namespace

ExternalToolOps::ExternalToolOps(std::string workDir) : m_workDir(std::move(workDir)) {}

std::string ExternalToolOps::tool(bool merge) const {
    const std::string key = merge ? "merge.tool" : "diff.tool";
    const ProcessResult process = runGit({"config", "--get", key}, m_workDir);
    if (process.exitCode != 0) {
        return {};
    }
    return trimmed(process.out);
}

bool ExternalToolOps::setTool(bool merge, const std::string& name, std::string* errorOut) const {
    const std::string key = merge ? "merge.tool" : "diff.tool";
    std::vector<std::string> args = {"config", "--local"};
    if (name.empty()) {
        // Unsetting a key that was never set is not a failure: it is how the reader
        // returns to git's default, and saying so would be a lie.
        args.push_back("--unset");
        args.push_back(key);
        runGit(args, m_workDir);
        return true;
    }
    args.push_back(key);
    args.push_back(name);
    const ProcessResult process = runGit(args, m_workDir);
    if (process.exitCode != 0) {
        if (errorOut) {
            *errorOut = process.err.empty() ? process.out : process.err;
        }
        return false;
    }
    return true;
}

std::vector<std::string> ExternalToolOps::availableTools(bool merge) const {
    const ProcessResult process =
        runGit({merge ? "mergetool" : "difftool", "--tool-help"}, m_workDir);
    if (process.exitCode != 0) {
        return {};
    }
    std::vector<std::string> out;
    std::istringstream lines(process.out);
    std::string line;
    while (std::getline(lines, line)) {
        // The tool lines are indented with tabs; the section headers and the prose at
        // the end are not. The name is the first token, the description follows.
        if (line.empty() || line.front() != '\t') {
            continue;
        }
        std::size_t begin = line.find_first_not_of("\t ");
        if (begin == std::string::npos) {
            continue;
        }
        const std::size_t end = line.find_first_of(" \t", begin);
        const std::string name = line.substr(begin, end == std::string::npos ? end : end - begin);
        if (name.empty()) {
            continue;
        }
        if (std::find(out.begin(), out.end(), name) == out.end()) {
            out.push_back(name);
        }
    }
    return out;
}

SyncResult ExternalToolOps::runDiff(const std::string& base, const std::string& target,
                                    const std::string& path, bool cached) const {
    // `--no-prompt`: there is nobody at a terminal to answer, and a tool asking
    // "launch this / skip it?" would hang the worker forever.
    std::vector<std::string> args{"difftool", "--no-prompt"};
    if (cached) {
        args.push_back("--cached");
    }
    if (!base.empty()) {
        args.push_back(base);
    }
    if (!target.empty()) {
        args.push_back(target);
    }
    if (!path.empty()) {
        args.push_back("--");
        args.push_back(path);
    }
    return toSyncResult(runGit(args, m_workDir));
}

SyncResult ExternalToolOps::runMerge(const std::string& path) const {
    std::vector<std::string> args{"mergetool", "--no-prompt"};
    if (!path.empty()) {
        args.push_back("--");
        args.push_back(path);
    }
    return toSyncResult(runGit(args, m_workDir));
}

} // namespace gk
