#pragma once

#include <string>
#include <vector>

namespace gk {

/// What a hook name resolves to on disk.
///
/// `Sample` is the state of a fresh `git init`: git leaves
/// `<name>.sample` behind and no functioning hook — an executable sample is
/// still a sample, never a hook that runs.
enum class HookState { Missing, Sample, Disabled, Enabled };

const char* hookStateName(HookState state);

/// One hook name and what the repository has for it.
struct HookEntry {
    std::string name;
    /// `<hooks dir>/<name>`; empty when the hooks directory cannot be resolved.
    std::string path;
    HookState state = HookState::Missing;
    /// Server-side hooks (pre-receive, update, …) never run on a local repository.
    bool serverSide = false;
};

/// Git hooks as files, managed through the `git` binary and the filesystem.
///
/// libgit2 has no concept of a hook, and listing the directory by hand would miss
/// `core.hooksPath`. The hooks directory is therefore resolved with
/// `git rev-parse --git-path hooks`, which already answers for the default, for a
/// configured `core.hooksPath` and for a worktree.
///
/// These write files, not pointers, so they are guarded, never recorded: the
/// operation log restores refs and could not put a hook back.
class HooksOps {
public:
    explicit HooksOps(std::string workDir);

    /// The known client and server hooks, each with its state, plus any other
    /// regular file found in the hooks directory so nothing is silently hidden.
    std::vector<HookEntry> list() const;

    /// The resolved hooks directory, empty when git cannot answer.
    std::string dir() const;

    /// The hook's text, empty when the file does not exist.
    std::string read(const std::string& name) const;

    /// Writes `name`'s content, creating the file. The executable bit is left as it
    /// was; `enable` is what turns a hook on.
    void write(const std::string& name, const std::string& content) const;

    /// Makes `name` executable. When only `<name>.sample` exists it is copied into
    /// place first, so a hook can be started from git's own sample.
    void enable(const std::string& name) const;

    /// Clears the executable bit. A name with no file is left alone.
    void disable(const std::string& name) const;

    const std::string& workDir() const { return m_workDir; }

private:
    std::string m_workDir;
};

} // namespace gk
