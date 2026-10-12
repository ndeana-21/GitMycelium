#pragma once

#include "git/remote.h"

#include <string>

namespace gk {

/// The maintenance verbs a reader can invoke. `Optimize` is the composite the
/// "Optimize Repository" action runs.
enum class MaintenanceTask { Gc, Fsck, CommitGraph, MultiPackIndex, Maintenance, Optimize };

const char* maintenanceTaskName(MaintenanceTask task);

/// Resolves a name from `maintenanceTaskName` back to the task.
bool maintenanceTaskFromName(const std::string& name, MaintenanceTask* task);

/// Repository maintenance, delegated to the `git` binary.
///
/// `gc` is a repack with the user's `gc.*` configuration, hooks and pack settings,
/// and `fsck` is the connectivity check — reimplementing either would be a lookalike
/// that diverges on exactly the repositories that need it. These mutate the object
/// store, not pointers, so they must never be recorded in the operation log: putting
/// the pointers back cannot bring an object back.
///
/// `Optimize` runs `gc` before the commit-graph and the multi-pack-index, because
/// the multi-pack-index has nothing to write while every object is still loose.
class MaintenanceOps {
public:
    explicit MaintenanceOps(std::string workDir);

    SyncResult run(MaintenanceTask task) const;

    const std::string& workDir() const { return m_workDir; }

private:
    std::string m_workDir;
};

} // namespace gk
