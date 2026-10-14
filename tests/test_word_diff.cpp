#include "model/word_diff.h"

#include <QtTest>

using namespace gk;

namespace {

QString emphasisedText(const QString& line, const QVector<QPair<int, int>>& ranges) {
    QString out;
    for (const QPair<int, int>& range : ranges) {
        out += line.mid(range.first, range.second);
    }
    return out;
}

} // namespace

class WordDiffTest : public QObject {
    Q_OBJECT

private slots:
    void unchangedLinesCarryNoEmphasis();
    void aChangedTokenIsTheOnlyEmphasis();
    void anInsertedLineIsEmphasisedWhole();
    void alignmentHoldsWhenTheReferenceHasAnExtraLine();
    void aReplacementBlockPairsPositionally();
    void anEmptyReferenceEmphasisesEveryLine();
    void blankLinesAreNotEmphasised();
    void tooLargeToAlignIsLeftPlain();
};

void WordDiffTest::unchangedLinesCarryNoEmphasis() {
    const std::vector<QString> side = {QStringLiteral("a"), QStringLiteral("b")};
    const std::vector<QVector<QPair<int, int>>> emphasis =
        emphasisAgainstReference(side, side);
    QCOMPARE(static_cast<int>(emphasis.size()), 2);
    QVERIFY(emphasis[0].isEmpty());
    QVERIFY(emphasis[1].isEmpty());
}

void WordDiffTest::aChangedTokenIsTheOnlyEmphasis() {
    const std::vector<QString> side = {QStringLiteral("int x = 1;")};
    const std::vector<QString> reference = {QStringLiteral("int x = 2;")};
    const std::vector<QVector<QPair<int, int>>> emphasis =
        emphasisAgainstReference(side, reference);

    QCOMPARE(static_cast<int>(emphasis.size()), 1);
    QCOMPARE(emphasis[0].size(), 1);
    QCOMPARE(emphasis[0][0].first, 8); // the "1"
    QCOMPARE(emphasis[0][0].second, 1);
    QCOMPARE(emphasisedText(side[0], emphasis[0]), QStringLiteral("1"));
}

void WordDiffTest::anInsertedLineIsEmphasisedWhole() {
    const std::vector<QString> side = {QStringLiteral("a"), QStringLiteral("b"),
                                       QStringLiteral("NEW"), QStringLiteral("c")};
    const std::vector<QString> reference = {QStringLiteral("a"), QStringLiteral("b"),
                                            QStringLiteral("c")};
    const std::vector<QVector<QPair<int, int>>> emphasis =
        emphasisAgainstReference(side, reference);

    QCOMPARE(static_cast<int>(emphasis.size()), 4);
    QVERIFY(emphasis[0].isEmpty());
    QVERIFY(emphasis[1].isEmpty());
    QCOMPARE(emphasis[2].size(), 1);
    QCOMPARE(emphasis[2][0].first, 0);
    QCOMPARE(emphasis[2][0].second, 3);
    QCOMPARE(emphasisedText(side[2], emphasis[2]), QStringLiteral("NEW"));
    QVERIFY(emphasis[3].isEmpty());
}

void WordDiffTest::alignmentHoldsWhenTheReferenceHasAnExtraLine() {
    // The base has a line the side dropped: the side's "c" must still match the
    // reference's "c" rather than being paired with "b".
    const std::vector<QString> side = {QStringLiteral("a"), QStringLiteral("c")};
    const std::vector<QString> reference = {QStringLiteral("a"), QStringLiteral("b"),
                                            QStringLiteral("c")};
    const std::vector<QVector<QPair<int, int>>> emphasis =
        emphasisAgainstReference(side, reference);

    QCOMPARE(static_cast<int>(emphasis.size()), 2);
    QVERIFY(emphasis[0].isEmpty());
    QVERIFY(emphasis[1].isEmpty());
}

void WordDiffTest::aReplacementBlockPairsPositionally() {
    // Two reference lines replaced by two side lines: each side line is diffed
    // against its own counterpart, not against the block taken as one.
    const std::vector<QString> side = {QStringLiteral("int a = 1;"), QStringLiteral("int b = 2;")};
    const std::vector<QString> reference = {QStringLiteral("int a = 9;"),
                                            QStringLiteral("int b = 8;")};
    const std::vector<QVector<QPair<int, int>>> emphasis =
        emphasisAgainstReference(side, reference);

    QCOMPARE(static_cast<int>(emphasis.size()), 2);
    QCOMPARE(emphasisedText(side[0], emphasis[0]), QStringLiteral("1"));
    QCOMPARE(emphasisedText(side[1], emphasis[1]), QStringLiteral("2"));
}

void WordDiffTest::anEmptyReferenceEmphasisesEveryLine() {
    const std::vector<QString> side = {QStringLiteral("x"), QStringLiteral("y")};
    const std::vector<QVector<QPair<int, int>>> emphasis =
        emphasisAgainstReference(side, {});
    QCOMPARE(static_cast<int>(emphasis.size()), 2);
    QCOMPARE(emphasisedText(side[0], emphasis[0]), QStringLiteral("x"));
    QCOMPARE(emphasisedText(side[1], emphasis[1]), QStringLiteral("y"));
}

void WordDiffTest::blankLinesAreNotEmphasised() {
    const std::vector<QString> side = {QStringLiteral(""), QStringLiteral("x")};
    const std::vector<QString> reference = {QStringLiteral("x")};
    const std::vector<QVector<QPair<int, int>>> emphasis =
        emphasisAgainstReference(side, reference);

    QCOMPARE(static_cast<int>(emphasis.size()), 2);
    QVERIFY(emphasis[0].isEmpty());
    QVERIFY(emphasis[1].isEmpty());
}

void WordDiffTest::tooLargeToAlignIsLeftPlain() {
    // Past the cap the quadratic table is not worth it: everything comes back plain,
    // which the caller draws as it would without emphasis.
    std::vector<QString> side;
    std::vector<QString> reference;
    for (int i = 0; i < 2500; ++i) {
        side.push_back(QStringLiteral("side line %1").arg(i));
        reference.push_back(QStringLiteral("reference line %1").arg(i));
    }
    const std::vector<QVector<QPair<int, int>>> emphasis =
        emphasisAgainstReference(side, reference);

    QCOMPARE(static_cast<int>(emphasis.size()), 2500);
    for (const QVector<QPair<int, int>>& line : emphasis) {
        QVERIFY2(line.isEmpty(), "a file past the cap must come back without emphasis");
    }
}

QTEST_MAIN(WordDiffTest)
#include "test_word_diff.moc"
