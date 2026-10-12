#include "core/conflict_auto.h"

#include <QtTest>

using namespace gk;

namespace {

std::string marked(const std::string& ours, const std::string& base, const std::string& theirs) {
    return "<<<<<<< ours\n" + ours + "||||||| base\n" + base + "=======\n" + theirs + ">>>>>>> theirs\n";
}

} // namespace

class ConflictAutoTest : public QObject {
    Q_OBJECT

private slots:
    void bothSidesSayingTheSameThingIsSettled() {
        // The classic false conflict: both branches made the same edit.
        const std::string file = marked("same\nline\n", "base\n", "same\nline\n");

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(1));
        QCOMPARE(resolved.leftCount, std::size_t(0));
        QCOMPARE(QString::fromStdString(resolved.content), QStringLiteral("same\nline\n"));
        QCOMPARE(resolved.hunks.at(0).reason, AutoReason::Identical);
    }

    void anUntouchedSideLosesTheConflict() {
        // Our side is exactly the base: nothing of ours to defend, so the incoming
        // change simply lands. This is the hunk that makes rebases miserable.
        const std::string file = marked("keep\n", "keep\n", "keep\nand more\n");

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(1));
        QCOMPARE(QString::fromStdString(resolved.content), QStringLiteral("keep\nand more\n"));
        QCOMPARE(resolved.hunks.at(0).reason, AutoReason::OnlyTheirsChanged);
    }

    void ourChangeSurvivesWhenTheirsIsTheBase() {
        const std::string file = marked("ours changed\n", "base\n", "base\n");

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(1));
        QCOMPARE(QString::fromStdString(resolved.content), QStringLiteral("ours changed\n"));
        QCOMPARE(resolved.hunks.at(0).reason, AutoReason::OnlyOursChanged);
    }

    void aDeletionOnTheIncomingSideIsSettled() {
        const std::string file = marked("gone\n", "gone\n", "");

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(1));
        QVERIFY(resolved.content.empty());
    }

    void trailingWhitespaceIsNotADisagreement() {
        const std::string file = marked("value = 1;\t\n", "value = 0;\n", "value = 1;   \n");

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(1));
        QCOMPARE(resolved.hunks.at(0).reason, AutoReason::TrailingSpace);
        QCOMPARE(QString::fromStdString(resolved.content), QStringLiteral("value = 1;\t\n"));
    }

    void aRebaseStepDoesNotTakeTheReplayedSnapshotWholesale() {
        // The shape a drop produces: our side is exactly the base, and their side is the
        // snapshot of the commit being replayed — which still contains context from the
        // commit the reader dropped. In a merge that is a tidy resolution; in a rebase it
        // quietly puts the dropped work back.
        const std::string file = marked("v1\n", "v1\n", "v1\nv2\nv3\n");

        AutoOptions rebase;
        rebase.rebaseStep = true;
        const AutoResolved asRebase = autoResolveMarkedConflicts(file, rebase);
        QCOMPARE(asRebase.resolvedCount, std::size_t(0));
        QCOMPARE(asRebase.leftCount, std::size_t(1));

        // The same three sides in a merge: there both other sides are real work, and
        // taking the side that moved is the ordinary answer.
        const AutoResolved asMerge = autoResolveMarkedConflicts(file);
        QCOMPARE(asMerge.resolvedCount, std::size_t(1));
        QCOMPARE(asMerge.hunks.at(0).reason, AutoReason::OnlyTheirsChanged);
    }

    void theExactDropConflictIsNotSettledDuringARebase() {
        // The real case, per-hunk, from the sweep's own trace: ours empty, base one line,
        // theirs that line plus the replayed commit's. A deletion and an insertion at its
        // edge — the sweep merged them and put the dropped line back.
        const std::string file = marked("", "v2\n", "v2\nv3\n");

        AutoOptions rebase;
        rebase.rebaseStep = true;
        const AutoResolved resolved = autoResolveMarkedConflicts(file, rebase);

        QCOMPARE(resolved.resolvedCount, std::size_t(0));
        QCOMPARE(resolved.leftCount, std::size_t(1));
    }

    void aDeletionIsNotReopenedByAnInsertionAtItsEdge() {
        // One side removes a line; the other adds a line where it stood. Each half reads
        // as a sensible change on its own, and merging them puts the new content on the
        // ground the deletion cleared — the deleted line's place is occupied again. The
        // result looks deliberate and is not; this is a person's call, not ours.
        // The insertion sits exactly where the deleted line was: `x` goes in at the
        // point `b` came out of. (An insertion further along — after another unchanged
        // line — is genuinely disjoint and is settled, which is the point of the rule.)
        const std::string file = marked("a\nc\n", "a\nb\nc\n", "a\nx\nb\nc\n");

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(0));
        QCOMPARE(resolved.leftCount, std::size_t(1));
    }

    void aRealDisagreementKeepsItsMarkers() {
        const std::string file = marked("ours\n", "base\n", "theirs\n");

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(0));
        QCOMPARE(resolved.leftCount, std::size_t(1));
        // Byte for byte, markers and all. Quietly dropping them is how a conflict
        // becomes invisible.
        QCOMPARE(QString::fromStdString(resolved.content), QString::fromStdString(file));
        QCOMPARE(resolved.hunks.at(0).reason, AutoReason::None);
    }

    void hunksAreDecidedOneByOne() {
        const std::string file = "top\n" + marked("ours\n", "base\n", "theirs\n") + "middle\n" +
                                 marked("same\n", "base\n", "same\n") + "bottom\n";

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(1));
        QCOMPARE(resolved.leftCount, std::size_t(1));
        QVERIFY(hasConflictMarkers(resolved.content)); // one hunk is still open
        QVERIFY(resolved.content.find("same\n") != std::string::npos);
        QVERIFY(resolved.content.find("<<<<<<<") != std::string::npos);
    }

    void withoutABaseNothingIsAssumed() {
        // Merge style, no base section: "our side is unchanged" cannot be known, so
        // the hunk is not touched even when it looks decidable.
        const std::string file = "<<<<<<< ours\nkeep\n=======\nkeep\nand more\n>>>>>>> theirs\n";

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(0));
        QCOMPARE(resolved.leftCount, std::size_t(1));
        QCOMPARE(QString::fromStdString(resolved.content), QString::fromStdString(file));
    }

    void differentLinesInOneBlockAreMerged() {
        // The exact shape a rebase produces for adjacent edits: git calls it a
        // conflict because its hunks are wider than the changes in them. Nothing
        // here is contested — the two sides changed different lines.
        const std::string file = marked("b\nC\n", "b\nc\n", "B\nc\n");

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(1));
        QCOMPARE(resolved.hunks.at(0).reason, AutoReason::DisjointChanges);
        QCOMPARE(QString::fromStdString(resolved.content), QStringLiteral("B\nC\n"));
    }

    void anInsertionClearOfTheOtherChangeIsMerged() {
        const std::string file = marked("a\nx\nb\n", "a\nb\n", "a\nB\n");

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.hunks.at(0).reason, AutoReason::DisjointChanges);
        QCOMPARE(QString::fromStdString(resolved.content), QStringLiteral("a\nx\nB\n"));
    }

    void aDeletionClearOfTheOtherChangeIsMerged() {
        const std::string file = marked("a\nc\n", "a\nb\nc\n", "a\nb\nC\n");

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.hunks.at(0).reason, AutoReason::DisjointChanges);
        QCOMPARE(QString::fromStdString(resolved.content), QStringLiteral("a\nC\n"));
    }

    void theSameLineChangedBothWaysIsStillAConflict() {
        // The one thing that is a real disagreement: both sides rewrote the same
        // line, differently. Nothing may be decided here.
        const std::string file = marked("base\nours\n", "base\nthird\n", "base\ntheirs\n");

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(0));
        QCOMPARE(resolved.leftCount, std::size_t(1));
        QCOMPARE(QString::fromStdString(resolved.content), QString::fromStdString(file));
    }

    void twoInsertionsAtTheSamePlaceAreAConflict() {
        // Both sides added a line in the same spot. Which comes first is a
        // judgement, so this stays a conflict.
        const std::string file = marked("a\nours\nb\n", "a\nb\n", "a\ntheirs\nb\n");

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(0));
        QCOMPARE(resolved.leftCount, std::size_t(1));
    }

    void textWithoutConflictsIsUntouched() {
        const std::string file = "one\ntwo\nthree\n";

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(0));
        QCOMPARE(resolved.leftCount, std::size_t(0));
        QCOMPARE(QString::fromStdString(resolved.content), QString::fromStdString(file));
        QVERIFY(!hasConflictMarkers(resolved.content));
    }

    void unbalancedMarkersAreLeftExactlyAsTheyAre() {
        const std::string file = "one\n<<<<<<< ours\nours\n=======\ntheirs\n";

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.leftCount, std::size_t(1));
        QCOMPARE(QString::fromStdString(resolved.content), QString::fromStdString(file));
    }

    void carriageReturnsSurvive() {
        // A file with CRLF endings must come back with CRLF endings: rewriting the
        // whole file's line endings is a change nobody asked for.
        const std::string file = "<<<<<<< ours\r\nsame\r\n||||||| base\r\nx\r\n=======\r\nsame\r\n>>>>>>> theirs\r\n";

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(1));
        QCOMPARE(QString::fromStdString(resolved.content), QStringLiteral("same\r\n"));
    }

    void markersAreRecognisedAnywhereInTheFile() {
        QVERIFY(hasConflictMarkers("a\nb\n<<<<<<< ours\nc\n"));
        QVERIFY(!hasConflictMarkers("a\nb\nc\n"));
        QVERIFY(!hasConflictMarkers(""));
    }

    void leftoverBaseOrSplitHalvesAreStillMarkers() {
        // What the old parser saved for an RST heading: the opening marker gone, the
        // base half and the separator left behind.
        QVERIFY(hasConflictMarkers("Intro\n||||||| doc.rst\nTitle\n=======\nNew\n"));
        QVERIFY(hasConflictMarkers("a\n>>>>>>>> theirs\n"));
        QVERIFY(!hasConflictMarkers("Title\n=======\n"));
        QVERIFY(!hasConflictMarkers("Title\n================\n"));
        QVERIFY(!hasConflictMarkers("a <<<<<<< b\n"));
    }

    void aHeadingUnderlineInsideABlockDoesNotSplitIt() {
        const std::string file = "<<<<<<< ours\nTitle\n================\n||||||| base\n"
                                 "Title\n=====\n=======\nTitle\n================\n"
                                 ">>>>>>> theirs\n";

        const AutoResolved resolved = autoResolveMarkedConflicts(file);

        QCOMPARE(resolved.resolvedCount, std::size_t(1));
        QCOMPARE(QString::fromStdString(resolved.content),
                 QStringLiteral("Title\n================\n"));
    }
};

QTEST_MAIN(ConflictAutoTest)
#include "test_conflict_auto.moc"
