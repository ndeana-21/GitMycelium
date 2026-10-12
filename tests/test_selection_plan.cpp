#include "core/selection_plan.h"

#include <QtTest>

using namespace gk;

namespace {

LineageRow row(const char* id, std::initializer_list<const char*> parents = {}) {
    LineageRow r;
    r.id = id;
    for (const char* parent : parents) {
        r.parents.emplace_back(parent);
    }
    return r;
}

std::vector<Oid> ids(std::initializer_list<const char*> list) {
    std::vector<Oid> out;
    for (const char* id : list) {
        out.emplace_back(id);
    }
    return out;
}

} // namespace

/// The rules that decide what a multi-commit selection may be asked to do. They are the
/// difference between a rewrite that means what the reader meant and one that quietly means
/// something else, so each of them is pinned here.
class SelectionPlanTest : public QObject {
    Q_OBJECT

private slots:
    void theLineFollowsFirstParentsFromTheTip() {
        // tip -> b -> c, a second branch rooted at b
        const std::vector<LineageRow> rows = {
            row("tip", {"b"}),
            row("side", {"b"}),
            row("b", {"c"}),
            row("c"),
        };

        const std::vector<Oid> line = firstParentLineage(rows, "tip");

        QCOMPARE(line, ids({"tip", "b", "c"}));
    }

    void aCommitMergedInFromElsewhereIsNotOnTheLine() {
        // `side` is a parent of the tip as well, but it is not the line the branch replays.
        const std::vector<LineageRow> rows = {
            row("tip", {"b", "side"}),
            row("side", {"c"}),
            row("b", {"c"}),
            row("c"),
        };

        const std::vector<Oid> line = firstParentLineage(rows, "tip");

        QCOMPARE(line, ids({"tip", "b", "c"}));
    }

    void theLineStopsAtTheEdgeOfWhatIsLoaded() {
        // The parent of c is older than the page, so the line ends at c rather than naming
        // a commit below it that happens to be loaded but is not its parent.
        const std::vector<LineageRow> rows = {
            row("tip", {"b"}),
            row("b", {"c"}),
            row("c", {"older-than-the-page"}),
            row("unrelated"),
        };

        const std::vector<Oid> line = firstParentLineage(rows, "tip");

        QCOMPARE(line, ids({"tip", "b", "c"}));
    }

    void aTipThatIsNotLoadedHasNoLine() {
        const std::vector<LineageRow> rows = {row("tip", {"b"}), row("b")};

        QVERIFY(firstParentLineage(rows, "somewhere-else").empty());
    }

    void oneCommitOfThisBranchCanBeDropped() {
        const SelectionPlan plan = planSelection(ids({"b"}), ids({"tip", "b", "c"}), true);

        QVERIFY(plan.allOnCurrentBranch);
        QVERIFY(plan.canDrop);
        QVERIFY(!plan.canSquash);
        QVERIFY(!plan.canCherryPick);
        QCOMPARE(plan.branchCommits, ids({"b"}));
        QVERIFY(!plan.squashRefusal.empty()); // the reason is stated, not hidden
    }

    void consecutiveCommitsOfThisBranchCanBeSquashed() {
        const SelectionPlan plan = planSelection(ids({"b", "c"}), ids({"tip", "b", "c"}), true);

        QVERIFY(plan.canDrop);
        QVERIFY(plan.canSquash);
        QVERIFY(plan.squashRefusal.empty());
        // Oldest first: that is the order a rewrite replays them in.
        QCOMPARE(plan.branchCommits, ids({"c", "b"}));
    }

    void aGapBetweenThemStopsSquash() {
        // tip, b, c, d — selecting b and d leaves c unselected between them.
        const SelectionPlan plan = planSelection(ids({"b", "d"}), ids({"tip", "b", "c", "d"}), true);

        QVERIFY(plan.canDrop);
        QVERIFY(!plan.canSquash);
        QCOMPARE(QString::fromStdString(plan.squashRefusal),
                 QStringLiteral("squash needs commits that follow one another on the branch"));
    }

    void aMergeAboveThemStopsSquash() {
        const SelectionPlan plan = planSelection(ids({"b", "c"}), ids({"tip", "b", "c"}), false);

        QVERIFY(!plan.canSquash);
        QVERIFY(plan.squashRefusal.find("merge") != std::string::npos);
    }

    void commitsFromElsewhereCanOnlyBeCherryPicked() {
        const SelectionPlan plan = planSelection(ids({"side", "side2"}), ids({"tip", "b"}), true);

        QVERIFY(plan.allElsewhere);
        QVERIFY(plan.canCherryPick);
        QVERIFY(!plan.canDrop);
        QVERIFY(!plan.canSquash);
        // Oldest first again: a cherry-pick replays in the order it is handed.
        QCOMPARE(plan.pickedCommits, ids({"side2", "side"}));
    }

    void aMixedSelectionIsRefusedRatherThanGuessed() {
        const SelectionPlan plan = planSelection(ids({"b", "side"}), ids({"tip", "b"}), true);

        QVERIFY(!plan.canDrop);
        QVERIFY(!plan.canSquash);
        QVERIFY(!plan.canCherryPick);
        QVERIFY(plan.refusal.find("mixes") != std::string::npos);
    }

    void anEmptySelectionSaysSo() {
        const SelectionPlan plan = planSelection({}, ids({"tip"}), true);

        QVERIFY(!plan.canDrop);
        QVERIFY(!plan.refusal.empty());
    }
};

QTEST_MAIN(SelectionPlanTest)
#include "test_selection_plan.moc"
