#include "core/rebase_plan.h"

#include <QtTest>

using namespace gk;

namespace {

std::vector<RebaseCandidate> threeCandidates() {
    return {{"aaaaaaaa00000000000000000000000000000001", "one"},
            {"bbbbbbbb00000000000000000000000000000002", "two"},
            {"cccccccc00000000000000000000000000000003", "three"}};
}

} // namespace

class RebasePlanTest : public QObject {
    Q_OBJECT

private slots:
    void actionNamesRoundTrip();
    void defaultPlanPicksEveryCommitInOrder();
    void aFirstStepThatFoldsIsRefused();
    void droppingEverythingIsRefused();
    void aRewordWithoutAMessageIsRefused();
    void aValidPlanHasNoRefusal();
    void aSpecNamesEveryCommitOnceInTheGivenOrder();
    void anAbbreviatedPrefixResolves();
    void anUnknownActionIsRejected();
    void anAmbiguousPrefixIsRejected();
    void aMissingOrDuplicateCommitIsRejected();
    void planTextNamesActionAndSubject();
};

void RebasePlanTest::actionNamesRoundTrip() {
    for (RewriteAction action :
         {RewriteAction::Keep, RewriteAction::Drop, RewriteAction::Squash,
          RewriteAction::Fixup, RewriteAction::Reword, RewriteAction::Edit}) {
        RewriteAction parsed = RewriteAction::Reword;
        QVERIFY(rewriteActionFromName(rewriteActionName(action), &parsed));
        QCOMPARE(parsed, action);
    }
    RewriteAction parsed = RewriteAction::Keep;
    QVERIFY(!rewriteActionFromName("nonsense", &parsed));
    QVERIFY(rewriteActionFromName("s", &parsed));
    QCOMPARE(parsed, RewriteAction::Squash);
}

void RebasePlanTest::defaultPlanPicksEveryCommitInOrder() {
    const std::vector<RebasePlanRow> rows = defaultRebasePlan(threeCandidates());
    QCOMPARE(rows.size(), std::size_t(3));
    for (const RebasePlanRow& row : rows) {
        QCOMPARE(row.action, RewriteAction::Keep);
    }
    QCOMPARE(QString::fromStdString(rows[0].subject), QStringLiteral("one"));
    QCOMPARE(QString::fromStdString(rows[2].subject), QStringLiteral("three"));
    QVERIFY(rebasePlanRefusal(rows).empty());
}

void RebasePlanTest::aFirstStepThatFoldsIsRefused() {
    auto rows = defaultRebasePlan(threeCandidates());
    rows[0].action = RewriteAction::Squash;
    QVERIFY(!rebasePlanRefusal(rows).empty());
    rows[0].action = RewriteAction::Fixup;
    QVERIFY(!rebasePlanRefusal(rows).empty());
}

void RebasePlanTest::droppingEverythingIsRefused() {
    auto rows = defaultRebasePlan(threeCandidates());
    for (RebasePlanRow& row : rows) {
        row.action = RewriteAction::Drop;
    }
    QVERIFY(!rebasePlanRefusal(rows).empty());
}

void RebasePlanTest::aRewordWithoutAMessageIsRefused() {
    auto rows = defaultRebasePlan(threeCandidates());
    rows[1].action = RewriteAction::Reword;
    QVERIFY(!rebasePlanRefusal(rows).empty());
    rows[1].message = "a new message";
    QVERIFY(rebasePlanRefusal(rows).empty());
}

void RebasePlanTest::aValidPlanHasNoRefusal() {
    auto rows = defaultRebasePlan(threeCandidates());
    rows[1].action = RewriteAction::Drop;
    rows[2].action = RewriteAction::Edit;
    QVERIFY(rebasePlanRefusal(rows).empty());
    const std::vector<RewriteStep> steps = rebasePlanSteps(rows);
    QCOMPARE(steps.size(), std::size_t(3));
    QCOMPARE(steps[1].action, RewriteAction::Drop);
    QCOMPARE(steps[2].action, RewriteAction::Edit);
}

