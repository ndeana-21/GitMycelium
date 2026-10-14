#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gk {

enum class AutoReason {
    None,
    Identical,
    OnlyTheirsChanged,
    OnlyOursChanged,
    TrailingSpace,
    DisjointChanges,
};

const char* toString(AutoReason reason);

struct AutoHunk {
    AutoReason reason = AutoReason::None;
    bool resolved = false;
    /// What replaces the conflict markers, when it was resolved. Includes its own
    /// line endings; empty and resolved means the region was dropped.
    std::string text;
};

struct AutoResolved {
    std::string content;
    std::vector<AutoHunk> hunks; ///< in file order, the undecided ones included
    std::size_t resolvedCount = 0;
    std::size_t leftCount = 0;
};

struct AutoOptions {
    /// True for a rebase step. There, "theirs" is not another branch's work but the
    /// snapshot of the commit being replayed, context and all. Taking that snapshot
    /// wholesale is correct for a merge and wrong here: it restores lines from commits
    /// the reader removed on purpose — a dropped commit, a reset — and does it looking
    /// like a tidy resolution. In a rebase only what both sides agree on is settled.
    bool rebaseStep = false;
};

AutoResolved autoResolveMarkedConflicts(const std::string& markedContent,
                                        const AutoOptions& options = {});

bool hasConflictMarkers(const std::string& content);

} // namespace gk
