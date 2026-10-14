#pragma once

#include "core/types.h"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gk {

/// A lane that keeps running below a given row, together with the colour it
/// carries there.
///
/// The colour cannot be a property of the lane index alone: lanes are recycled,
/// so lane 3 can belong to one branch near the top of the list and to a
/// different branch further down. A per-row snapshot is what keeps a line from
/// changing colour halfway down the column.
struct GraphLane {
    int lane = 0;
    int colorIndex = 0;
};

/// The lanes alive across one gap between rows, sorted by lane, shared between
/// the rows that have the same ones.
///
/// Most rows leave the set exactly as the row above left it, so they point at one
/// immutable copy rather than holding their own. A copy per row is rows × lanes:
/// with a couple of thousand live branches that was a gigabyte by twenty thousand
/// rows.
class LaneSet {
public:
    using const_iterator = std::vector<GraphLane>::const_iterator;

    LaneSet() = default;
    explicit LaneSet(std::vector<GraphLane> lanes);

    const_iterator begin() const { return lanes().begin(); }
    const_iterator end() const { return lanes().end(); }
    std::size_t size() const { return lanes().size(); }
    bool empty() const { return lanes().empty(); }
    const GraphLane& operator[](std::size_t index) const { return lanes()[index]; }

    const GraphLane* find(int lane) const;
    int maxLane() const { return m_data ? m_data->maxLane : -1; }
    /// Two sets built once and handed to several rows answer the same identity.
    const void* identity() const { return m_data.get(); }

private:
    struct Data {
        std::vector<GraphLane> lanes;
        int maxLane = -1;
    };
    const std::vector<GraphLane>& lanes() const;

    std::shared_ptr<const Data> m_data;
};

/// What a row stands for. The graph draws each one differently: an ordinary
/// commit is a disc with initials, a merge is a small junction dot, a stash is a
/// boxed mark, and the working-tree row takes one of three shapes depending on
/// whether the reader has conflicts to settle or an operation left open.
///
/// The commit list alone cannot tell all of these apart — a stash tip and a merge
/// look identical to the walk — so the model hands the ones it knows as hints and
/// the layout fills the rest from the parents.
enum class GraphNodeType {
    Commit,
    Merge,             ///< more than one parent
    MergeConflict,     ///< the working tree is mid-operation with conflicts
    Stash,             ///< the tip of `refs/stash`
    UnsupportedRebase, ///< a rebase is open
    WorkDir,           ///< the synthetic working-tree row
    Timeline,          ///< a date separator: no commit, the lanes pass through
};

/// One edge leaving a commit towards a parent that needed a lane of its own.
///
/// Only parents that were not already awaited by another lane get an edge here:
/// that is how a merge's second parent line is born. A parent that some lane is
/// already waiting for is reached by that lane further down instead, and the
/// two lines meet at the parent's row — see `converging`.
struct GraphEdge {
    int fromLane = 0;
    int toLane = 0;
    int colorIndex = 0;
    /// Index of the parent commit in the loaded list, or -1 when the parent
    /// has not been loaded yet (rendered as a line continuing downwards).
    int toRow = -1;
};

struct GraphNode {
    int lane = 0;
    int colorIndex = 0;
    /// The palette index the node's colour came from, kept apart from `lane` for
    /// the reason the lane order is a permutation: `updateLaneOrder` moves a node
    /// to another lane, but its colour must not move with it. Equal to
    /// `colorIndex` (a per-row colour snapshot, never a lane property).
    int colorColumn = 0;
    /// The furthest lane this row's lines reach, node and edges together. In
    /// display lanes, so it answers "does this row draw past column N" without
    /// walking the edges again — the check a pinned-width snapshot makes.
    int edgeColumnMax = 0;
    std::vector<GraphEdge> edges;
    /// Lanes that arrive from above and *end* at this commit's node.
    ///
    /// This is where several branches meet — a merge base, or the commit two
    /// branches both descend from. Without it those lines stop dead in mid-air
    /// and the graph reads as broken: you cannot see where a branch came from.
    std::vector<GraphLane> converging;
    /// Lanes that carry a line through the gap above this row, i.e. the ones
    /// that existed before it.
    ///
    /// Needed to close the gaps: a line that already existed is drawn across
    /// the full row height so it meets the segment above it, while a line that
    /// *starts* at this commit is drawn from the node downwards only.
    LaneSet activeLanesAbove;
    /// Lanes that carry a line through the gap below this row. The delegate
    /// draws one vertical segment per entry, which is what makes long edges
    /// render correctly in a per-row (virtualised) painter.
    LaneSet activeLanesBelow;
    bool isMerge = false;
    /// What this row is. `Merge`/`Commit` are derived from the parents; the model
    /// overrides the rest through `setNodeTypeHints`.
    GraphNodeType type = GraphNodeType::Commit;
};