void RebasePlanTest::aSpecNamesEveryCommitOnceInTheGivenOrder() {
    std::string error;
    const std::vector<RebasePlanRow> rows =
        parseRebaseSpec("drop:bbbbbbbb00000000000000000000000000000002,"
                        "pick:cccccccc00000000000000000000000000000003,"
                        "pick:aaaaaaaa00000000000000000000000000000001",
                        threeCandidates(), &error);
    QVERIFY2(error.empty(), error.c_str());
    QCOMPARE(rows.size(), std::size_t(3));
    QCOMPARE(QString::fromStdString(rows[0].subject), QStringLiteral("two"));
    QCOMPARE(rows[0].action, RewriteAction::Drop);
    QCOMPARE(QString::fromStdString(rows[1].subject), QStringLiteral("three"));
    QCOMPARE(QString::fromStdString(rows[2].subject), QStringLiteral("one"));
}

void RebasePlanTest::anAbbreviatedPrefixResolves() {
    std::string error;
    const std::vector<RebasePlanRow> rows =
        parseRebaseSpec("reword:cccccccc00000000000000000000000000000003=hello,"
                        "pick:aaaaaaaa,"
                        "pick:bbbbbbbb",
                        threeCandidates(), &error);
    QVERIFY2(error.empty(), error.c_str());
    QCOMPARE(rows.size(), std::size_t(3));
    QCOMPARE(QString::fromStdString(rows[0].subject), QStringLiteral("three"));
    QCOMPARE(rows[0].action, RewriteAction::Reword);
    QCOMPARE(QString::fromStdString(rows[0].message), QStringLiteral("hello"));
}

void RebasePlanTest::anUnknownActionIsRejected() {
    std::string error;
    const std::vector<RebasePlanRow> rows =
        parseRebaseSpec("frobnicate:aaaaaaaa,pick:bbbbbbbb,pick:cccccccc",
                        threeCandidates(), &error);
    QVERIFY(rows.empty());
    QVERIFY(error.find("unknown action") != std::string::npos);
}

void RebasePlanTest::anAmbiguousPrefixIsRejected() {
    std::vector<RebaseCandidate> candidates = {{"abcd000000000000000000000000000000000001", "a"},
                                               {"abcd000000000000000000000000000000000002", "b"}};
    std::string error;
    const std::vector<RebasePlanRow> rows =
        parseRebaseSpec("pick:abcd", candidates, &error);
    QVERIFY(rows.empty());
    QVERIFY(error.find("ambiguous") != std::string::npos);
}

void RebasePlanTest::aMissingOrDuplicateCommitIsRejected() {
    std::string error;
    QVERIFY(parseRebaseSpec("pick:aaaaaaaa,pick:bbbbbbbb", threeCandidates(), &error).empty());
    QVERIFY(error.find("exactly once") != std::string::npos);
    error.clear();
    QVERIFY(parseRebaseSpec("pick:aaaaaaaa,pick:aaaaaaaa,pick:bbbbbbbb",
                            threeCandidates(), &error)
                .empty());
    QVERIFY(error.find("more than once") != std::string::npos);
    error.clear();
    QVERIFY(parseRebaseSpec("pick:deadbeef,pick:bbbbbbbb,pick:cccccccc",
                            threeCandidates(), &error)
                .empty());
    QVERIFY(error.find("names no commit") != std::string::npos);
}

void RebasePlanTest::planTextNamesActionAndSubject() {
    auto rows = defaultRebasePlan(threeCandidates());
    rows[1].action = RewriteAction::Drop;
    const QString text = QString::fromStdString(rebasePlanText(rows));
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 3);
    QVERIFY(lines[0].startsWith(QStringLiteral("pick aaaaaaaa")));
    QVERIFY(lines[0].endsWith(QStringLiteral("one")));
    QVERIFY(lines[1].startsWith(QStringLiteral("drop bbbbbbbb")));
}

QTEST_MAIN(RebasePlanTest)
#include "test_rebase_plan.moc"
