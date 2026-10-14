#include "graph/graph_layout.h"

#include <QtTest>

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace gk;

namespace {

CommitInfo mk(const std::string& id, std::vector<std::string> parents) {
    CommitInfo c;
    c.id = id;
    c.summary = id;
    c.parents = std::move(parents);
    return c;
}

std::string cid(int i) {
    return "c" + std::to_string(i);
}

std::vector<CommitInfo> makeGraph(int n) {
    std::vector<CommitInfo> commits;
    commits.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        commits.push_back(mk(cid(i), {}));
    }
    unsigned seed = 0x1234567u;
    const auto rnd = [&seed]() {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 16) & 0x7fff;
    };
    for (int i = 0; i < n; ++i) {
        const int remaining = n - i - 1;
        if (remaining <= 0) {
            continue;
        }
        const int wanted = (remaining >= 2 && rnd() % 4 == 0) ? 2 : 1;
        std::set<int> chosen;
        for (int p = 0; p < wanted; ++p) {
            const int at = i + 1 + static_cast<int>(rnd() % static_cast<unsigned>(remaining));
            if (chosen.insert(at).second) {
                commits[static_cast<std::size_t>(i)].parents.push_back(cid(at));
            }
        }
    }
    return commits;
}

void compareLayouts(const GraphLayout& a, const GraphLayout& b) {
    QCOMPARE(a.size(), b.size());
    QCOMPARE(a.laneCount(), b.laneCount());
    for (std::size_t i = 0; i < a.size(); ++i) {
        const GraphNode& x = a.node(i);
        const GraphNode& y = b.node(i);
        QCOMPARE(x.lane, y.lane);
        QCOMPARE(x.colorIndex, y.colorIndex);
        QCOMPARE(x.colorColumn, y.colorColumn);
        QCOMPARE(x.edgeColumnMax, y.edgeColumnMax);
        QCOMPARE(x.isMerge, y.isMerge);
        QCOMPARE(x.edges.size(), y.edges.size());
        for (std::size_t e = 0; e < x.edges.size(); ++e) {
            QCOMPARE(x.edges[e].fromLane, y.edges[e].fromLane);
            QCOMPARE(x.edges[e].toLane, y.edges[e].toLane);
            QCOMPARE(x.edges[e].colorIndex, y.edges[e].colorIndex);
            QCOMPARE(x.edges[e].toRow, y.edges[e].toRow);
        }
        QCOMPARE(x.converging.size(), y.converging.size());
        for (std::size_t c = 0; c < x.converging.size(); ++c) {
            QCOMPARE(x.converging[c].lane, y.converging[c].lane);
            QCOMPARE(x.converging[c].colorIndex, y.converging[c].colorIndex);
        }
        QCOMPARE(x.activeLanesAbove.size(), y.activeLanesAbove.size());
        for (std::size_t c = 0; c < x.activeLanesAbove.size(); ++c) {
            QCOMPARE(x.activeLanesAbove[c].lane, y.activeLanesAbove[c].lane);
            QCOMPARE(x.activeLanesAbove[c].colorIndex, y.activeLanesAbove[c].colorIndex);
        }
        QCOMPARE(x.activeLanesBelow.size(), y.activeLanesBelow.size());
        for (std::size_t c = 0; c < x.activeLanesBelow.size(); ++c) {
            QCOMPARE(x.activeLanesBelow[c].lane, y.activeLanesBelow[c].lane);
            QCOMPARE(x.activeLanesBelow[c].colorIndex, y.activeLanesBelow[c].colorIndex);
        }
        // The folded view must agree too: it is derived from the fields above, so a
        // drift here would mean the derivation, not the layout, is wrong.
        const std::vector<GraphRowEdge> xe = rowEdgesOf(x);
        const std::vector<GraphRowEdge> ye = rowEdgesOf(y);
        QCOMPARE(xe.size(), ye.size());
        for (std::size_t c = 0; c < xe.size(); ++c) {
            QCOMPARE(xe[c].lane, ye[c].lane);
            QCOMPARE(xe[c].role, ye[c].role);
            QCOMPARE(xe[c].colorIndex, ye[c].colorIndex);
            QCOMPARE(xe[c].toRow, ye[c].toRow);
        }
    }
}

} // namespace

class GraphLayoutTest : public QObject {
    Q_OBJECT

private slots:
    void emptyLayoutHasNoLanes() {
        GraphLayout layout;
        QCOMPARE(layout.size(), static_cast<std::size_t>(0));
        QCOMPARE(layout.laneCount(), 0);
    }

