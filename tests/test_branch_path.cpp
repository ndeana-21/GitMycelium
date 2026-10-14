/// The traced branch of a selected commit: up its own lane to the tip, then down the
/// first-parent chain into the branches it was cut from, each from the fork down.

#include "graph/branch_path.h"
#include "graph/graph_layout.h"

#include <QtTest>

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

struct Traced {
    GraphLayout layout;
    std::vector<CommitInfo> commits;
    BranchPath path;

    std::vector<std::string> nodeIds() const {
        std::vector<std::string> ids;
        for (int row : path.nodeRows()) {
            ids.push_back(commits[static_cast<std::size_t>(row)].id);
        }
        return ids;
    }
};

Traced trace(std::vector<CommitInfo> commits, const std::string& anchor) {
    Traced t;
    t.commits = std::move(commits);
    t.layout.setCommits(t.commits);
    std::unordered_map<std::string, int> rowOf;
    int anchorRow = -1;
    for (int row = 0; row < static_cast<int>(t.commits.size()); ++row) {
        rowOf[t.commits[static_cast<std::size_t>(row)].id] = row;
        if (t.commits[static_cast<std::size_t>(row)].id == anchor) {
            anchorRow = row;
        }
    }
    t.path = BranchPath::trace(
        anchorRow, static_cast<int>(t.commits.size()),
        [&t](int row) { return &t.layout.node(static_cast<std::size_t>(row)); },
        [&t, &rowOf](int row) {
            const auto& parents = t.commits[static_cast<std::size_t>(row)].parents;
            if (parents.empty()) {
                return -1;
            }
            const auto found = rowOf.find(parents.front());
            return found == rowOf.end() ? BranchPath::kNotLoaded : found->second;
        });
    return t;
}

} // namespace

class BranchPathTest : public QObject {
    Q_OBJECT

private slots:
    void aBranchIsTracedFromItsTipIntoEveryBranchItWasCutFrom();
    void theForkIsWhereTheLineBendsIntoTheParentBranch();
    void aMergedBranchStopsBelowTheMerge();
    void aParentNotPagedInRunsOnPastTheLastRow();
};

void BranchPathTest::aBranchIsTracedFromItsTipIntoEveryBranchItWasCutFrom() {
    // main A-B-C-D; feature1 cut from B: E-F; feature2 cut from E: G-H.
    const Traced t = trace({mk("H", {"G"}), mk("F", {"E"}), mk("D", {"C"}), mk("G", {"E"}),
                            mk("E", {"B"}), mk("C", {"B"}), mk("B", {"A"}), mk("A", {})},
                           "G");
    QCOMPARE(t.nodeIds(), (std::vector<std::string>{"H", "G", "E", "B", "A"}));
    QCOMPARE(t.path.tipRow(), 0);
    QVERIFY(!t.path.hasNode(1)); // F: feature1 above the fork is not the path
    QVERIFY(!t.path.hasNode(2)); // D
    QVERIFY(!t.path.hasNode(5)); // C
}

void BranchPathTest::theForkIsWhereTheLineBendsIntoTheParentBranch() {
    const Traced t = trace({mk("H", {"G"}), mk("F", {"E"}), mk("D", {"C"}), mk("G", {"E"}),
                            mk("E", {"B"}), mk("C", {"B"}), mk("B", {"A"}), mk("A", {})},
                           "H");
    // H's line runs through F and D in its own lane...
    for (int row : {1, 2}) {
        const auto pieces = t.path.piecesAt(row);
        QCOMPARE(pieces.size(), std::size_t{1});
        QCOMPARE(pieces.front().fromRow, 0);
        QVERIFY(pieces.front().top && pieces.front().bottom);
    }
    // ...and at E the line from G ends while E's own line to B begins. Whichever of the
    // two lanes E took, the delegate bends the other into it.
    const auto atFork = t.path.piecesAt(4);
    QCOMPARE(atFork.size(), std::size_t{2});
    QCOMPARE(atFork[0].fromRow, 3);
    QVERIFY(atFork[0].top && !atFork[0].bottom);
    QCOMPARE(atFork[1].fromRow, 4);
    QVERIFY(!atFork[1].top && atFork[1].bottom);
    // C sits on main beside E's line down to B: not on the path, but the line from E
    // to B crosses its row.
    QVERIFY(!t.path.hasNode(5));
    QCOMPARE(t.path.piecesAt(5).size(), std::size_t{1});
    QCOMPARE(t.path.piecesAt(5).front().fromRow, 4);
}

void BranchPathTest::aMergedBranchStopsBelowTheMerge() {
    // M merges feature (E-F, cut from B) into main.
    const Traced t = trace({mk("M", {"D", "F"}), mk("F", {"E"}), mk("D", {"B"}), mk("E", {"B"}),
                            mk("B", {})},
                           "E");
    QCOMPARE(t.nodeIds(), (std::vector<std::string>{"F", "E", "B"}));
    QVERIFY(t.path.piecesAt(0).empty());
}

void BranchPathTest::aParentNotPagedInRunsOnPastTheLastRow() {
    const Traced t = trace({mk("F", {"E"}), mk("D", {"C"})}, "F");
    QCOMPARE(t.nodeIds(), (std::vector<std::string>{"F"}));
    QCOMPARE(t.path.segments().size(), std::size_t{1});
    QCOMPARE(t.path.segments().front().toRow, BranchPath::kNotLoaded);
    const auto below = t.path.piecesAt(1);
    QCOMPARE(below.size(), std::size_t{1});
    QVERIFY(below.front().top && below.front().bottom);
}

QTEST_MAIN(BranchPathTest)
#include "test_branch_path.moc"
