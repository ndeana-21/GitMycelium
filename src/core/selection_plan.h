#pragma once

#include "core/types.h"

#include <string>
#include <vector>

namespace gk {

/// The two things a lineage walk needs from a commit: which one it is, and what it says
/// its parents are. The graph already holds both in every row, and the walk needs nothing
/// else, so it asks for nothing else — copying whole commit messages to trace a line would
/// be work for no reason, and would make the caller do it more often than it has to.
struct LineageRow {
    Oid id;
    std::vector<Oid> parents;
};

/// What a set of commits picked in the graph can be asked to do.
///
/// The graph lets a reader select several commits at once, and what makes sense depends on
/// where they sit. One fact governs it: a rewrite of history replays a stretch of one
/// branch. So commits of the checked-out branch can be dropped or squashed, commits from
/// anywhere else can only be cherry-picked — copied here, not removed from there — and a
/// squash needs its commits to follow one another, because it folds each into the one below
/// it, and "the one below" has to be part of the selection or it means something else.
///
/// All three answers are reported at once rather than one operation being picked here: the
/// menu greys out what is not on offer and needs the reason for each, not a verdict.
struct SelectionPlan {
    bool allOnCurrentBranch = false;
    bool allElsewhere = false;

    bool canDrop = false;
    /// Folding the selection into the commit below it. Needs the commits to follow one
    /// another without a gap and the stretch above them to be free of merges.
    bool canSquash = false;
    bool canCherryPick = false;

    /// Commits of the checked-out branch in the selection, oldest first — the order a
    /// rewrite replays them in.
    std::vector<Oid> branchCommits;
    /// Commits from elsewhere in the selection, oldest first.
    std::vector<Oid> pickedCommits;

    /// Why squash is not on offer, when it is not. Said out loud rather than hidden.
    std::string squashRefusal;
    /// Why nothing at all can be done with this selection. Empty when something can.
    std::string refusal;
};

/// Decides what can be done with the commits a reader selected.
///
/// `selection` is in the order the graph shows them — newest first, the order the rows come
/// in. `lineage` is the checked-out branch's own line, newest first. A selected commit that
/// is not in it is not on this branch.
///
/// `rangeIsLinear` says whether the stretch between the selection's oldest commit and the
/// branch tip holds no merges. Only the caller can answer that — it needs the repository —
/// and it is consulted only for a squash, where a merge in the range would make git fold a
/// commit into something the reader never selected.
SelectionPlan planSelection(const std::vector<Oid>& selection, const std::vector<Oid>& lineage,
                            bool rangeIsLinear);

/// The commits of the checked-out branch's own line, newest first: the tip, its first
/// parent, that commit's first parent, and so on.
///
/// The graph draws every branch at once, so "the same branch" has to be something a reader
/// can point at, and this is it: the line the checked-out branch would replay if it were
/// rebased. Commits merged in from elsewhere are not on it. `rows` must be the loaded ones
/// in graph order; when `tip` is not among them the result is empty, because nothing loaded
/// is on this branch and guessing would be worse than saying so.
std::vector<Oid> firstParentLineage(const std::vector<LineageRow>& rows, const Oid& tip);

/// Every commit reachable from `tip` through its parents, restricted to `rows`.
///
/// This is "the commits of a branch" for the hover highlight when the pointer is
/// over that branch: its tip and all of its loaded ancestry. A commit merged in
/// from a side branch is included, because it really is part of the branch's
/// history. Returns empty when the reachable set would exceed `cap` — a highlight
/// covering an arbitrary part of a branch is worse than none, so the caller is
/// told to show nothing rather than shown a guess.
std::vector<Oid> ancestorSet(const std::vector<LineageRow>& rows, const Oid& tip,
                             std::size_t cap);

} // namespace gk