    void theLayoutDerivesCommitAndMergeTypes() {
        GraphLayout layout;
        layout.setCommits({mk("m", {"a", "b"}), mk("a", {"r"}), mk("b", {"r"}), mk("r", {})});
        QCOMPARE(layout.node(0).type, GraphNodeType::Merge);
        QCOMPARE(layout.node(1).type, GraphNodeType::Commit);
        QCOMPARE(layout.node(3).type, GraphNodeType::Commit);
        QCOMPARE(QString::fromLatin1(graphNodeTypeName(layout.node(0).type)),
                 QStringLiteral("merge"));
    }

    void aHintOverridesTheDerivedTypeWithoutMovingLanes() {
        GraphLayout layout;
        layout.setCommits({mk("m", {"a", "b"}), mk("a", {"r"}), mk("b", {"r"}), mk("r", {})});
        const int lane = layout.node(0).lane;
        const int color = layout.node(0).colorIndex;

        layout.setNodeTypeHints({{"m", GraphNodeType::Stash}, {"a", GraphNodeType::WorkDir}});

        // The stash commit is a merge of HEAD and the index, so the walk still saw
        // two parents — the hint changes what it is drawn as, not how it is laid
        // out. A row with no hint keeps what the parents said.
        QCOMPARE(layout.node(0).type, GraphNodeType::Stash);
        QVERIFY(layout.node(0).isMerge);
        QCOMPARE(layout.node(1).type, GraphNodeType::WorkDir);
        QCOMPARE(layout.node(2).type, GraphNodeType::Commit);
        QCOMPARE(layout.node(0).lane, lane);
        QCOMPARE(layout.node(0).colorIndex, color);
    }

    void aForkStaysOnTheMoreImportantLine() {
        // `feature` (checked out, newest) was cut from `main` at `base`. Both lines
        // await `base`; it continues `main`'s, and `feature` bends into it.
        GraphLayout layout;
        layout.setTipPriority({{"main2", 0}, {"feat2", 1}});
        layout.setCommits({mk("feat2", {"feat1"}), mk("main2", {"main1"}), mk("feat1", {"base"}),
                           mk("main1", {"base"}), mk("base", {})});

        const int mainLane = layout.node(1).lane;
        const int featureLane = layout.node(0).lane;
        QCOMPARE(mainLane, 0);
        QVERIFY(featureLane != mainLane);
        QCOMPARE(layout.node(4).lane, mainLane);
        QCOMPARE(layout.node(4).converging.size(), static_cast<std::size_t>(1));
        QCOMPARE(layout.node(4).converging.front().lane, featureLane);
    }

    void aHintIsAppliedToRowsPagedInLater() {
        GraphLayout layout;
        layout.append({mk("m", {"a", "b"}), mk("a", {"r"})});
        layout.setNodeTypeHints({{"r", GraphNodeType::Stash}});
        layout.append({mk("b", {"r"}), mk("r", {})});
        QCOMPARE(layout.node(3).type, GraphNodeType::Stash);
    }

    void theRowExtentAndColourAreDerivedFromItsLines() {
        GraphLayout layout;
        layout.setCommits({mk("m", {"a", "b"}), mk("a", {"r"}), mk("b", {"r"}), mk("r", {})});
        // A priority change permutes lanes; the extent and the colour column have to
        // come out right on the other side of it.
        layout.setTipPriority({{"b", 0}, {"m", 5}, {"a", 100}});

        for (std::size_t i = 0; i < layout.size(); ++i) {
            const GraphNode& node = layout.node(i);
            int furthest = node.lane;
            for (const GraphRowEdge& edge : rowEdgesOf(node)) {
                furthest = std::max(furthest, edge.lane);
            }
            QCOMPARE(node.edgeColumnMax, furthest);
            QVERIFY(node.edgeColumnMax >= node.lane);
            // The colour is a per-row snapshot, not a property of the lane.
            QCOMPARE(node.colorColumn, node.colorIndex);
        }
    }

