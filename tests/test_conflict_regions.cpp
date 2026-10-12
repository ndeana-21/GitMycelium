#include "core/conflict_regions.h"

#include <QtTest>

using namespace gk;

namespace {

std::string marked(const std::string& ours, const std::string& base, const std::string& theirs) {
    return "<<<<<<< ours\n" + ours + "||||||| base\n" + base + "=======\n" + theirs +
           ">>>>>>> theirs\n";
}

/// A block git writes when the conflict style is `merge`, with no base section.
std::string markedMergeStyle(const std::string& ours, const std::string& theirs) {
    return "<<<<<<< HEAD\n" + ours + "=======\n" + theirs + ">>>>>>> feature\n";
}

QString joined(const std::vector<std::string>& lines) {
    QString out;
    for (const std::string& line : lines) {
        out += QString::fromStdString(line);
    }
    return out;
}

} // namespace

/// The region parser is pure text: it says what the regions are, and nothing about
/// how to settle them. The editor leans on it for every card it draws.
class ConflictRegionsTest : public QObject {
    Q_OBJECT

private slots:
    void aRegionIsSplitFromItsSurroundingText();
    void twoRegionsAreNumberedInOrder();
    void mergeStyleWithoutBaseStillParses();
    void anAbsentSideIsEmpty();
    void unbalancedMarkersAreReportedAsSuch();
    void textWithoutConflictsHasNoRegions();
    void carriageReturnsSurvive();
    void rawKeepsTheBlockByteForByte();
    void perLineTakesOnlyTheSelectedLines();
    void perLineOrderFollowsTheSideChosenFirst();
    void anEmptyLineSelectionLeavesTheRegionOpen();
    void aWholeSideChoiceIgnoresTheLineMask();
    void anUnbalancedRegionIsNeverResolved();
    void aHeadingUnderlineIsTextNotASeparator();
    void longerMarkersAreReadAtTheLengthTheBlockOpenedWith();
    void theSafeMarkerSizeOutgrowsEveryRunInTheText();
};

void ConflictRegionsTest::aRegionIsSplitFromItsSurroundingText() {
    const std::string file =
        "before\n" + marked("ours\n", "base\n", "theirs\n") + "after\n";

    const ConflictDocument doc = parseConflictDocument(file);

    QCOMPARE(doc.pieces.size(), std::size_t(3));
    QCOMPARE(doc.regions.size(), std::size_t(1));
    QVERIFY(!doc.pieces[0].isRegion);
    QCOMPARE(joined(doc.pieces[0].lines), QStringLiteral("before\n"));
    QVERIFY(doc.pieces[1].isRegion);
    QCOMPARE(doc.pieces[1].region, std::size_t(0));
    QVERIFY(!doc.pieces[2].isRegion);
    QCOMPARE(joined(doc.pieces[2].lines), QStringLiteral("after\n"));

    const ConflictRegion& region = doc.regions[0];
    QVERIFY(region.hasBase);
    QVERIFY(region.balanced);
    QCOMPARE(joined(region.ours), QStringLiteral("ours\n"));
    QCOMPARE(joined(region.base), QStringLiteral("base\n"));
    QCOMPARE(joined(region.theirs), QStringLiteral("theirs\n"));
}

void ConflictRegionsTest::twoRegionsAreNumberedInOrder() {
    const std::string file = marked("a1\n", "b1\n", "c1\n") + "middle\n" +
                             marked("a2\n", "b2\n", "c2\n");

    const ConflictDocument doc = parseConflictDocument(file);

    QCOMPARE(doc.regions.size(), std::size_t(2));
    QCOMPARE(doc.pieces.size(), std::size_t(3));
    QCOMPARE(doc.pieces[1].region, std::size_t(0));
    QCOMPARE(doc.pieces[2].region, std::size_t(1));
    QCOMPARE(joined(doc.regions[1].theirs), QStringLiteral("c2\n"));
    QVERIFY(doc.balanced());
}

