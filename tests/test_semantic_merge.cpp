#include "core/conflict_regions.h"
#include "core/semantic_merge.h"

#include <QtTest>

using namespace gk;

namespace {

const std::string kBase = "{\n  \"a\": 1,\n  \"b\": 1\n}\n";

} // namespace

class SemanticMergeTest : public QObject {
    Q_OBJECT

private slots:
    void disjointKeysMergeWithoutMarkers();
    void theSameKeyChangedBothWaysIsAConflict();
    void aKeyOnlyOneSideMovedIsTakenWhole();
    void deletingAKeyOnOneSideRemovesIt();
    void anUntouchedMemberKeepsItsExactBytes();
    void compactJsonStaysCompact();
    void aCompactConflictKeepsTheBracesOffTheMarkers();
    void invalidJsonIsNotApplicable();
};

void SemanticMergeTest::disjointKeysMergeWithoutMarkers() {
    const std::string ours = "{\n  \"a\": 2,\n  \"b\": 1\n}\n";
    const std::string theirs = "{\n  \"a\": 1,\n  \"b\": 3\n}\n";

    const SemanticMergeResult result = semanticMergeJson(ours, kBase, theirs);
    QVERIFY(result.applicable);
    QVERIFY(!result.hasConflicts);
    QCOMPARE(result.conflicts, std::size_t(0));
    QCOMPARE(result.merged, std::string("{\n  \"a\": 2,\n  \"b\": 3\n}\n"));
}

void SemanticMergeTest::theSameKeyChangedBothWaysIsAConflict() {
    const std::string ours = "{\n  \"a\": 2,\n  \"b\": 1\n}\n";
    const std::string theirs = "{\n  \"a\": 3,\n  \"b\": 1\n}\n";

    const SemanticMergeResult result = semanticMergeJson(ours, kBase, theirs);
    QVERIFY(result.applicable);
    QVERIFY(result.hasConflicts);
    QCOMPARE(result.conflicts, std::size_t(1));

    // The caller shows the same cards as any other conflict, so the markers must
    // be what the parser expects.
    const ConflictDocument doc = parseConflictDocument(result.merged);
    QCOMPARE(doc.regions.size(), std::size_t(1));
    QCOMPARE(doc.regions[0].ours.size(), std::size_t(1));
    QVERIFY(doc.regions[0].ours[0].find("\"a\": 2") != std::string::npos);
    QVERIFY(doc.regions[0].base[0].find("\"a\": 1") != std::string::npos);
    QVERIFY(doc.regions[0].theirs[0].find("\"a\": 3") != std::string::npos);
}

void SemanticMergeTest::aKeyOnlyOneSideMovedIsTakenWhole() {
    const std::string ours = "{\n  \"a\": 1,\n  \"b\": 1,\n  \"c\": 9\n}\n";

    const SemanticMergeResult result = semanticMergeJson(ours, kBase, kBase);
    QVERIFY(result.applicable);
    QVERIFY(!result.hasConflicts);
    QVERIFY(result.merged.find("\"c\": 9") != std::string::npos);
}

void SemanticMergeTest::deletingAKeyOnOneSideRemovesIt() {
    const std::string ours = "{\n  \"a\": 1\n}\n";

    const SemanticMergeResult result = semanticMergeJson(ours, kBase, kBase);
    QVERIFY(result.applicable);
    QVERIFY(!result.hasConflicts);
    QVERIFY(result.merged.find("\"b\"") == std::string::npos);
    QVERIFY(result.merged.find("\"a\"") != std::string::npos);
}

void SemanticMergeTest::anUntouchedMemberKeepsItsExactBytes() {
    const std::string base = "{\n  \"keep\": { \"deep\":  1 },\n  \"a\": 1\n}\n";
    const std::string ours = "{\n  \"keep\": { \"deep\":  1 },\n  \"a\": 2\n}\n";

    const SemanticMergeResult result = semanticMergeJson(ours, base, base);
    QVERIFY(result.applicable);
    QVERIFY(!result.hasConflicts);
    QVERIFY2(result.merged.find("\"keep\": { \"deep\":  1 }") != std::string::npos,
             result.merged.c_str());
}

void SemanticMergeTest::compactJsonStaysCompact() {
    const SemanticMergeResult result = semanticMergeJson("{\"a\":2,\"b\":1}", "{\"a\":1,\"b\":1}",
                                                         "{\"a\":1,\"b\":3}");
    QVERIFY(result.applicable);
    QVERIFY(!result.hasConflicts);
    QCOMPARE(result.merged, std::string("{\"a\":2,\"b\":3}"));
}

void SemanticMergeTest::aCompactConflictKeepsTheBracesOffTheMarkers() {
    const SemanticMergeResult result = semanticMergeJson("{\"a\":2}", "{\"a\":1}", "{\"a\":3}");
    QVERIFY(result.applicable);
    QVERIFY(result.hasConflicts);
    // The marker must begin a line, and the closing brace must not sit on one.
    QVERIFY2(result.merged.rfind("{\n<<<<<<<", 0) == 0, result.merged.c_str());
    QVERIFY2(result.merged.find(">>>>>>> theirs\n}") != std::string::npos, result.merged.c_str());
    QCOMPARE(parseConflictDocument(result.merged).regions.size(), std::size_t(1));
}

void SemanticMergeTest::invalidJsonIsNotApplicable() {
    QVERIFY(!semanticMergeJson("not json", kBase, kBase).applicable);
    QVERIFY(!semanticMergeJson("[1, 2]", kBase, kBase).applicable);
}

QTEST_MAIN(SemanticMergeTest)
#include "test_semantic_merge.moc"
