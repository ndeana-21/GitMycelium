#pragma once

#include "git/remote.h"

#include <string>
#include <vector>

namespace gk {

/// Sparse checkout, delegated to the `git` binary.
///
/// Sparse checkout is a cone: the chosen directories and everything under them,
/// and nothing else. It moves no refs, but it changes the working tree and the
/// index, so it goes through `guarded`, never the operation log: putting the
/// pointers back would not bring a removed file back.
class SparseOps {
public:
    explicit SparseOps(std::string workDir);

    bool enabled() const;
    std::vector<std::string> list() const;
    /// The repository's top-level directories, read from HEAD's tree so a
    /// directory currently excluded still appears and can be re-included.
    std::vector<std::string> topLevelDirectories() const;
    SyncResult set(const std::vector<std::string>& dirs) const;
    SyncResult disable() const;

    const std::string& workDir() const { return m_workDir; }

private:
    std::string m_workDir;
};

std::vector<std::string> parseSparseList(const std::string& output);

} // namespace gk
