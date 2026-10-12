#include "ui/rebase_editor.h"

#include "core/types.h"

#include <QtTest>
#include <QApplication>

using namespace gk;

namespace {

std::vector<RebaseCandidate> threeCandidates() {
    return {{"aaaaaaaa00000000000000000000000000000001", "one"},
            {"bbbbbbbb00000000000000000000000000000002", "two"},
            {"cccccccc00000000000000000000000000000003", "three"}};
}

} // namespace

class RebaseEditorTest : public QObject {
    Q_OBJECT

private slots:
    void everyRowStartsAsAPick();
    void anActionAndAReorderDriveTheSteps();
    void aFirstFoldIsRefused();
    void aRewordCarriesItsMessageIntoTheSteps();
};

void RebaseEditorTest::everyRowStartsAsAPick() {
    RebaseEditor editor;
    editor.setCandidates(threeCandidates());
    QCOMPARE(editor.rowCount(), 3);
    for (int row = 0; row < editor.rowCount(); ++row) {
        QCOMPARE(editor.actionAt(row), RewriteAction::Keep);
    }
    QVERIFY(editor.refusal().isEmpty());
}

void RebaseEditorTest::anActionAndAReorderDriveTheSteps() {
    RebaseEditor editor;
    editor.setCandidates(threeCandidates());

    // Drop the oldest, then drag it to the end. The steps must follow the list, not the
    // original range order.
    editor.setAction(0, RewriteAction::Drop);
    editor.moveRow(0, 2);
    QCOMPARE(editor.actionAt(2), RewriteAction::Drop);

    const std::vector<RewriteStep> steps = editor.steps();
    QCOMPARE(steps.size(), std::size_t(3));
    QCOMPARE(steps[0].commit, std::string("bbbbbbbb00000000000000000000000000000002"));
    QCOMPARE(steps[1].commit, std::string("cccccccc00000000000000000000000000000003"));
    QCOMPARE(steps[2].commit, std::string("aaaaaaaa00000000000000000000000000000001"));
    QCOMPARE(steps[2].action, RewriteAction::Drop);
}

void RebaseEditorTest::aFirstFoldIsRefused() {
    RebaseEditor editor;
    editor.setCandidates(threeCandidates());
    editor.setAction(0, RewriteAction::Squash);
    QVERIFY(!editor.refusal().isEmpty());
    editor.setAction(0, RewriteAction::Keep);
    QVERIFY(editor.refusal().isEmpty());
}

void RebaseEditorTest::aRewordCarriesItsMessageIntoTheSteps() {
    RebaseEditor editor;
    editor.setCandidates(threeCandidates());
    editor.setAction(1, RewriteAction::Reword);
    // A reword with no message is not runnable yet.
    QVERIFY(!editor.refusal().isEmpty());
    editor.setMessage(1, QStringLiteral("a better message"));
    QVERIFY(editor.refusal().isEmpty());

    const std::vector<RewriteStep> steps = editor.steps();
    QCOMPARE(steps[1].action, RewriteAction::Reword);
    QCOMPARE(steps[1].message, std::string("a better message"));
}

QTEST_MAIN(RebaseEditorTest)
#include "test_rebase_editor.moc"
