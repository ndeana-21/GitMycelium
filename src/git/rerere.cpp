#include "git/rerere.h"

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

} // namespace

RerereOps::RerereOps(std::string workDir) : m_workDir(std::move(workDir)) {}

bool RerereOps::enabled() const {
    const ProcessResult process = runGit({"config", "--bool", "--get", "rerere.enabled"}, m_workDir);
    return process.exitCode == 0 && process.out.rfind("true", 0) == 0;
}

bool RerereOps::autoupdate() const {
    const ProcessResult process =
        runGit({"config", "--bool", "--get", "rerere.autoupdate"}, m_workDir);
    return process.exitCode == 0 && process.out.rfind("true", 0) == 0;
}

void RerereOps::setEnabled(bool on) const {
    runGit({"config", "rerere.enabled", on ? "true" : "false"}, m_workDir);
}

void RerereOps::setAutoupdate(bool on) const {
    runGit({"config", "rerere.autoupdate", on ? "true" : "false"}, m_workDir);
}

std::vector<std::string> RerereOps::linesFor(const std::vector<std::string>& args) const {
    std::vector<std::string> out;
    const ProcessResult process = runGit(args, m_workDir);
    if (process.exitCode != 0) {
        return out;
    }
    std::istringstream stream(process.out);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    return out;
}

std::vector<std::string> RerereOps::statusPaths() const {
    // `git rerere status` answers "rerere is disabled" otherwise, and a prediction
    // from a disabled feature is no prediction at all.
    if (!enabled()) {
        return {};
    }
    return linesFor({"rerere", "status"});
}

std::vector<std::string> RerereOps::remainingPaths() const {
    if (!enabled()) {
        return {};
    }
    return linesFor({"rerere", "remaining"});
}

SyncResult RerereOps::apply() const {
    return toSyncResult(runGit({"rerere"}, m_workDir));
}

SyncResult RerereOps::forget(const std::string& path) const {
    return toSyncResult(runGit({"rerere", "forget", path}, m_workDir));
}

} // namespace gk
