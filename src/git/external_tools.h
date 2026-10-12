#pragma once

#include "git/remote.h"

#include <string>
#include <vector>

namespace gk {

/// External diff and merge tools, through the `git` binary.
///
/// The choice lives in the git config (`diff.tool`/`merge.tool`) rather than in
/// `QSettings`, so it is the same answer every client sees, and git is what knows
/// how to invoke each tool.
class ExternalToolOps {
public:
    explicit ExternalToolOps(std::string workDir);

    /// The effective `diff.tool`/`merge.tool`, empty when unset (git then picks its
    /// own default).
    std::string tool(bool merge) const;

    /// Writes the choice into the repository's own config (`git config --local`). An
    /// empty `name` unsets it, which is how the reader returns to git's default.
    bool setTool(bool merge, const std::string& name, std::string* errorOut) const;

    /// Names `git difftool/mergetool --tool-help` knows, in its own order. It lists
    /// valid-but-not-installed tools too: hiding them would make a tool the reader has
    /// look unknown.
    std::vector<std::string> availableTools(bool merge) const;

    /// Opens the pair in the external diff tool. An empty `target` diffs the working
    /// tree against `base`; both empty diff the index against the working tree, and
    /// `cached` diffs HEAD against the index.
    SyncResult runDiff(const std::string& base, const std::string& target,
                       const std::string& path, bool cached = false) const;
    SyncResult runMerge(const std::string& path) const;

    const std::string& workDir() const { return m_workDir; }

private:
    std::string m_workDir;
};

} // namespace gk
