#include "graph/graph_layout.h"
#include "ui/ref_pill_layout.h"
#include "ui/refs_delegate.h"

#include <QFont>
#include <QImage>
#include <QPixmap>
#include <QSet>
#include <QtTest>

#include <algorithm>

using namespace gk;

namespace {

CommitInfo commit(const std::string& id, const std::string& summary,
                  const std::vector<std::string>& parents) {
    CommitInfo ci;
    ci.id = id;
    ci.summary = summary;
    ci.parents = parents;
    return ci;
}

/// Lanes reported below one row must be exactly the lanes reported above the
/// next one, otherwise the per-row painter leaves a gap in the line.
bool connectsTo(const GraphNode& above, const GraphNode& below) {
    std::vector<int> a;
    for (const GraphLane& l : above.activeLanesBelow) {
        a.push_back(l.lane);
    }
    std::vector<int> b;
    for (const GraphLane& l : below.activeLanesAbove) {
        b.push_back(l.lane);
    }
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

} // namespace

class GraphAndRefsTest : public QObject {
    Q_OBJECT

private slots:
    void everyBranchTipGetsItsOwnLaneAndColour();
    void importantBranchGetsTheLeftmostLane();
    void tipColourIsCarriedDownTheBranchLine();
    void consecutiveRowsAgreeOnLiveLanes();
    void mergeCommitSpansTwoLanesInDifferentColours();