void ConflictRegionsTest::mergeStyleWithoutBaseStillParses() {
    const ConflictDocument doc = parseConflictDocument(markedMergeStyle("mine\n", "theirs\n"));

    QCOMPARE(doc.regions.size(), std::size_t(1));
    QVERIFY(!doc.regions[0].hasBase);
    QVERIFY(doc.regions[0].base.empty());
    QCOMPARE(joined(doc.regions[0].ours), QStringLiteral("mine\n"));
    QCOMPARE(joined(doc.regions[0].theirs), QStringLiteral("theirs\n"));
}

void ConflictRegionsTest::anAbsentSideIsEmpty() {
    // A delete/modify conflict: one side has no lines at all.
    const ConflictDocument doc = parseConflictDocument(marked("", "base\n", "kept\n"));

    QCOMPARE(doc.regions.size(), std::size_t(1));
    QVERIFY(doc.regions[0].ours.empty());
    QCOMPARE(joined(doc.regions[0].theirs), QStringLiteral("kept\n"));
}

void ConflictRegionsTest::unbalancedMarkersAreReportedAsSuch() {
    const std::string file = "<<<<<<< ours\nkept\n=======\nright\n";

    const ConflictDocument doc = parseConflictDocument(file);

    QCOMPARE(doc.regions.size(), std::size_t(1));
    QVERIFY(!doc.regions[0].balanced);
    QVERIFY(!doc.balanced());
}

void ConflictRegionsTest::textWithoutConflictsHasNoRegions() {
    const ConflictDocument doc = parseConflictDocument("plain\nfile\n");

    QVERIFY(doc.regions.empty());
    QCOMPARE(doc.pieces.size(), std::size_t(1));
    QVERIFY(doc.balanced());
}

void ConflictRegionsTest::carriageReturnsSurvive() {
    const std::string file = marked("ours\r\n", "base\r\n", "theirs\r\n");

    const ConflictDocument doc = parseConflictDocument(file);

    // The line ending is part of the line: writing the file back must not turn
    // CRLF into LF.
    QCOMPARE(doc.regions[0].ours[0], std::string("ours\r\n"));
}

void ConflictRegionsTest::rawKeepsTheBlockByteForByte() {
    const std::string block = marked("ours\n", "base\n", "theirs\n");
    const ConflictDocument doc = parseConflictDocument(block);

    QCOMPARE(joined(doc.regions[0].raw), QString::fromStdString(block));
}

void ConflictRegionsTest::perLineTakesOnlyTheSelectedLines() {
    const ConflictDocument doc =
        parseConflictDocument(marked("a1\na2\n", "base\n", "b1\nb2\n"));
    const ConflictRegion& region = doc.regions[0];

    RegionLineTake take;
    take.resize(region);
    take.toggle(true, 1);
    take.toggle(false, 0);

    QCOMPARE(joined(regionChosenLines(region, RegionLines, take)), QStringLiteral("a2\nb1\n"));
    QVERIFY(regionResolved(region, RegionLines, take));
}

void ConflictRegionsTest::perLineOrderFollowsTheSideChosenFirst() {
    const ConflictDocument doc =
        parseConflictDocument(marked("a1\na2\n", "base\n", "b1\nb2\n"));
    const ConflictRegion& region = doc.regions[0];

    RegionLineTake take;
    take.resize(region);
    take.toggle(true, 1);
    take.toggle(false, 0);
    take.oursFirst = false;

    QCOMPARE(joined(regionChosenLines(region, RegionLines, take)), QStringLiteral("b1\na2\n"));
}

