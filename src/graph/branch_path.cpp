#include "graph/branch_path.h"

#include <algorithm>

namespace gk {
namespace {

bool isSeparator(const GraphNode& node) { return node.type == GraphNodeType::Timeline; }

/// The topmost commit whose first-parent line reaches `row` in its own lane: walking
/// up while that lane carries on above. A lane born as a merge's extra parent ends the
/// walk, since the merge is not on the branch, only on its way out of it.
int tipOf(int row, const std::function<const GraphNode*(int)>& node) {
    int tip = row;
    const int lane = node(row)->lane;
    for (int above = row - 1; above >= 0; --above) {
        const GraphNode* current = node(above + 1);
        if (!current->activeLanesAbove.find(lane)) {
            break;
        }
        const GraphNode* candidate = node(above);
        if (!isSeparator(*candidate) && candidate->lane == lane) {
            tip = above;
        }
    }
    return tip;
}

} // namespace

BranchPath BranchPath::trace(int anchorRow, int rowCount,
                             const std::function<const GraphNode*(int)>& node,
                             const std::function<int(int)>& firstParentRow) {
    BranchPath path;
    if (anchorRow < 0 || anchorRow >= rowCount || !node(anchorRow) ||
        isSeparator(*node(anchorRow))) {
        return path;
    }
    for (int row = tipOf(anchorRow, node); row >= 0 && row < rowCount;) {
        path.m_nodes.push_back(row);
        const int parent = firstParentRow(row);
        if (parent < 0) {
            break;
        }
        path.m_segments.push_back({row, parent});
        if (parent == kNotLoaded || parent <= row) {
            break;
        }
        row = parent;
    }
    return path;
}

bool BranchPath::hasNode(int row) const {
    return std::binary_search(m_nodes.begin(), m_nodes.end(), row);
}

std::vector<BranchPath::RowPiece> BranchPath::piecesAt(int row) const {
    std::vector<RowPiece> pieces;
    const auto after = std::upper_bound(
        m_segments.begin(), m_segments.end(), row,
        [](int value, const Segment& segment) { return value < segment.fromRow; });
    if (after == m_segments.begin()) {
        return pieces;
    }
    const auto current = after - 1;
    if (current != m_segments.begin()) {
        const Segment& previous = *(current - 1);
        if (previous.toRow == row) {
            pieces.push_back({previous.fromRow, true, false});
        }
    }
    if (current->fromRow == row) {
        pieces.push_back({current->fromRow, false, true});
    } else if (row < current->toRow) {
        pieces.push_back({current->fromRow, true, true});
    } else if (row == current->toRow) {
        pieces.push_back({current->fromRow, true, false});
    }
    return pieces;
}

} // namespace gk
