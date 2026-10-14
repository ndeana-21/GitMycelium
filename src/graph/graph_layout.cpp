#include "graph/graph_layout.h"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace gk {
namespace {

/// The largest lane any of the row's lines touches: the node's own, the lanes
/// carrying through, the ones meeting here, and the lanes a merge's extra parent
/// was born into. This is `edgeColumnMax`, and it is what lets the
/// delegate or a headless check ask "does this row draw past column N" without
/// walking the edges a second time.
int maxLaneOf(const GraphNode& node) {
    int best = node.lane;
    const auto consider = [&best](const GraphLane& lane) {
        best = std::max(best, lane.lane);
    };
    best = std::max({best, node.activeLanesBelow.maxLane(), node.activeLanesAbove.maxLane()});
    for (const GraphLane& lane : node.converging) {
        consider(lane);
    }
    for (const GraphEdge& edge : node.edges) {
        best = std::max({best, edge.fromLane, edge.toLane});
    }
    return best;
}

} // namespace

LaneSet::LaneSet(std::vector<GraphLane> lanes) {
    if (lanes.empty()) {
        return;
    }
    std::sort(lanes.begin(), lanes.end(),
              [](const GraphLane& a, const GraphLane& b) { return a.lane < b.lane; });
    auto data = std::make_shared<Data>();
    data->maxLane = lanes.back().lane;
    data->lanes = std::move(lanes);
    m_data = std::move(data);
}

const std::vector<GraphLane>& LaneSet::lanes() const {
    static const std::vector<GraphLane> none;
    return m_data ? m_data->lanes : none;
}

const GraphLane* LaneSet::find(int lane) const {
    const std::vector<GraphLane>& all = lanes();
    const auto it = std::lower_bound(all.begin(), all.end(), lane,
                                     [](const GraphLane& l, int value) { return l.lane < value; });
    return it != all.end() && it->lane == lane ? &*it : nullptr;
}

const char* graphNodeTypeName(GraphNodeType type) {
    switch (type) {
    case GraphNodeType::Commit: return "commit";
    case GraphNodeType::Merge: return "merge";
    case GraphNodeType::MergeConflict: return "conflict";
    case GraphNodeType::Stash: return "stash";
    case GraphNodeType::UnsupportedRebase: return "rebase";
    case GraphNodeType::WorkDir: return "workdir";
    case GraphNodeType::Timeline: return "timeline";
    }
    return "commit";
}

std::vector<GraphRowEdge> rowEdgesOf(const GraphNode& node) {
    const auto edgeTo = [&node](int lane) -> const GraphEdge* {
        for (const GraphEdge& e : node.edges) {
            if (e.toLane == lane) {
                return &e;
            }
        }
        return nullptr;
    };

    // Lines the reader sees leave the node first, then any lane that only ends
    // here: a stable order, so the probe's output is comparable between runs.
    std::vector<int> lanes;
    lanes.reserve(node.activeLanesBelow.size() + node.converging.size() + 1);
    for (const GraphLane& l : node.activeLanesBelow) {
        lanes.push_back(l.lane);
    }
    for (const GraphLane& l : node.activeLanesAbove) {
        if (!node.activeLanesBelow.find(l.lane)) {
            lanes.push_back(l.lane);
        }
    }

    std::vector<GraphRowEdge> out;
    out.reserve(lanes.size());
    for (const int lane : lanes) {
        const GraphLane* below = node.activeLanesBelow.find(lane);
        const GraphLane* above = node.activeLanesAbove.find(lane);
        GraphRowEdge row;
        row.lane = lane;
        if (above != nullptr && below != nullptr) {
            row.role = GraphEdgeRole::PassThrough;
            row.colorIndex = below->colorIndex;
        } else if (below != nullptr) {
            row.role = GraphEdgeRole::Starting;
            row.colorIndex = below->colorIndex;
            if (const GraphEdge* e = edgeTo(lane)) {
                row.toRow = e->toRow;
            }
        } else {
            row.role = GraphEdgeRole::Ending;
            row.colorIndex = above->colorIndex;
        }
        out.push_back(row);
    }
    return out;
}