void ConflictRegionsTest::anEmptyLineSelectionLeavesTheRegionOpen() {
    const ConflictDocument doc =
        parseConflictDocument(marked("a1\n", "base\n", "b1\n"));
    const ConflictRegion& region = doc.regions[0];

    RegionLineTake take;
    take.resize(region);

    QVERIFY(!take.anyTaken());
    QVERIFY(regionChosenLines(region, RegionLines, take).empty());
    // Nothing taken is not an answer: the region must still be counted unresolved.
    QVERIFY(!regionResolved(region, RegionLines, take));
    QVERIFY(!regionResolved(region, RegionUnanswered, take));
}

void ConflictRegionsTest::aWholeSideChoiceIgnoresTheLineMask() {
    const ConflictDocument doc =
        parseConflictDocument(marked("a1\na2\n", "base\n", "b1\n"));
    const ConflictRegion& region = doc.regions[0];

    RegionLineTake take;
    take.resize(region);
    take.toggle(false, 0); // a stray line take must not leak into a whole-side answer

    QCOMPARE(joined(regionChosenLines(region, RegionOurs, take)), QStringLiteral("a1\na2\n"));
    QVERIFY(regionResolved(region, RegionOurs, take));
    QVERIFY(regionResolved(region, RegionBoth, take));
    QVERIFY(!regionChosenLines(region, RegionUnanswered, take).size());
}

void ConflictRegionsTest::anUnbalancedRegionIsNeverResolved() {
    const ConflictDocument doc = parseConflictDocument("<<<<<<< ours\nkept\n=======\nright\n");
    const ConflictRegion& region = doc.regions[0];

    RegionLineTake take;
    take.resize(region);
    take.takeAll(true, true);

    QVERIFY(!regionResolved(region, RegionOurs, take));
    QVERIFY(!regionResolved(region, RegionLines, take));
}

void ConflictRegionsTest::aHeadingUnderlineIsTextNotASeparator() {
    const ConflictDocument doc = parseConflictDocument(
        "<<<<<<< HEAD\nOur Title\n================\n||||||| base\nTitle\n=====\n"
        "=======\nTheir Title\n================\n>>>>>>> other\n");
    QCOMPARE(doc.regions.size(), std::size_t(1));
    const ConflictRegion& region = doc.regions[0];
    QVERIFY(region.balanced);
    QCOMPARE(joined(region.ours), QStringLiteral("Our Title\n================\n"));
    QCOMPARE(joined(region.base), QStringLiteral("Title\n=====\n"));
    QCOMPARE(joined(region.theirs), QStringLiteral("Their Title\n================\n"));
}

void ConflictRegionsTest::longerMarkersAreReadAtTheLengthTheBlockOpenedWith() {
    const ConflictDocument doc = parseConflictDocument(
        "<<<<<<<< ours\nHead\n=======\n|||||||| base\nOld\n========\nNew\n=======\n"
        ">>>>>>>> theirs\n");
    QCOMPARE(doc.regions.size(), std::size_t(1));
    const ConflictRegion& region = doc.regions[0];
    QVERIFY(region.balanced);
    QCOMPARE(joined(region.ours), QStringLiteral("Head\n=======\n"));
    QCOMPARE(joined(region.base), QStringLiteral("Old\n"));
    QCOMPARE(joined(region.theirs), QStringLiteral("New\n=======\n"));
}

void ConflictRegionsTest::theSafeMarkerSizeOutgrowsEveryRunInTheText() {
    QCOMPARE(safeConflictMarkerSize({"plain\ntext\n"}), kDefaultConflictMarkerSize);
    QCOMPARE(safeConflictMarkerSize({"a\n", "Title\n==========\n"}), std::size_t(11));
    QCOMPARE(conflictMarkerOf("================\n"), ConflictMarker::None);
    QCOMPARE(conflictMarkerOf("=======\r\n"), ConflictMarker::Split);
    QCOMPARE(conflictMarkerOf("<<<<<<<\n"), ConflictMarker::Ours);
    QCOMPARE(conflictMarkerOf("<<<<<<<x\n"), ConflictMarker::None);
}

QTEST_MAIN(ConflictRegionsTest)
#include "test_conflict_regions.moc"