    void aTimelineRowTakesNoLaneAndTheLinesCarryThrough() {
        GraphLayout layout;
        // The hint has to be known before the walk, or the separator would be laid
        // out as a root commit and take a lane of its own.
        layout.setNodeTypeHints({{"sep", GraphNodeType::Timeline}});
        layout.setCommits({mk("c2", {"c1"}), mk("sep", {}), mk("c1", {})});

        QCOMPARE(layout.size(), static_cast<std::size_t>(3));
        QCOMPARE(layout.node(1).type, GraphNodeType::Timeline);
        // Both real commits stay in lane 0: the separator invented no lane.
        QCOMPARE(layout.node(0).lane, 0);
        QCOMPARE(layout.node(2).lane, 0);
        QCOMPARE(layout.laneCount(), 1);
        // The line runs through the separator rather than stopping at it.
        QVERIFY(!layout.node(1).activeLanesBelow.empty());
        QCOMPARE(layout.node(1).activeLanesAbove.size(), layout.node(1).activeLanesBelow.size());
        QVERIFY(layout.node(1).edges.empty());
        QVERIFY(layout.node(1).converging.empty());
    }

    void linearHistoryStaysInOneLane() {
        GraphLayout layout;
        layout.setCommits({mk("c3", {"c2"}), mk("c2", {"c1"}), mk("c1", {})});

        QCOMPARE(layout.laneCount(), 1);
        for (std::size_t i = 0; i < layout.size(); ++i) {
            QCOMPARE(layout.node(i).lane, 0);
        }
        // A straight line needs no curve: the first parent is reached by the
        // lane carrying on downwards, which the delegate draws from
        // activeLanesBelow.
        QVERIFY(layout.node(0).edges.empty());
        QVERIFY(layout.node(0).converging.empty());
        QCOMPARE(layout.node(0).activeLanesBelow.size(), static_cast<std::size_t>(1));
        QCOMPARE(layout.node(0).activeLanesBelow[0].lane, 0);
        QVERIFY(!layout.node(0).isMerge);
        QVERIFY(layout.node(2).edges.empty());
        QVERIFY(layout.node(2).activeLanesBelow.empty());
    }

    void mergeCommitSpansTwoLanes() {
        GraphLayout layout;
        layout.setCommits({
            mk("m", {"a", "b"}),
            mk("a", {"root"}),
            mk("b", {"root"}),
            mk("root", {}),
        });

        QCOMPARE(layout.laneCount(), 2);
        QCOMPARE(layout.node(0).lane, 0);
        QCOMPARE(layout.node(1).lane, 0);
        QCOMPARE(layout.node(2).lane, 1);
        QCOMPARE(layout.node(3).lane, 0);

        QVERIFY(layout.node(0).isMerge);
        // Exactly one curve leaves the merge: the second parent's line is born
        // there. The first parent continues straight down lane 0.
        QCOMPARE(layout.node(0).edges.size(), static_cast<std::size_t>(1));
        QCOMPARE(layout.node(0).edges[0].fromLane, 0);
        QCOMPARE(layout.node(0).edges[0].toLane, 1);
        QCOMPARE(layout.node(0).edges[0].toRow, 2);

        // The side branch runs down lane 1 and meets lane 0 at the root; the
        // meeting is recorded on the root, not as a curve up at the merge.
        QVERIFY(layout.node(2).edges.empty());
        QCOMPARE(layout.node(3).converging.size(), static_cast<std::size_t>(1));
        QCOMPARE(layout.node(3).converging[0].lane, 1);
        QCOMPARE(layout.node(3).lane, 0);
    }

    void unloadedParentContinuesStraightDown() {
        GraphLayout layout;
        layout.setCommits({mk("head", {"not-loaded-yet"})});

        QCOMPARE(layout.size(), static_cast<std::size_t>(1));
        QCOMPARE(layout.node(0).lane, 0);
        // The parent is not loaded, so the line simply continues downwards and
        // the next page picks it up. It must not be dropped.
        QCOMPARE(layout.node(0).activeLanesBelow.size(), static_cast<std::size_t>(1));
        QCOMPARE(layout.node(0).activeLanesBelow[0].lane, 0);
        QVERIFY(layout.node(0).edges.empty());
        // The lane is still reserved for the missing parent.
        QCOMPARE(layout.laneCount(), 1);
    }

    void appendingPagesMatchesSingleShotLayout() {
        const std::vector<CommitInfo> all = {
            mk("m", {"a", "b"}),
            mk("a", {"root"}),
            mk("b", {"root"}),
            mk("root", {}),
        };

        GraphLayout oneShot;
        oneShot.setCommits(all);

        GraphLayout paged;
        paged.append({all[0], all[1]});
        const int lanesAfterFirstPage = paged.laneCount();
        QCOMPARE(lanesAfterFirstPage, 2);
        paged.append({all[2], all[3]});

        QCOMPARE(paged.size(), oneShot.size());
        QCOMPARE(paged.laneCount(), oneShot.laneCount());
        for (std::size_t i = 0; i < oneShot.size(); ++i) {
            QCOMPARE(paged.node(i).lane, oneShot.node(i).lane);
            QCOMPARE(paged.node(i).edges.size(), oneShot.node(i).edges.size());
            for (std::size_t e = 0; e < oneShot.node(i).edges.size(); ++e) {
                QCOMPARE(paged.node(i).edges[e].toLane, oneShot.node(i).edges[e].toLane);
                QCOMPARE(paged.node(i).edges[e].toRow, oneShot.node(i).edges[e].toRow);
            }
        }
    }

