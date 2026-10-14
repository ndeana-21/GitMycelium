#include "ui/diff_view.h"

#include <QSignalSpy>
#include <QTextEdit>
#include <QToolButton>
#include <QtTest>

using namespace gk;

namespace {

FileDiff diffWithHunks(int hunks) {
    FileDiff diff;
    diff.oldPath = "file.txt";
    diff.newPath = "file.txt";
    for (int i = 0; i < hunks; ++i) {
        DiffHunk hunk;
        hunk.oldStart = i * 10 + 1;
        hunk.oldLines = 1;
        hunk.newStart = i * 10 + 1;
        hunk.newLines = 1;

        DiffLine context;
        context.kind = DiffLineKind::Context;
        context.text = "unchanged line";
        context.oldLine = i * 10 + 1;
        context.newLine = i * 10 + 1;

        DiffLine removed;
        removed.kind = DiffLineKind::Removed;
        removed.text = "old text number " + std::to_string(i);
        removed.oldLine = i * 10 + 2;

        DiffLine added;
        added.kind = DiffLineKind::Added;
        added.text = "new text number " + std::to_string(i);
        added.newLine = i * 10 + 2;

        hunk.lines = {context, removed, added};
        diff.hunks.push_back(hunk);
        diff.added += 1;
        diff.removed += 1;
    }
    return diff;
}

DiffView::Document documentWith(int hunks) {
    DiffView::Document document;
    document.path = QStringLiteral("file.txt");
    document.diff = diffWithHunks(hunks);
    document.revisionContent = "new text number 0\n";
    document.hasRevision = true;
    document.contentLabel = QStringLiteral("HEAD");
    return document;
}

} // namespace

class DiffViewTest : public QObject {
    Q_OBJECT

private slots:
    void countsEveryHunk();
    void arrowWalksForwardAndWraps();
    void arrowWalksBackwardFromTheEnd();
    void switchingToFileViewHidesTheChanges();
    void hunkNavigationDoesNothingWithoutHunks();
    void stagingControlReportsTheHunk();
    void stagingControlReportsALine();
    void aCommitsDiffOffersNoStaging();
    void splitViewKeepsTheTwoPanesAligned();
    void splitViewShowsAMessageWhenThereIsNoLineDiff();
    void discardControlReportsTheHunk();
    void wrapCanBeToggled();
    void theMinimapMarksChangesAndHidesInTheFileView();
    void theExternalButtonReportsThePath();
};

void DiffViewTest::countsEveryHunk() {
    DiffView view;
    view.show(documentWith(3));
    QCOMPARE(view.changeCount(), 3);
    QCOMPARE(view.currentChange(), -1);
}

void DiffViewTest::arrowWalksForwardAndWraps() {
    DiffView view;
    view.show(documentWith(3));

    view.goToNextChange();
    QCOMPARE(view.currentChange(), 0);
    view.goToNextChange();
    QCOMPARE(view.currentChange(), 1);
    view.goToNextChange();
    QCOMPARE(view.currentChange(), 2);
    view.goToNextChange();
    QCOMPARE(view.currentChange(), 0);
}

void DiffViewTest::arrowWalksBackwardFromTheEnd() {
    DiffView view;
    view.show(documentWith(2));

    view.goToPreviousChange();
    QCOMPARE(view.currentChange(), 1);
    view.goToPreviousChange();
    QCOMPARE(view.currentChange(), 0);
    view.goToPreviousChange();
    QCOMPARE(view.currentChange(), 1);
}

void DiffViewTest::switchingToFileViewHidesTheChanges() {
    DiffView view;
    view.show(documentWith(2));
    QCOMPARE(view.changeCount(), 2);

    view.setFileView(true);
    view.goToNextChange();
    QCOMPARE(view.currentChange(), -1);

    view.setFileView(false);
    view.goToNextChange();
    QCOMPARE(view.currentChange(), 0);
}

void DiffViewTest::stagingControlReportsTheHunk() {
    DiffView view;
    DiffView::Document document = documentWith(1);
    document.staging = DiffView::Staging::Stage;
    view.show(document);

    QSignalSpy spy(&view, &DiffView::changeStagedRequested);
    view.requestHunkStaging(0);
    QCOMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("file.txt"));
    QCOMPARE(args.at(1).toInt(), 0);
    QCOMPARE(args.at(2).value<QVector<int>>().size(), 0);
    QCOMPARE(args.at(3).toBool(), true);
}