void GraphLayout::reset() {
    m_commits.clear();
    m_nodes.clear();
    m_tipColors.clear();
    m_typeHints.clear();
    m_laneCount = 0;
    m_walkReady = false;
    m_expected.clear();
    m_awaited.clear();
    m_laneColors.clear();
    m_lanePriorities.clear();
    m_lastActiveBelow = LaneSet();
    m_rowOf.clear();
    m_pendingEdges.clear();
    m_laneOrder.clear();
    m_laneOrderInv.clear();
    m_orderedLanes = 0;
}

void GraphLayout::setCommits(const std::vector<CommitInfo>& commits) {
    m_commits = commits;
    rebuild();
}

void GraphLayout::append(const std::vector<CommitInfo>& commits) {
    if (commits.empty()) {
        return;
    }
    const std::size_t firstNew = m_commits.size();
    m_commits.insert(m_commits.end(), commits.begin(), commits.end());
    if (!m_walkReady) {
        rebuild();
        return;
    }
    processRows(firstNew, m_commits.size());
    updateLaneOrder(firstNew);
}

void GraphLayout::setTipColors(std::unordered_map<std::string, int> tipColors) {
    m_tipColors = std::move(tipColors);
    if (!m_commits.empty()) {
        rebuild();
    }
}

void GraphLayout::setTipPriority(std::unordered_map<std::string, int> tipPriority) {
    m_tipPriority = std::move(tipPriority);
    if (!m_commits.empty()) {
        rebuild();
    }
}

void GraphLayout::setNodeTypeHints(std::unordered_map<std::string, GraphNodeType> hints) {
    m_typeHints = std::move(hints);
    // Relabelling only: the lane a node sits on is a property of the walk, and a
    // stash tip still occupies the lane its parents put it in.
    for (std::size_t i = 0; i < m_nodes.size() && i < m_commits.size(); ++i) {
        const auto it = m_typeHints.find(m_commits[i].id);
        m_nodes[i].type = it != m_typeHints.end()
                              ? it->second
                              : (m_nodes[i].isMerge ? GraphNodeType::Merge
                                                    : GraphNodeType::Commit);
    }
}

int GraphLayout::freeColor(const std::vector<int>& laneColors) const {
    std::uint64_t used = 0;
    for (const int color : laneColors) {
        if (color >= 0 && color < 64) {
            used |= std::uint64_t{1} << color;
        }
    }
    for (int candidate = 0; candidate < 64; ++candidate) {
        if ((used & (std::uint64_t{1} << candidate)) == 0) {
            return candidate;
        }
    }
    return 0;
}

LaneSet GraphLayout::liveLanes() const {
    std::vector<GraphLane> lanes;
    for (std::size_t k = 0; k < m_expected.size(); ++k) {
        if (!m_expected[k].empty()) {
            lanes.push_back(GraphLane{static_cast<int>(k), m_laneColors[k]});
        }
    }
    return LaneSet(std::move(lanes));
}

void GraphLayout::rebuild() {
    m_nodes.clear();
    m_expected.clear();
    m_awaited.clear();
    m_laneColors.clear();
    m_lanePriorities.clear();
    m_lastActiveBelow = LaneSet();
    m_rowOf.clear();
    m_pendingEdges.clear();
    m_laneOrder.clear();
    m_laneOrderInv.clear();
    m_orderedLanes = 0;
    m_laneCount = 0;
    m_walkReady = false;
    if (m_commits.empty()) {
        return;
    }
    processRows(0, m_commits.size());
    updateLaneOrder(0);
}