enum class GraphEdgeRole {
    Starting,    ///< born here: a merge's extra parent, or a branch tip
    PassThrough, ///< already alive above and still alive below
    Ending,      ///< arrives here and stops: a meeting point, or a root
};

/// One lane's line at one row, in the single canonical shape the drawing rule
/// needs.
///
/// The layout keeps the pieces in four fields — `edges`, `converging`,
/// `activeLanesAbove`, `activeLanesBelow` — because that is how the walk fills
/// them in. But the rule the delegate must follow is simpler than that storage:
/// a starting line is only a curve out of the node, a pass-through line is only a
/// vertical, and an ending line is only a curve into it. Folding the four into
/// one view is what stops the delegate and `--probe` from disagreeing about which
/// piece a lane is — the confusion behind the "curve on the child row" and "loose
/// segment above a born lane" artefacts.
struct GraphRowEdge {
    GraphEdgeRole role = GraphEdgeRole::PassThrough;
    int lane = 0;
    int colorIndex = 0;
    /// Row of the parent a starting edge reaches, when it is loaded; -1 for a
    /// merge whose parent is not paged in yet, and for a tip whose line simply
    /// runs on below.
    int toRow = -1;
};

/// The row's lines, one per lane, derived from the four stored fields.
///
/// A lane alive both above and below passes through; one alive only below starts
/// here; one alive only above ends here. That is exactly the membership the walk
/// leaves behind, and it reproduces the delegate's drawing without a second
/// opinion about topology.
std::vector<GraphRowEdge> rowEdgesOf(const GraphNode& node);

const char* graphNodeTypeName(GraphNodeType type);

/// Lane and colour assignment for a newest-first commit list.
///
/// Appending a page continues the walk from the live-lane state the previous page
/// left behind, so a page costs its own rows rather than the whole loaded range.
/// Changing tip colours or importance re-lays out from the start: those are
/// per-refresh decisions, not per-page ones.
class GraphLayout {
public:
    void reset();
    void setCommits(const std::vector<CommitInfo>& commits);
    void append(const std::vector<CommitInfo>& commits);

    /// Colour index to use for the branch tip at a given commit. Without this
    /// the graph would invent a colour per lane, and a branch's line would not
    /// match its label. Keys are commit ids; values index the branch palette.
    void setTipColors(std::unordered_map<std::string, int> tipColors);

    void setTipPriority(std::unordered_map<std::string, int> tipPriority);

    /// Types the commit list cannot tell on its own: a stash tip, a working-tree
    /// row, an operation left open. Keyed by commit id. Set after
    /// `setCommits`/`append`; it only relabels nodes, it never moves a lane, so a
    /// refresh that changes nothing but a row's type costs no re-layout.
    void setNodeTypeHints(std::unordered_map<std::string, GraphNodeType> hints);

    std::size_t size() const { return m_nodes.size(); }
    bool empty() const { return m_nodes.empty(); }
    const GraphNode& node(std::size_t index) const { return m_nodes.at(index); }
    int laneCount() const { return m_laneCount; }
    const std::vector<CommitInfo>& commits() const { return m_commits; }

private:
    void rebuild();
    /// Lays out rows `[from, to)` continuing from the live-lane state the last
    /// laid-out row left behind, appending to m_nodes.
    void processRows(std::size_t from, std::size_t to);
    /// Builds the natural-lane -> display-lane order from lane importance and
    /// applies it. When it leaves every lane that already existed where it was,
    /// only rows from `firstNewRow` are remapped; a change remaps every row.
    void updateLaneOrder(std::size_t firstNewRow);
    int freeColor(const std::vector<int>& laneColors) const;
    LaneSet liveLanes() const;

    std::vector<CommitInfo> m_commits;
    std::vector<GraphNode> m_nodes;
    std::unordered_map<std::string, int> m_tipColors;
    std::unordered_map<std::string, int> m_tipPriority;
    std::unordered_map<std::string, GraphNodeType> m_typeHints;

    // Live-lane state after the last laid-out row, so append continues instead
    // of redoing the whole walk. Lanes here are natural, not display, numbers.
    std::vector<std::string> m_expected;
    std::unordered_map<std::string, std::vector<int>> m_awaited;
    std::vector<int> m_laneColors;
    std::vector<int> m_lanePriorities;
    LaneSet m_lastActiveBelow;
    std::unordered_map<std::string, int> m_rowOf;
    /// Parent oid -> the edges born before it loaded, waiting for its row.
    std::unordered_map<std::string, std::vector<std::pair<std::size_t, std::size_t>>>
        m_pendingEdges;
    bool m_walkReady = false;

    std::vector<int> m_laneOrder;
    std::vector<int> m_laneOrderInv;
    int m_orderedLanes = 0;

    int m_laneCount = 0;
};

} // namespace gk
