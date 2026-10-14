#include "graph/graph_animation.h"

#include <QtTest>

using namespace gk;

namespace {

std::vector<GraphRowKey> rows(std::initializer_list<GraphRowKey> list) {
    return std::vector<GraphRowKey>(list);
}

} // namespace

class GraphAnimationTest : public QObject {
    Q_OBJECT

private slots:
    void theFirstSnapshotIsOnlyABaseline();
    void aNewCommitAppears();
    void aCommitThatIsGoneVanishes();
    void aCommitThatChangedLaneOrRowMoves();
    void aMovedHeadPulses();
    void labelsComeAndGo();
    void transitionsExpireAndActiveStops();
    void clearingForgetsTheBaseline();
};

void GraphAnimationTest::theFirstSnapshotIsOnlyABaseline() {
    GraphAnimation animation;
    animation.observe(rows({{"a", 0, 0}, {"b", 1, 1}}), {}, "a", 1000);
    QVERIFY(animation.empty());
    QVERIFY(!animation.active(1000));
}

void GraphAnimationTest::aNewCommitAppears() {
    GraphAnimation animation;
    animation.observe(rows({{"a", 0, 0}}), {}, "a", 0);
    animation.observe(rows({{"c", 0, 0}, {"a", 1, 0}}), {}, "c", 100);

    const GraphTransition* appear = animation.find("c", GraphMotion::Appear);
    QVERIFY(appear);
    QCOMPARE(appear->durationMs, GraphAnimation::kFadeMs);
    QCOMPARE(GraphAnimation::progress(*appear, 100), 0.0);
    QCOMPARE(GraphAnimation::progress(*appear, 100 + GraphAnimation::kFadeMs / 2), 0.5);
    QVERIFY(animation.find("a", GraphMotion::Move));
}

void GraphAnimationTest::aCommitThatIsGoneVanishes() {
    GraphAnimation animation;
    animation.observe(rows({{"a", 0, 0}, {"b", 1, 1}}), {}, "a", 0);
    animation.observe(rows({{"a", 0, 0}}), {}, "a", 0);
    const GraphTransition* vanish = animation.find("b", GraphMotion::Vanish);
    QVERIFY(vanish);
    QCOMPARE(vanish->fromRow, 1);
    QCOMPARE(vanish->fromLane, 1);
}

void GraphAnimationTest::aCommitThatChangedLaneOrRowMoves() {
    GraphAnimation animation;
    animation.observe(rows({{"a", 0, 0}}), {}, "a", 0);
    animation.observe(rows({{"a", 3, 2}}), {}, "a", 0);
    const GraphTransition* move = animation.find("a", GraphMotion::Move);
    QVERIFY(move);
    QCOMPARE(move->fromRow, 0);
    QCOMPARE(move->fromLane, 0);
    QCOMPARE(move->toRow, 3);
    QCOMPARE(move->toLane, 2);
}

void GraphAnimationTest::aMovedHeadPulses() {
    GraphAnimation animation;
    animation.observe(rows({{"a", 0, 0}, {"b", 1, 0}}), {}, "a", 0);
    animation.observe(rows({{"a", 0, 0}, {"b", 1, 0}}), {}, "b", 0);
    QVERIFY(animation.find("b", GraphMotion::Pulse));
    QVERIFY(animation.find("a", GraphMotion::Move) == nullptr);
}

void GraphAnimationTest::labelsComeAndGo() {
    GraphAnimation animation;
    animation.observe(rows({{"a", 0, 0}}), {"main"}, "a", 0);
    animation.observe(rows({{"a", 0, 0}}), {"main", "feature"}, "a", 0);
    QVERIFY(animation.findLabel("feature", GraphMotion::LabelIn));
    animation.observe(rows({{"a", 0, 0}}), {}, "a", 0);
    QVERIFY(animation.findLabel("feature", GraphMotion::LabelOut));
}

void GraphAnimationTest::transitionsExpireAndActiveStops() {
    GraphAnimation animation;
    animation.observe(rows({{"a", 0, 0}}), {}, "a", 0);
    animation.observe(rows({{"a", 0, 0}, {"b", 1, 0}}), {}, "a", 1000);
    QVERIFY(animation.active(1000));
    QVERIFY(animation.advance(1000));
    QVERIFY(animation.active(1000 + GraphAnimation::kFadeMs - 1));
    QVERIFY(!animation.advance(1000 + GraphAnimation::kMoveMs + 1));
    QVERIFY(animation.empty());
    QVERIFY(!animation.active(1000 + GraphAnimation::kMoveMs + 1));
}

void GraphAnimationTest::clearingForgetsTheBaseline() {
    GraphAnimation animation;
    animation.observe(rows({{"a", 0, 0}}), {}, "a", 0);
    animation.clear();
    animation.observe(rows({{"a", 0, 0}, {"b", 1, 0}}), {}, "a", 0);
    QVERIFY(animation.empty());
}

QTEST_MAIN(GraphAnimationTest)
#include "test_graph_animation.moc"
