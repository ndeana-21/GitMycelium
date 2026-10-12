#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gk {

/// One branch's tip metadata, read in a single `git for-each-ref` call.
///
/// A filter preset ("mine", "recent", "no upstream", "ahead/behind") needs a fact
/// or two about every branch; `for-each-ref` answers for the whole list at once,
/// which is the only shape that survives a repository with thousands of branches.
struct RefMeta {
    std::string fullName;
    std::string shortName;
    std::string authorName;
    std::string authorEmail; ///< without the angle brackets
    std::int64_t commitUnix = 0;
    std::string upstream; ///< origin/feature; empty when none
    bool ahead = false;
    bool behind = false;
    bool local = true; ///< false for a remote-tracking branch
};

/// Branch tip metadata, delegated to the `git` binary in one call.
class RefMetaOps {
public:
    explicit RefMetaOps(std::string workDir);

    /// Local and remote-tracking branches. Tags are left out: a preset filters
    /// branches, and a tag is a name for a commit.
    std::vector<RefMeta> read() const;

    const std::string& workDir() const { return m_workDir; }

private:
    std::string m_workDir;
};

} // namespace gk
