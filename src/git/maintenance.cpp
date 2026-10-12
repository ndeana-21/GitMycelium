#include "git/maintenance.h"

#include "git/cli.h"

#include <utility>
#include <vector>

namespace gk {
namespace {

std::vector<std::string> commandFor(MaintenanceTask task) {
    switch (task) {
    case MaintenanceTask::Gc:
        return {"gc"};
    case MaintenanceTask::Fsck:
        return {"fsck", "--no-progress"};
    case MaintenanceTask::CommitGraph:
        return {"commit-graph", "write", "--reachable", "--changed-paths"};
    case MaintenanceTask::MultiPackIndex:
        return {"multi-pack-index", "write"};
    case MaintenanceTask::Maintenance:
        return {"maintenance", "run"};
    case MaintenanceTask::Optimize:
        break;
    }
    return {"gc"};
}

SyncResult toResult(const ProcessResult& process) {
    SyncResult result;
    result.ok = process.exitCode == 0;
    result.exitCode = process.exitCode;
    result.output = process.out + process.err;
    return result;
}

SyncResult combine(SyncResult first, const SyncResult& second) {
    first.output += second.output;
    if (first.ok && !second.ok) {
        first.ok = false;
        first.exitCode = second.exitCode;
    }
    return first;
}

} // namespace

const char* maintenanceTaskName(MaintenanceTask task) {
    switch (task) {
    case MaintenanceTask::Gc:
        return "gc";
    case MaintenanceTask::Fsck:
        return "fsck";
    case MaintenanceTask::CommitGraph:
        return "commit-graph";
    case MaintenanceTask::MultiPackIndex:
        return "multi-pack-index";
    case MaintenanceTask::Maintenance:
        return "maintenance";
    case MaintenanceTask::Optimize:
        return "optimize";
    }
    return "gc";
}

bool maintenanceTaskFromName(const std::string& name, MaintenanceTask* task) {
    for (const MaintenanceTask candidate :
         {MaintenanceTask::Gc, MaintenanceTask::Fsck, MaintenanceTask::CommitGraph,
          MaintenanceTask::MultiPackIndex, MaintenanceTask::Maintenance,
          MaintenanceTask::Optimize}) {
        if (name == maintenanceTaskName(candidate)) {
            *task = candidate;
            return true;
        }
    }
    return false;
}

MaintenanceOps::MaintenanceOps(std::string workDir) : m_workDir(std::move(workDir)) {}

SyncResult MaintenanceOps::run(MaintenanceTask task) const {
    if (task == MaintenanceTask::Optimize) {
        SyncResult combined = run(MaintenanceTask::Gc);
        combined = combine(combined, run(MaintenanceTask::CommitGraph));
        return combine(combined, run(MaintenanceTask::MultiPackIndex));
    }
    return toResult(runGit(commandFor(task), m_workDir));
}

} // namespace gk