    void resetClearsEverything() {
        GraphLayout layout;
        layout.setCommits({mk("c1", {})});
        QCOMPARE(layout.size(), static_cast<std::size_t>(1));
        layout.reset();
        QCOMPARE(layout.size(), static_cast<std::size_t>(0));
        QCOMPARE(layout.laneCount(), 0);
    }

    void incrementalPagesMatchOneShotOnAWideGraph() {
        const std::vector<CommitInfo> all = makeGraph(60);
        std::unordered_map<std::string, int> colors;
        std::unordered_map<std::string, int> priority;
        for (int i = 0; i < 60; i += 5) {
            colors[cid(i)] = i % 7;
            priority[cid(i)] = i % 4;
        }

        GraphLayout oneShot;
        oneShot.setTipColors(colors);
        oneShot.setTipPriority(priority);
        oneShot.setCommits(all);

        GraphLayout paged;
        paged.setTipColors(colors);
        paged.setTipPriority(priority);
        for (std::size_t at = 0; at < all.size(); at += 3) {
            const std::size_t end = std::min(all.size(), at + 3);
            paged.append(std::vector<CommitInfo>(all.begin() + static_cast<std::ptrdiff_t>(at),
                                                 all.begin() + static_cast<std::ptrdiff_t>(end)));
        }

        compareLayouts(oneShot, paged);
    }

    void aParentInALaterPageResolvesItsEdge() {
        GraphLayout layout;
        // The merge's second parent lives two rows below, on the next page.
        layout.append({mk("m", {"a", "b"}), mk("a", {"root"})});
        QCOMPARE(layout.node(0).edges.size(), static_cast<std::size_t>(1));
        QCOMPARE(layout.node(0).edges[0].toRow, -1);

        layout.append({mk("b", {"root"}), mk("root", {})});
        QCOMPARE(layout.node(0).edges[0].toRow, 2);
    }

    void rowEdgesFoldTheFieldsIntoOneRolePerLane() {
        GraphLayout layout;
        layout.setCommits(
            {mk("m", {"a", "b"}), mk("a", {"root"}), mk("b", {"root"}), mk("root", {})});

        // At the merge both lanes begin: lane 0 is the commit's own line running
        // on, lane 1 is the born second-parent line that curves out.
        const std::vector<GraphRowEdge> mergeRow = rowEdgesOf(layout.node(0));
        QCOMPARE(mergeRow.size(), static_cast<std::size_t>(2));
        QCOMPARE(mergeRow[0].lane, 0);
        QCOMPARE(mergeRow[0].role, GraphEdgeRole::Starting);
        QCOMPARE(mergeRow[0].toRow, -1);
        QCOMPARE(mergeRow[1].lane, 1);
        QCOMPARE(mergeRow[1].role, GraphEdgeRole::Starting);
        QCOMPARE(mergeRow[1].toRow, 2);

        // The side branch's rows are straight lines through their lanes.
        for (const std::size_t row : {std::size_t(1), std::size_t(2)}) {
            const std::vector<GraphRowEdge> edges = rowEdgesOf(layout.node(row));
            QCOMPARE(edges.size(), static_cast<std::size_t>(2));
            for (const GraphRowEdge& edge : edges) {
                QCOMPARE(edge.role, GraphEdgeRole::PassThrough);
            }
        }

        // Both branches end at the root; lane 1 is the one that met lane 0 there.
        const std::vector<GraphRowEdge> rootRow = rowEdgesOf(layout.node(3));
        QCOMPARE(rootRow.size(), static_cast<std::size_t>(2));
        QCOMPARE(rootRow[0].role, GraphEdgeRole::Ending);
        QCOMPARE(rootRow[1].role, GraphEdgeRole::Ending);
        QCOMPARE(rootRow[1].lane, 1);
    }
};

QTEST_MAIN(GraphLayoutTest)
#include "test_graph_layout.moc"