    void pillsStayInsideTheCell();
    void anIconlessArrangementGivesTheWidthBackToTheName();
    void branchesOnOneCommitCollapseIntoOnePill();
    void aLocalBranchAndItsRemoteShareOnePill();
    void aFoldedBranchStandsForItsRemoteTwin();
    void theCheckedOutBranchNamesThePill();
    void theFocusedBranchNamesThePill();
    void aLocalBranchBeatsARemoteOne();
    void tagsKeepTheirOwnPill();
    void tooManyLabelsCollapseIntoABadge();
    void hitTestFindsTheGrabbedLabel();
    void dragChipIsActuallyDrawn();
};

void GraphAndRefsTest::everyBranchTipGetsItsOwnLaneAndColour() {
    // Two branches from one base, newest-first, the way a revwalk over all refs
    // returns them: tipA, tipB, base.
    std::vector<CommitInfo> commits{
        commit("a1", "tip of branch A", {"base"}),
        commit("b1", "tip of branch B", {"base"}),
        commit("base", "root", {}),
    };

    GraphLayout layout;
    layout.setTipColors({{"a1", 0}, {"b1", 3}});
    layout.setCommits(commits);

    QCOMPARE(layout.size(), static_cast<std::size_t>(3));
    QVERIFY(layout.laneCount() >= 2);

    const GraphNode& tipA = layout.node(0);
    const GraphNode& tipB = layout.node(1);
    QVERIFY(tipA.lane != tipB.lane);
    QCOMPARE(tipA.colorIndex, 0);
    QCOMPARE(tipB.colorIndex, 3);
}

void GraphAndRefsTest::importantBranchGetsTheLeftmostLane() {
    // Three unrelated branches. The walk order (a, b, c) says nothing about
    // which one the user cares about, so priority has to decide.
    std::vector<CommitInfo> commits{
        commit("a1", "tip A", {"base"}),
        commit("b1", "tip B", {"base"}),
        commit("c1", "tip C", {"base"}),
        commit("base", "root", {}),
    };

    GraphLayout layout;
    // B is the current branch, C is focused, A is nobody's favourite.
    layout.setTipPriority({{"b1", 0}, {"c1", 1}, {"a1", 2}});
    layout.setCommits(commits);

    const int laneB = layout.node(1).lane;
    const int laneC = layout.node(2).lane;
    const int laneA = layout.node(0).lane;

    QCOMPARE(laneB, 0); // the branch you are on is leftmost, always visible
    QVERIFY(laneC > laneB);
    QVERIFY(laneA > laneC);

    // The trunk everyone descends from keeps the leftmost lane for itself at
    // the bottom, so the line you follow stays straight.
    QVERIFY(layout.laneCount() >= 3);
}

void GraphAndRefsTest::tipColourIsCarriedDownTheBranchLine() {
    std::vector<CommitInfo> commits{
        commit("tip", "tip", {"mid"}),
        commit("mid", "mid", {"base"}),
        commit("base", "base", {}),
    };

    GraphLayout layout;
    layout.setTipColors({{"tip", 7}});
    layout.setCommits(commits);

    // A straight branch keeps one colour all the way down; a line that changed
    // colour halfway would not match its label.
    for (std::size_t i = 0; i < layout.size(); ++i) {
        QCOMPARE(layout.node(i).colorIndex, 7);
    }
}

void GraphAndRefsTest::consecutiveRowsAgreeOnLiveLanes() {
    std::vector<CommitInfo> commits{
        commit("m3", "merge", {"a2", "b1"}),
        commit("a2", "a2", {"a1"}),
        commit("b1", "b1", {"base"}),
        commit("a1", "a1", {"base"}),
        commit("base", "base", {}),
    };

    GraphLayout layout;
    layout.setCommits(commits);

    for (std::size_t i = 1; i < layout.size(); ++i) {
        QVERIFY2(connectsTo(layout.node(i - 1), layout.node(i)),
                 qPrintable(QStringLiteral("разрыв линий между строками %1 и %2")
                                .arg(i - 1)
                                .arg(i)));
    }
}

void GraphAndRefsTest::mergeCommitSpansTwoLanesInDifferentColours() {
    std::vector<CommitInfo> commits{
        commit("merge", "merge", {"a1", "b1"}),
        commit("a1", "a1", {"base"}),
        commit("b1", "b1", {"base"}),
        commit("base", "base", {}),
    };

    GraphLayout layout;
    layout.setTipColors({{"merge", 1}, {"b1", 5}});
    layout.setCommits(commits);

    const GraphNode& node = layout.node(0);
    QVERIFY(node.isMerge);
    // One line is born at the merge (the second parent); the first parent is
    // the same line carrying on downwards.
    QCOMPARE(node.edges.size(), static_cast<std::size_t>(1));
    QVERIFY(node.edges.at(0).toLane != node.lane);

    // Both parents are reachable, and the side branch meets the trunk at the
    // base commit — that is the visible "where it was merged" marker.
    QCOMPARE(layout.node(3).converging.size(), static_cast<std::size_t>(1));
    QCOMPARE(layout.node(3).converging.at(0).lane, node.edges.at(0).toLane);
}

void GraphAndRefsTest::pillsStayInsideTheCell() {
    const QRect cell(0, 0, 220, 30);
    // A branch and a tag: two pills, so the stacking geometry is exercised.
    const auto pills = RefPillLayout::arrange({QStringLiteral("main"), QStringLiteral("v1.0")},
                                              {0, 2}, {0, 2}, {}, cell);

    QCOMPARE(pills.size(), static_cast<std::size_t>(2));
    for (const RefPill& pill : pills) {
        QVERIFY2(cell.contains(pill.rect.topLeft()),
                 qPrintable(QStringLiteral("плашка вылезла за строку: %1")
                                .arg(pill.rect.topLeft().y())));
        QVERIFY(pill.rect.right() <= cell.right());
    }
    QVERIFY(pills.at(1).rect.top() >= pills.at(0).rect.bottom());
    QCOMPARE(pills.at(0).colorIndex, 0);
    QCOMPARE(pills.at(1).colorIndex, 2);
}

void GraphAndRefsTest::anIconlessArrangementGivesTheWidthBackToTheName() {
    // A name long enough to be elided inside the cell, so removing the glyph really
    // changes how much of it survives.
    const QRect cell(0, 0, 220, 30);
    const QStringList labels{QStringLiteral("feature/a-very-long-branch-name")};
    const QVariantList colors{0};
    const QVariantList kinds{0}; // local branch

    const auto withIcons = RefPillLayout::arrange(labels, colors, kinds, {}, cell, QString(),
                                                  QString(), /*showIcon=*/true);
    const auto withoutIcons = RefPillLayout::arrange(labels, colors, kinds, {}, cell, QString(),
                                                     QString(), /*showIcon=*/false);

    QVERIFY(!withIcons.empty() && !withoutIcons.empty());
    QVERIFY(!withIcons.front().iconRect.isNull());
    QVERIFY(withoutIcons.front().iconRect.isNull());
    // The room the glyph held went to the name, so more of it survives the cell.
    QVERIFY(withoutIcons.front().textRect.width() > withIcons.front().textRect.width());
}

void GraphAndRefsTest::branchesOnOneCommitCollapseIntoOnePill() {
    const QRect cell(0, 0, 220, 30);
    const auto pills = RefPillLayout::arrange(
        {QStringLiteral("main"), QStringLiteral("origin/main"), QStringLiteral("feature/x")},
        {0, 1, 2}, {0, 1, 0}, {}, cell);

    // One place to be, said once, with the count of the *other logical* branches on
    // it. `origin/main` is `main`'s remote counterpart, not a second branch.
    QCOMPARE(pills.size(), static_cast<std::size_t>(1));
    QCOMPARE(pills.at(0).text, QStringLiteral("main +1"));
    QVERIFY(pills.at(0).hasRemote);
    // The name itself stays whole: it is what a drag carries and a click resolves.
    QCOMPARE(pills.at(0).shortName, QStringLiteral("main"));
    // The other branch is there for the hover expansion; `origin/main` is not, it is
    // the cloud on this pill.
    QCOMPARE(pills.at(0).collapsedNames, QStringList({QStringLiteral("feature/x")}));
}

void GraphAndRefsTest::aLocalBranchAndItsRemoteShareOnePill() {
    const QRect cell(0, 0, 220, 30);
    const auto pills = RefPillLayout::arrange(
        {QStringLiteral("main"), QStringLiteral("origin/main")}, {0, 1}, {0, 1}, {}, cell);

    QCOMPARE(pills.size(), static_cast<std::size_t>(1));
    QCOMPARE(pills.at(0).shortName, QStringLiteral("main"));
    QCOMPARE(pills.at(0).text, QStringLiteral("main")); // no "+1" for the pair
    QVERIFY(pills.at(0).hasRemote);
    QVERIFY(pills.at(0).iconRect.isValid());
    QVERIFY(pills.at(0).remoteRect.isValid());
    QVERIFY(pills.at(0).collapsedNames.isEmpty());
}

void GraphAndRefsTest::aFoldedBranchStandsForItsRemoteTwin() {
    const QRect cell(0, 0, 220, 30);
    const auto pills = RefPillLayout::arrange(
        {QStringLiteral("main"), QStringLiteral("feature/x"), QStringLiteral("origin/feature/x"),
         QStringLiteral("origin/other")},
        {0, 1, 1, 2}, {0, 0, 1, 1}, {}, cell, QStringLiteral("main"));

    QCOMPARE(pills.size(), static_cast<std::size_t>(1));
    QCOMPARE(pills.at(0).text, QStringLiteral("main +2"));
    QCOMPARE(pills.at(0).collapsedNames,
             QStringList({QStringLiteral("feature/x"), QStringLiteral("origin/other")}));
    QCOMPARE(pills.at(0).collapsedWithRemote, QStringList({QStringLiteral("feature/x")}));
}

void GraphAndRefsTest::theCheckedOutBranchNamesThePill() {
    const QRect cell(0, 0, 220, 30);
    const auto pills = RefPillLayout::arrange(
        {QStringLiteral("main"), QStringLiteral("origin/main")}, {0, 1}, {0, 1}, {}, cell,
        QStringLiteral("main"));

    // Where the reader is standing is the branch they mean, shown once with its
    // remote counterpart rather than counted as another branch.
    QCOMPARE(pills.size(), static_cast<std::size_t>(1));
    QCOMPARE(pills.at(0).shortName, QStringLiteral("main"));
    QVERIFY(pills.at(0).isHead);
    QVERIFY(pills.at(0).hasRemote);
    QCOMPARE(pills.at(0).text, QStringLiteral("main"));
}

void GraphAndRefsTest::theFocusedBranchNamesThePill() {
    const QRect cell(0, 0, 220, 30);
    // No checked-out branch on this commit, so the one being focused wins over the
    // one that merely sorts first.
    const auto pills = RefPillLayout::arrange(
        {QStringLiteral("main"), QStringLiteral("feature/x")}, {0, 2}, {0, 0}, {}, cell, QString(),
        QStringLiteral("feature/x"));

    QCOMPARE(pills.size(), static_cast<std::size_t>(1));
    QCOMPARE(pills.at(0).shortName, QStringLiteral("feature/x"));
    QCOMPARE(pills.at(0).text, QStringLiteral("feature/x +1"));
}

void GraphAndRefsTest::aLocalBranchBeatsARemoteOne() {
    const QRect cell(0, 0, 220, 30);
    // Nothing checked out and nothing focused: a local branch is a place the working
    // tree can stand, a remote-tracking one is not.
    const auto pills = RefPillLayout::arrange(
        {QStringLiteral("origin/main"), QStringLiteral("main")}, {1, 0}, {1, 0}, {}, cell);

    QCOMPARE(pills.size(), static_cast<std::size_t>(1));
    QCOMPARE(pills.at(0).shortName, QStringLiteral("main"));
}

void GraphAndRefsTest::tagsKeepTheirOwnPill() {
    const QRect cell(0, 0, 220, 30);
    const auto pills = RefPillLayout::arrange(
        {QStringLiteral("main"), QStringLiteral("origin/main"), QStringLiteral("v1.0")}, {0, 1, 2},
        {0, 1, 2}, {}, cell);

    // The local branch and its remote fold into one; the tag keeps saying what it says.
    QCOMPARE(pills.size(), static_cast<std::size_t>(2));
    QCOMPARE(pills.at(0).text, QStringLiteral("main"));
    QVERIFY(pills.at(0).hasRemote);
    QCOMPARE(pills.at(1).shortName, QStringLiteral("v1.0"));
    QCOMPARE(pills.at(1).text, QStringLiteral("v1.0"));
}

void GraphAndRefsTest::tooManyLabelsCollapseIntoABadge() {
    const QRect cell(0, 0, 220, 30);
    const auto pills = RefPillLayout::arrange(
        {QStringLiteral("main"), QStringLiteral("v1.0"), QStringLiteral("v1.1"),
         QStringLiteral("v1.2")},
        {0, 1, 2, 3}, {0, 2, 2, 2}, {}, cell);

    QCOMPARE(pills.size(), static_cast<std::size_t>(3)); // two pills + badge
    QVERIFY(!pills.at(0).overflow);
    QVERIFY(!pills.at(1).overflow);
    QVERIFY(pills.at(2).overflow);
    QCOMPARE(pills.at(2).hiddenCount, 2);
    QCOMPARE(pills.at(2).text, QStringLiteral("+2"));
}

void GraphAndRefsTest::hitTestFindsTheGrabbedLabel() {
    const QRect cell(0, 0, 220, 30);
    // A branch and a tag, so there really are two pills to tell apart.
    const auto pills = RefPillLayout::arrange({QStringLiteral("main"), QStringLiteral("v1.0")},
                                              {0, 2}, {0, 2}, {}, cell);
    QVERIFY(pills.size() == 2);

    // Dragging must resolve the label the cursor is actually on: this is the
    // whole reason painting and hit-testing share one geometry.
    const RefPill* first = RefPillLayout::hitTest(pills, pills.at(0).rect.center());
    QVERIFY(first != nullptr);
    QCOMPARE(first->shortName, QStringLiteral("main"));

    const RefPill* second = RefPillLayout::hitTest(pills, pills.at(1).rect.center());
    QVERIFY(second != nullptr);
    QCOMPARE(second->shortName, QStringLiteral("v1.0"));

    QVERIFY(RefPillLayout::hitTest(pills, QPoint(219, 15)) == nullptr);
}

void GraphAndRefsTest::dragChipIsActuallyDrawn() {
    // Qt shows nothing while dragging unless the QDrag carries a pixmap. A chip
    // that comes back blank is exactly the "I am dragging something invisible"
    // bug, so assert that real pixels were painted, not just that an object
    // exists.
    const QPixmap chip =
        RefsDelegate::dragPixmap(QStringLiteral("feature/login"), 2, RefKind::LocalBranch);

    QVERIFY(!chip.isNull());
    QVERIFY(chip.deviceIndependentSize().width() > 40);
    QVERIFY(chip.deviceIndependentSize().height() > 10);

    const QImage image = chip.toImage();
    QSet<QRgb> opaqueColours;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qAlpha(pixel) > 0) {
                opaqueColours.insert(pixel);
            }
        }
    }
    // Fill, border and text: a blank chip would yield at most one colour.
    QVERIFY2(opaqueColours.size() > 1,
             qPrintable(QStringLiteral("чип перетаскивания пустой, цветов: %1")
                            .arg(opaqueColours.size())));
}

QTEST_MAIN(GraphAndRefsTest)
#include "test_graph_refs.moc"
