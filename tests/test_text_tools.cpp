#include "model/text_encoding.h"
#include "model/word_diff.h"

#include <QtTest>

using namespace gk;

namespace {

QString joined(const std::vector<TextRun>& runs) {
    QString out;
    for (const TextRun& run : runs) {
        out += run.text;
    }
    return out;
}

QStringList changedParts(const std::vector<TextRun>& runs) {
    QStringList out;
    for (const TextRun& run : runs) {
        if (run.changed) {
            out.push_back(run.text);
        }
    }
    return out;
}

std::string bytesOf(const QByteArray& data) {
    return std::string(data.constData(), static_cast<std::size_t>(data.size()));
}

} // namespace

class TextToolsTest : public QObject {
    Q_OBJECT

private slots:
    void detectsPlainUtf8();
    void detectsUtf8FromItsBom();
    void detectsUtf16FromItsBom();
    void fallsBackWhenUtf8IsImpossible();
    void decodesRussianInTheChosenCodec();
    void unknownCodecDegradesToUtf8RatherThanGarbage();
    void decodesNothingFromNothing();

    void identicalLinesHaveNoChangedRuns();
    void oneRenamedWordIsOneChangedRun();
    void punctuationIsComparedToo();
    void aRewrittenLineIsWhollyDifferent();
    void theReassembledLineIsTheOriginal();
};

void TextToolsTest::detectsPlainUtf8() {
    QCOMPARE(TextEncoding::detect("plain ascii\n"), QStringLiteral("UTF-8"));
    // A multi-byte character settles it: this is not CP1251.
    QCOMPARE(TextEncoding::detect(bytesOf(QStringLiteral("привет\n").toUtf8())),
             QStringLiteral("UTF-8"));
}

void TextToolsTest::detectsUtf8FromItsBom() {
    QCOMPARE(TextEncoding::detect(bytesOf(QByteArray("\xEF\xBB\xBFhello"))),
             QStringLiteral("UTF-8"));
}

void TextToolsTest::detectsUtf16FromItsBom() {
    // The mark is the only encoding information the bytes carry, so it wins.
    QCOMPARE(TextEncoding::detect(bytesOf(QByteArray("\xFF\xFEh\0i\0"))),
             QStringLiteral("UTF-16LE"));
    QCOMPARE(TextEncoding::detect(bytesOf(QByteArray("\xFE\xFF\0h\0i"))),
             QStringLiteral("UTF-16BE"));
}

void TextToolsTest::fallsBackWhenUtf8IsImpossible() {
    if (!TextEncoding::isAvailable(QStringLiteral("windows-1251"))) {
        QSKIP("this build of Qt has no windows-1251 codec");
    }
    // CP1251 for "Привет" — invalid UTF-8, so the fallback is what is left.
    const std::string cp1251 = bytesOf(QByteArray("\xCF\xF0\xE8\xE2\xE5\xF2"));
    QCOMPARE(TextEncoding::detect(cp1251, QStringLiteral("windows-1251")),
             QStringLiteral("windows-1251"));
}

void TextToolsTest::decodesRussianInTheChosenCodec() {
    if (!TextEncoding::isAvailable(QStringLiteral("windows-1251"))) {
        QSKIP("this build of Qt has no windows-1251 codec");
    }
    const std::string cp1251 = bytesOf(QByteArray("\xCF\xF0\xE8\xE2\xE5\xF2"));
    QCOMPARE(TextEncoding::decode(cp1251, QStringLiteral("windows-1251")),
             QStringLiteral("Привет"));

    // The same bytes read as UTF-8 are the noise this whole class exists to avoid.
    QVERIFY(TextEncoding::decode(cp1251, QStringLiteral("UTF-8")) != QStringLiteral("Привет"));
}

void TextToolsTest::unknownCodecDegradesToUtf8RatherThanGarbage() {
    const std::string utf8 = bytesOf(QStringLiteral("привет").toUtf8());
    QCOMPARE(TextEncoding::decode(utf8, QStringLiteral("no-such-codec")), QStringLiteral("привет"));
}

void TextToolsTest::decodesNothingFromNothing() {
    QVERIFY(TextEncoding::decode(std::string(), QStringLiteral("UTF-8")).isEmpty());
    // A picker with no real choice would be worse than none: UTF-8 must be there.
    QVERIFY(TextEncoding::codecs().contains(QStringLiteral("UTF-8")));
}

void TextToolsTest::identicalLinesHaveNoChangedRuns() {
    const WordDiff diff = wordDiff(QStringLiteral("int value = 42;"),
                                   QStringLiteral("int value = 42;"));
    QVERIFY(changedParts(diff.before).isEmpty());
    QVERIFY(changedParts(diff.after).isEmpty());
    QVERIFY(!diff.whollyDifferent());
}

void TextToolsTest::oneRenamedWordIsOneChangedRun() {
    const WordDiff diff = wordDiff(QStringLiteral("int count = 0;"),
                                   QStringLiteral("int total = 0;"));
    QCOMPARE(changedParts(diff.before), QStringList({QStringLiteral("count")}));
    QCOMPARE(changedParts(diff.after), QStringList({QStringLiteral("total")}));
}

void TextToolsTest::punctuationIsComparedToo() {
    // One letter and one bracket: three tokens differ, and saying so is the point
    // of comparing tokens instead of whole lines.
    const WordDiff diff = wordDiff(QStringLiteral("call(x)"), QStringLiteral("call(y)"));
    QCOMPARE(changedParts(diff.before), QStringList({QStringLiteral("x")}));
    QCOMPARE(changedParts(diff.after), QStringList({QStringLiteral("y")}));

    const std::vector<TextRun>& after = diff.after;
    QVERIFY(!diff.whollyDifferent());
    QCOMPARE(joined(after), QStringLiteral("call(y)"));
}

void TextToolsTest::aRewrittenLineIsWhollyDifferent() {
    // Nothing but a space in common is not common ground: the line reads as
    // rewritten, and the space between the words is not emphasised.
    const WordDiff diff = wordDiff(QStringLiteral("alpha beta"), QStringLiteral("gamma delta"));
    QVERIFY(diff.whollyDifferent());
    QCOMPARE(changedParts(diff.before),
             QStringList({QStringLiteral("alpha"), QStringLiteral("beta")}));

    // Neighbouring runs of the same kind are merged: four tokens with nothing in
    // common are one span each side, not four the painter has to draw.
    const WordDiff merged = wordDiff(QStringLiteral("a++b"), QStringLiteral("c--d"));
    QCOMPARE(merged.before.size(), std::size_t(1));
    QCOMPARE(merged.after.size(), std::size_t(1));
}

void TextToolsTest::theReassembledLineIsTheOriginal() {
    const QString before = QStringLiteral("    const std::string name = \"value\";");
    const QString after = QStringLiteral("    const std::string label = \"value\";");
    const WordDiff diff = wordDiff(before, after);

    // Whatever the runs are, putting them back together must give the line back:
    // a diff that loses a character is worse than no diff.
    QCOMPARE(joined(diff.before), before);
    QCOMPARE(joined(diff.after), after);
}

QTEST_MAIN(TextToolsTest)
#include "test_text_tools.moc"