void GraphLayout::processRows(std::size_t from, std::size_t to) {
    constexpr int kUnprioritised = 1000;

    const auto addAwaited = [this](int lane, const std::string& oid) {
        auto& lanes = m_awaited[oid];
        lanes.insert(std::lower_bound(lanes.begin(), lanes.end(), lane), lane);
    };
    const auto removeAwaited = [this](int lane, const std::string& oid) {
        const auto it = m_awaited.find(oid);
        if (it == m_awaited.end()) {
            return;
        }
        auto& lanes = it->second;
        lanes.erase(std::remove(lanes.begin(), lanes.end(), lane), lanes.end());
        if (lanes.empty()) {
            m_awaited.erase(it);
        }
    };
    const auto laneAwaiting = [this](const std::string& oid) -> int {
        if (oid.empty()) {
            return -1;
        }
        const auto it = m_awaited.find(oid);
        return it == m_awaited.end() || it->second.empty() ? -1 : it->second.front();
    };
    // Where branches meet, the commit stays on the most important line and the
    // others bend into it: a branch cut from `main` reads as cut from `main`, not
    // as `main` cut from it. Equal importance keeps the leftmost.
    const auto mostImportantLaneAwaiting = [this](const std::string& oid) -> int {
        const auto it = m_awaited.find(oid);
        if (it == m_awaited.end() || it->second.empty()) {
            return -1;
        }
        int best = it->second.front();
        for (const int k : it->second) {
            if (m_lanePriorities[static_cast<std::size_t>(k)] <
                m_lanePriorities[static_cast<std::size_t>(best)]) {
                best = k;
            }
        }
        return best;
    };
    // A lane that just ended at this row must not be handed straight to another
    // line in the same row: the two overlap at the same x and read as one line.
    // Testing the handful burned this row is cheaper than a flag per lane.
    const auto ensureLane = [this](int preferred, int priority,
                                   const std::vector<int>& burnedThisRow) -> int {
        for (std::size_t i = 0; i < m_expected.size(); ++i) {
            if (m_expected[i].empty() &&
                std::find(burnedThisRow.begin(), burnedThisRow.end(), static_cast<int>(i)) ==
                    burnedThisRow.end()) {
                m_laneColors[i] = preferred >= 0 ? preferred : freeColor(m_laneColors);
                m_lanePriorities[i] = std::min(m_lanePriorities[i], priority);
                return static_cast<int>(i);
            }
        }
        m_expected.emplace_back();
        m_laneColors.push_back(preferred >= 0 ? preferred : freeColor(m_laneColors));
        m_lanePriorities.push_back(priority);
        return static_cast<int>(m_expected.size()) - 1;
    };

    for (std::size_t i = from; i < to; ++i) {
        const CommitInfo& commit = m_commits[i];

        // A timeline row is a separator, not a commit: it takes no lane and ends
        // none, so the lines simply carry through it. One node is still pushed so
        // that a row index keeps meaning the same graph node it always did.
        if (const auto hint = m_typeHints.find(commit.id);
            hint != m_typeHints.end() && hint->second == GraphNodeType::Timeline) {
            GraphNode spacer;
            spacer.type = GraphNodeType::Timeline;
            spacer.activeLanesAbove = m_lastActiveBelow;
            spacer.activeLanesBelow = m_lastActiveBelow;
            spacer.edgeColumnMax = maxLaneOf(spacer);
            m_nodes.push_back(std::move(spacer));
            continue;
        }

        if (const auto it = m_pendingEdges.find(commit.id); it != m_pendingEdges.end()) {
            for (const auto& [row, edge] : it->second) {
                m_nodes[row].edges[edge].toRow = static_cast<int>(i);
            }
            m_pendingEdges.erase(it);
        }

        std::vector<int> burnedThisRow;
        int lane = mostImportantLaneAwaiting(commit.id);
        // Whether the set of live lanes below differs from the one above. A commit
        // that continues the lane it was awaited on, into its first parent, leaves
        // it as it was — that is most rows, and those share the row above's set.
        bool lanesChanged = lane < 0 || commit.parents.size() != 1;
        if (lane < 0) {
            const auto tip = m_tipColors.find(commit.id);
            const auto prio = m_tipPriority.find(commit.id);
            lane = ensureLane(tip != m_tipColors.end() ? tip->second : -1,
                              prio != m_tipPriority.end() ? prio->second : kUnprioritised,
                              burnedThisRow);
        }

        GraphNode node;
        node.isMerge = commit.parents.size() > 1;
        node.type = node.isMerge ? GraphNodeType::Merge : GraphNodeType::Commit;
        if (const auto hint = m_typeHints.find(commit.id); hint != m_typeHints.end()) {
            node.type = hint->second;
        }
        node.activeLanesAbove = i == 0 ? LaneSet() : m_lastActiveBelow;

        // Any *other* lane awaiting this commit has arrived here: several
        // branches have met. The meeting must be drawn at this row, from each
        // arriving lane into this node — otherwise those lines just stop and
        // the graph shows branches with no origin.
        if (const auto it = m_awaited.find(commit.id); it != m_awaited.end()) {
            const std::vector<int> arriving = it->second;
            for (const int k : arriving) {
                if (k == lane) {
                    continue;
                }
                node.converging.push_back(
                    GraphLane{k, m_laneColors[static_cast<std::size_t>(k)]});
                removeAwaited(k, commit.id);
                m_expected[static_cast<std::size_t>(k)].clear();
                m_laneColors[static_cast<std::size_t>(k)] = -1;
                burnedThisRow.push_back(k);
                lanesChanged = true;
            }
        }

        const int color = m_laneColors[static_cast<std::size_t>(lane)];
        removeAwaited(lane, commit.id);
        m_expected[static_cast<std::size_t>(lane)].clear();
        burnedThisRow.push_back(lane);

        // A tip's importance belongs to the lane it sits on, not only to lanes
        // created for it. `main` can point at a commit that another branch's
        // line already passes through, in which case no lane is created for it.
        if (const auto prio = m_tipPriority.find(commit.id); prio != m_tipPriority.end()) {
            m_lanePriorities[static_cast<std::size_t>(lane)] =
                std::min(m_lanePriorities[static_cast<std::size_t>(lane)], prio->second);
        }

        // A commit that carries a label is a branch tip, even when it also sits
        // on another branch's line. The node takes the label's colour so it
        // matches its pill; the line below keeps the lane's colour.
        node.lane = lane;
        node.colorIndex = color;
        if (const auto tip = m_tipColors.find(commit.id); tip != m_tipColors.end()) {
            node.colorIndex = tip->second;
        }

        // The first parent continues straight down in this lane, keeping this
        // commit's colour. No edge is recorded: the vertical segment drawn by
        // activeLanesBelow *is* that line.
        if (!commit.parents.empty()) {
            m_expected[static_cast<std::size_t>(lane)] = commit.parents.front();
            m_laneColors[static_cast<std::size_t>(lane)] = color;
            addAwaited(lane, commit.parents.front());
        } else {
            m_laneColors[static_cast<std::size_t>(lane)] = -1;
        }

        // Additional parents get a lane of their own unless one already awaits
        // them. A new lane is a line born at this commit, so it needs an edge.
        for (std::size_t p = 1; p < commit.parents.size(); ++p) {
            if (laneAwaiting(commit.parents[p]) < 0) {
                const int pl = ensureLane(-1, kUnprioritised, burnedThisRow);
                m_expected[static_cast<std::size_t>(pl)] = commit.parents[p];
                addAwaited(pl, commit.parents[p]);

                GraphEdge edge;
                edge.fromLane = lane;
                edge.toLane = pl;
                edge.colorIndex = m_laneColors[static_cast<std::size_t>(pl)];
                edge.toRow = -1;
                const std::size_t edgeIndex = node.edges.size();
                if (const auto at = m_rowOf.find(commit.parents[p]); at != m_rowOf.end()) {
                    edge.toRow = at->second;
                } else {
                    m_pendingEdges[commit.parents[p]].push_back({i, edgeIndex});
                }
                node.edges.push_back(edge);
            }
        }

        if (lanesChanged) {
            m_lastActiveBelow = liveLanes();
        }
        node.activeLanesBelow = m_lastActiveBelow;

        node.colorColumn = node.colorIndex;
        node.edgeColumnMax = maxLaneOf(node);

        m_rowOf.emplace(commit.id, static_cast<int>(i));
        m_nodes.push_back(std::move(node));
    }

    m_laneCount = static_cast<int>(m_expected.size());
    m_walkReady = true;
}

