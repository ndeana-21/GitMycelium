#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gk {

/// Why a conflict hunk could be settled without asking anyone.
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

/// Settles the conflict hunks in a file marked up in diff3 style — ours, base,
/// theirs — wherever the answer is a matter of comparing text rather than judging.
///
/// This is the case that makes a rebase miserable for no reason: a hunk where our
/// side is exactly the base and only the incoming side changed something is not a
/// disagreement, it is a conflict that exists because the merge had to widen the
/// region around a real change. Those are settled here. Anything where both sides
/// really wrote different text is left alone, markers and all.
///
/// Deliberately not done: taking both sides when each added different lines. It is
/// what a person often wants and what a program must not assume — two added lines
/// can be two halves of one statement, and a client that joins them produces code
/// that compiles into something nobody wrote.
/// What the caller knows about the operation that produced the conflict.
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
