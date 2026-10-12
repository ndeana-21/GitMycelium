#include "core/selection_plan.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace gk {

std::vector<Oid> ancestorSet(const std::vector<LineageRow>& rows, const Oid& tip,
                             std::size_t cap) {
    std::vector<Oid> out;
    if (tip.empty()) {
        return out;
    }

    std::unordered_map<Oid, std::size_t> rowOf;
    rowOf.reserve(rows.size() * 2);
    for (std::size_t row = 0; row < rows.size(); ++row) {
        rowOf.emplace(rows[row].id, row);
    }
    if (rowOf.find(tip) == rowOf.end()) {
        // The tip is not loaded, so nothing loaded can be called a commit of it.
        return out;
    }

    std::unordered_set<Oid> seen;
    std::vector<Oid> pending{tip};
    while (!pending.empty()) {
        const Oid id = std::move(pending.back());
        pending.pop_back();
        if (!seen.insert(id).second) {
            continue;
        }
        if (out.size() >= cap) {
            // Too much history behind this branch to dim the rest usefully. Saying
            // "no highlight" is honest; dimming all but an arbitrary slice is not.
            out.clear();
            return out;
        }
        out.push_back(id);
        const auto row = rowOf.find(id);
        if (row == rowOf.end()) {
            // Older than the loaded range: the walk stops at the page edge rather
            // than pretending a row it never saw is not an ancestor.
            continue;
        }
        for (const Oid& parent : rows[row->second].parents) {
            if (seen.find(parent) == seen.end()) {
                pending.push_back(parent);
            }
        }
    }
    return out;
}

std::vector<Oid> firstParentLineage(const std::vector<LineageRow>& rows, const Oid& tip) {
    std::vector<Oid> line;
    if (tip.empty()) {
        return line;
    }

    std::unordered_map<Oid, std::size_t> rowOf;
    rowOf.reserve(rows.size() * 2);
    for (std::size_t row = 0; row < rows.size(); ++row) {
        rowOf.emplace(rows[row].id, row);
    }

    const auto start = rowOf.find(tip);
    if (start == rowOf.end()) {
        // The tip is not among the loaded rows, so nothing loaded can be called a commit of
        // this branch. An empty answer sends the caller to the other operation, which is
        // the honest one: it cannot know, so it must not act as though it did.
        return line;
    }

    std::size_t row = start->second;
    for (std::size_t guard = 0; guard <= rows.size(); ++guard) {
        const LineageRow& current = rows[row];
        line.push_back(current.id);
        if (current.parents.empty()) {
            break;
        }
        const auto parent = rowOf.find(current.parents.front());
        if (parent == rowOf.end()) {
            // The parent is older than the loaded range. The line stops at the page edge
            // rather than pretending the row below is a parent it is not.
            break;
        }
        if (parent->second <= row) {
            break; // parents are drawn below their children; anything else is not history
        }
        row = parent->second;
    }
    return line;
}

SelectionPlan planSelection(const std::vector<Oid>& selection, const std::vector<Oid>& lineage,
                            bool rangeIsLinear) {
    SelectionPlan plan;
    if (selection.empty()) {
        plan.refusal = "nothing is selected";
        return plan;
    }

    std::unordered_map<Oid, std::size_t> positionOnBranch;
    positionOnBranch.reserve(lineage.size() * 2);
    for (std::size_t position = 0; position < lineage.size(); ++position) {
        positionOnBranch.emplace(lineage[position], position);
    }

    // Positions are counted from the tip, so a smaller number is a newer commit.
    std::vector<std::size_t> onBranch;
    for (const Oid& id : selection) {
        const auto found = positionOnBranch.find(id);
        if (found != positionOnBranch.end()) {
            onBranch.push_back(found->second);
        }
    }

    if (onBranch.empty()) {
        plan.allElsewhere = true;
        plan.canCherryPick = true;
        // Oldest first: the graph hands them over newest first, and a cherry-pick replays in
        // the order it is given.
        plan.pickedCommits.assign(selection.rbegin(), selection.rend());
        return plan;
    }

    if (onBranch.size() != selection.size()) {
        plan.refusal =
            "this selection mixes commits of the checked-out branch with commits from "
            "elsewhere: rewriting one branch and copying from another are different "
            "operations, so select one or the other";
        return plan;
    }

    plan.allOnCurrentBranch = true;
    plan.canDrop = true;
    std::sort(onBranch.begin(), onBranch.end(), std::greater<std::size_t>());
    for (const std::size_t position : onBranch) {
        plan.branchCommits.push_back(lineage[position]);
    }

    if (plan.branchCommits.size() < 2) {
        plan.squashRefusal =
            "squash folds a commit into the one below it, so it needs at least two commits";
        return plan;
    }

    // Consecutive means consecutive on the branch, not in the table: other branches' rows
    // are drawn in between, and a gap of two means a commit of this branch sits between the
    // selected ones that the reader did not select. Folding then means something the reader
    // did not ask for.
    bool consecutive = true;
    for (std::size_t index = 1; index < onBranch.size(); ++index) {
        if (onBranch[index - 1] != onBranch[index] + 1) {
            consecutive = false;
            break;
        }
    }

    if (!consecutive) {
        plan.squashRefusal = "squash needs commits that follow one another on the branch";
    } else if (!rangeIsLinear) {
        plan.squashRefusal =
            "a merge sits between these commits and the branch tip: squash needs an "
            "uninterrupted stretch of one branch";
    } else {
        plan.canSquash = true;
    }
    return plan;
}

} // namespace gk