void GraphLayout::updateLaneOrder(std::size_t firstNewRow) {
    if (m_laneCount <= 1 || m_lanePriorities.size() < static_cast<std::size_t>(m_laneCount)) {
        m_laneOrder.clear();
        m_laneOrderInv.clear();
        m_orderedLanes = 0;
        return;
    }

    std::vector<int> order(static_cast<std::size_t>(m_laneCount));
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
        return m_lanePriorities[static_cast<std::size_t>(a)] <
               m_lanePriorities[static_cast<std::size_t>(b)];
    });
    std::vector<int> perm(static_cast<std::size_t>(m_laneCount), 0);
    for (int display = 0; display < m_laneCount; ++display) {
        perm[static_cast<std::size_t>(order[static_cast<std::size_t>(display)])] = display;
    }

    // Stable when it leaves every lane that already existed where it was: only
    // the new rows then need remapping, and the ones above are left alone.
    bool stable = m_orderedLanes > 0 &&
                  m_laneOrder.size() == static_cast<std::size_t>(m_orderedLanes);
    if (stable) {
        for (int natural = 0; natural < m_orderedLanes; ++natural) {
            if (perm[static_cast<std::size_t>(natural)] !=
                m_laneOrder[static_cast<std::size_t>(natural)]) {
                stable = false;
                break;
            }
        }
    }

    // Rows already laid out carry display lanes; the new ones carry natural
    // lanes. `toNatural` turns the former back so the new order can be applied.
    const auto toNatural = [this](int display) {
        return display >= 0 && display < static_cast<int>(m_laneOrderInv.size())
                   ? m_laneOrderInv[static_cast<std::size_t>(display)]
                   : display;
    };
    const auto move = [&perm](int natural) {
        return natural >= 0 && natural < static_cast<int>(perm.size())
                   ? perm[static_cast<std::size_t>(natural)]
                   : natural;
    };
    // Rows share their lane sets, so each set is remapped once and handed to every
    // row that held it. The original is kept in the cache beside its remap: were
    // it freed, a later set could be allocated at its address and be mistaken for it.
    using Remapped = std::unordered_map<const void*, std::pair<LaneSet, LaneSet>>;
    Remapped fromDisplayCache;
    Remapped fromNaturalCache;
    const auto remap = [&](GraphNode& node, bool fromDisplay) {
        const auto lane = [&](int value) {
            return move(fromDisplay ? toNatural(value) : value);
        };
        const auto remapSet = [&](const LaneSet& set) {
            Remapped& cache = fromDisplay ? fromDisplayCache : fromNaturalCache;
            const auto hit = cache.find(set.identity());
            if (hit != cache.end()) {
                return hit->second.second;
            }
            std::vector<GraphLane> lanes(set.begin(), set.end());
            for (GraphLane& l : lanes) {
                l.lane = lane(l.lane);
            }
            LaneSet moved(std::move(lanes));
            cache.emplace(set.identity(), std::make_pair(set, moved));
            return moved;
        };
        node.lane = lane(node.lane);
        for (GraphLane& l : node.converging) {
            l.lane = lane(l.lane);
        }
        node.activeLanesAbove = remapSet(node.activeLanesAbove);
        node.activeLanesBelow = remapSet(node.activeLanesBelow);
        for (GraphEdge& e : node.edges) {
            e.fromLane = lane(e.fromLane);
            e.toLane = lane(e.toLane);
        }
        // The extent is a maximum over lanes, and a permutation is not monotonic,
        // so it is recomputed from the remapped lanes rather than moved.
        node.edgeColumnMax = maxLaneOf(node);
    };

    if (!stable) {
        for (std::size_t i = 0; i < firstNewRow; ++i) {
            remap(m_nodes[i], true);
        }
    }
    for (std::size_t i = firstNewRow; i < m_nodes.size(); ++i) {
        remap(m_nodes[i], false);
    }

    m_laneOrderInv.assign(static_cast<std::size_t>(m_laneCount), 0);
    for (int natural = 0; natural < m_laneCount; ++natural) {
        m_laneOrderInv[static_cast<std::size_t>(perm[static_cast<std::size_t>(natural)])] =
            natural;
    }
    m_laneOrder = std::move(perm);
    m_orderedLanes = m_laneCount;
}

} // namespace gk
