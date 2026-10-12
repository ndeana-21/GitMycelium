#include "core/diff_rows.h"

#include <QtTest>

using namespace gk;

namespace {

DiffHunk hunkOf(std::vector<DiffLineKind> kinds) {
    DiffHunk hunk;
    int oldLine = 0;
    int newLine = 0;
    for (DiffLineKind kind : kinds) {
        DiffLine line;
        line.kind = kind;
        line.text = "x";
        if (kind != DiffLineKind::Added) {
            line.oldLine = ++oldLine;
        }
        if (kind != DiffLineKind::Removed) {
            line.newLine = ++newLine;
        }
        hunk.lines.push_back(line);
    }
    return hunk;
}

} // namespace

class DiffRowsTest : public QObject {
    Q_OBJECT

private slots:
    void anEmptyHunkHasNoRows();
    void contextSitsOnBothSides();
    void aPairedChangeSharesOneRow();
    void anExcessRemovalStandsAlone();
    void anExcessAdditionStandsAlone();
    void aChangeBlockWithEqualSidesPairsEveryLine();
    void contextAroundAChangeKeepsItsPlace();
};

void DiffRowsTest::anEmptyHunkHasNoRows() {
    QVERIFY(alignedRows(DiffHunk{}).empty());
}

void DiffRowsTest::contextSitsOnBothSides() {
    const std::vector<DiffRow> rows =
        alignedRows(hunkOf({DiffLineKind::Context, DiffLineKind::Context}));
    QCOMPARE(rows.size(), std::size_t(2));
    QCOMPARE(rows[0].left, 0);
    QCOMPARE(rows[0].right, 0);
    QCOMPARE(rows[1].left, 1);
    QCOMPARE(rows[1].right, 1);
}

void DiffRowsTest::aPairedChangeSharesOneRow() {
    const std::vector<DiffRow> rows =
        alignedRows(hunkOf({DiffLineKind::Removed, DiffLineKind::Added}));
    QCOMPARE(rows.size(), std::size_t(1));
    QCOMPARE(rows[0].left, 0);
    QCOMPARE(rows[0].right, 1);
}

void DiffRowsTest::anExcessRemovalStandsAlone() {
    const std::vector<DiffRow> rows = alignedRows(
        hunkOf({DiffLineKind::Removed, DiffLineKind::Removed, DiffLineKind::Added}));
    QCOMPARE(rows.size(), std::size_t(2));
    QCOMPARE(rows[0].left, 0);
    QCOMPARE(rows[0].right, 2);
    QCOMPARE(rows[1].left, 1);
    QCOMPARE(rows[1].right, -1);
}

void DiffRowsTest::anExcessAdditionStandsAlone() {
    const std::vector<DiffRow> rows = alignedRows(
        hunkOf({DiffLineKind::Removed, DiffLineKind::Added, DiffLineKind::Added}));
    QCOMPARE(rows.size(), std::size_t(2));
    QCOMPARE(rows[0].left, 0);
    QCOMPARE(rows[0].right, 1);
    QCOMPARE(rows[1].left, -1);
    QCOMPARE(rows[1].right, 2);
}

void DiffRowsTest::aChangeBlockWithEqualSidesPairsEveryLine() {
    const std::vector<DiffRow> rows = alignedRows(hunkOf(
        {DiffLineKind::Removed, DiffLineKind::Removed, DiffLineKind::Added, DiffLineKind::Added}));
    QCOMPARE(rows.size(), std::size_t(2));
    QCOMPARE(rows[0].left, 0);
    QCOMPARE(rows[0].right, 2);
    QCOMPARE(rows[1].left, 1);
    QCOMPARE(rows[1].right, 3);
}

void DiffRowsTest::contextAroundAChangeKeepsItsPlace() {
    const std::vector<DiffRow> rows = alignedRows(hunkOf(
        {DiffLineKind::Context, DiffLineKind::Removed, DiffLineKind::Added, DiffLineKind::Context}));
    QCOMPARE(rows.size(), std::size_t(3));
    QCOMPARE(rows[0].left, 0);
    QCOMPARE(rows[0].right, 0);
    QCOMPARE(rows[1].left, 1);
    QCOMPARE(rows[1].right, 2);
    QCOMPARE(rows[2].left, 3);
    QCOMPARE(rows[2].right, 3);
}

QTEST_MAIN(DiffRowsTest)
#include "test_diff_rows.moc"
