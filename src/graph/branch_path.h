#pragma once

#include "graph/graph_layout.h"

#include <climits>
#include <functional>
#include <vector>

namespace gk {

/// The line a selected commit sits on, traced through the loaded graph: up its lane to
/// the tip of its branch, then down the first-parent chain. Below the point where the
/// branch was cut, that chain runs along the branch it came from, and from there along
/// that branch's own origin, so each ancestor branch is part of the path only from the
/// fork downwards.
///
/// Rows, never lanes: the lane order is a permutation that a new page may change, so a
/// segment's lane is read from the layout when it is drawn.
class BranchPath {
public:
    /// A first parent that is not paged in yet: the line runs on past the last row.
    static constexpr int kNotLoaded = INT_MAX;

    /// One commit-to-first-parent stretch. Its line is the lane `fromRow`'s node sits
    /// in, from that node down to `toRow` (or past the end, for `kNotLoaded`).
    struct Segment {
        int fromRow = 0;
        int toRow = kNotLoaded;
    };

    /// What of the path a row draws.
    struct RowPiece {
        int fromRow = 0;  ///< the segment's start; its node's lane is the line's lane
        bool top = false;    ///< from the top of the row to its centre
        bool bottom = false; ///< from the centre to the bottom of the row
    };

    /// `firstParentRow(row)` answers the row of that commit's first parent, -1 for a
    /// root, or `kNotLoaded`.
    static BranchPath trace(int anchorRow, int rowCount,
                            const std::function<const GraphNode*(int)>& node,
                            const std::function<int(int)>& firstParentRow);

    bool empty() const { return m_nodes.empty(); }
    int tipRow() const { return m_nodes.empty() ? -1 : m_nodes.front(); }
    const std::vector<int>& nodeRows() const { return m_nodes; }
    const std::vector<Segment>& segments() const { return m_segments; }

    bool hasNode(int row) const;
    /// The pieces of the path crossing `row`: at most two, where one segment ends at a
    /// node and the next begins there.
    std::vector<RowPiece> piecesAt(int row) const;

private:
    std::vector<int> m_nodes;
    std::vector<Segment> m_segments;
};

} // namespace gk