void DiffViewTest::stagingControlReportsALine() {
    DiffView view;
    DiffView::Document document = documentWith(1);
    document.staging = DiffView::Staging::Stage;
    view.show(document);

    QSignalSpy spy(&view, &DiffView::changeStagedRequested);
    view.requestLineStaging(0, 2);
    QCOMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(2).value<QVector<int>>(), QVector<int>{2});
    QCOMPARE(args.at(3).toBool(), true);
}

void DiffViewTest::aCommitsDiffOffersNoStaging() {
    DiffView view;
    view.show(documentWith(1)); // no staging side: a commit's diff

    QSignalSpy spy(&view, &DiffView::changeStagedRequested);
    view.requestHunkStaging(0);
    QCOMPARE(spy.count(), 0);
}

void DiffViewTest::splitViewKeepsTheTwoPanesAligned() {
    DiffView view;
    view.resize(800, 400);
    view.show(documentWith(3));
    view.setSplitView(true);
    QVERIFY(view.splitView());

    // Every aligned row is one line on each side; if the panes disagree, a change
    // would not sit across from its counterpart and the view is worthless.
    const int left = view.splitPaneBlocks(0);
    QVERIFY(left > 0);
    QCOMPARE(view.splitPaneBlocks(1), left);

    view.goToNextChange();
    QCOMPARE(view.currentChange(), 0);

    view.setSplitView(false);
    QVERIFY(!view.splitView());
}

void DiffViewTest::splitViewShowsAMessageWhenThereIsNoLineDiff() {
    DiffView view;
    DiffView::Document binary = documentWith(1);
    binary.diff.binary = true;
    binary.diff.hunks.clear();

    view.setSplitView(true);
    view.show(binary);
    QVERIFY(view.splitView());
    QVERIFY(view.splitPaneBlocks(0) >= 1);
}

void DiffViewTest::discardControlReportsTheHunk() {
    DiffView view;
    DiffView::Document document = documentWith(1);
    document.staging = DiffView::Staging::Stage;
    document.offerDiscard = true;
    view.show(document);

    QSignalSpy spy(&view, &DiffView::discardRequested);
    view.requestHunkDiscard(0);
    QCOMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("file.txt"));
    QCOMPARE(args.at(1).toInt(), 0);

    DiffView plain;
    plain.show(documentWith(1));
    QSignalSpy none(&plain, &DiffView::discardRequested);
    plain.requestHunkDiscard(0);
    QCOMPARE(none.count(), 0);
}

void DiffViewTest::wrapCanBeToggled() {
    DiffView view;
    view.show(documentWith(1));
    QVERIFY(!view.wrapEnabled());
    view.setWrap(true);
    QVERIFY(view.wrapEnabled());
    view.setWrap(false);
    QVERIFY(!view.wrapEnabled());
}

void DiffViewTest::theMinimapMarksChangesAndHidesInTheFileView() {
    DiffView view;
    view.show(documentWith(2));
    QVERIFY(view.minimapVisible());
    QVERIFY(view.minimapMarkCount() > 0);

    view.setFileView(true);
    QVERIFY(!view.minimapVisible());

    view.setFileView(false);
    QVERIFY(view.minimapVisible());
}

void DiffViewTest::hunkNavigationDoesNothingWithoutHunks() {
    DiffView view;
    DiffView::Document document;
    document.path = QStringLiteral("empty.txt");
    view.show(document);

    QCOMPARE(view.changeCount(), 0);
    view.goToNextChange();
    QCOMPARE(view.currentChange(), -1);
}

void DiffViewTest::theExternalButtonReportsThePath() {
    DiffView view;
    view.show(documentWith(1));
    QSignalSpy requested(&view, &DiffView::externalDiffRequested);
    auto* button = view.findChild<QToolButton*>(QStringLiteral("externalDiff"));
    QVERIFY(button);
    button->click();
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.first().first().toString(), QStringLiteral("file.txt"));
}

QTEST_MAIN(DiffViewTest)
#include "test_diff_view.moc"
