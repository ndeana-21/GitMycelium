#include "git/cli.h"
#include "repo/repo_session.h"
#include "ui/main_window.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace gk;

namespace {

bool git(const QString& workDir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const auto& arg : args) {
        argv.push_back(arg.toStdString());
    }
    return runGit(argv, workDir.toStdString()).exitCode == 0;
}

bool writeFile(const QString& path, const QByteArray& content) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(content) == content.size();
}

QString revParse(const QString& workDir, const QString& revision) {
    const ProcessResult result =
        runGit({"rev-parse", revision.toStdString()}, workDir.toStdString());
    return QString::fromStdString(result.out).trimmed();
}

bool refExists(RepoSession& session, const QString& fullName) {
    for (const RefInfo& ref : session.refs()) {
        if (QString::fromStdString(ref.fullName) == fullName) {
            return true;
        }
    }
    return false;
}

int stagedCount(RepoSession& session) {
    int count = 0;
    for (const FileChange& change : session.status()) {
        if (change.staged) {
            ++count;
        }
    }
    return count;
}

} // namespace

class OplogTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
    QString m_first;

private slots:
    void init();
    void cleanup();

    void undoPutBackACommitAndKeepsItsChanges();
    void undoAPutsBackACheckout();
    void anUndoThatCannotCheckOutMovesNothing();
    void undoRestoresADeletedBranch();
    void undoARename();
    void undoAMixedResetRestoresTheIndex();
    void aNewOperationDropsTheRedoStack();
    void nothingToUndoOnAFreshSession();
    void refusesWhileAnOperationIsOpen();
    void refusesWhenThePointerMovedSince();
    void anUnrelatedRefDoesNotBlockTheUndo();
    void undoTheFirstCommitLeavesAnUnbornBranch();
    void theWindowUndoesThroughItsOwnPath();
};

void OplogTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();

    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Demo"}));
    QVERIFY(git(m_path, {"config", "user.email", "demo@example.com"}));
    QVERIFY(git(m_path, {"config", "commit.gpgsign", "false"}));

    QVERIFY(writeFile(m_path + "/a.txt", "base\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "A"}));
    m_first = revParse(m_path, "HEAD");
    QVERIFY(!m_first.isEmpty());
}

void OplogTest::cleanup() {
    m_dir.reset();
}

void OplogTest::undoPutBackACommitAndKeepsItsChanges() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    QVERIFY(writeFile(m_path + "/a.txt", "base\nmore\n"));
    QString error;
    QVERIFY(session.stageFile("a.txt", &error));
    QString newCommit;
    QVERIFY2(session.createCommit("B", &newCommit, &error), qPrintable(error));

    QVERIFY(session.canUndo());
    QCOMPARE(session.nextUndoLabel(), QStringLiteral("Commit"));

    QString label;
    QVERIFY2(session.undo(&label, &error), qPrintable(error));
    QCOMPARE(label, QStringLiteral("Commit"));
    // The branch tip is back at A, and the commit's content is still here — as a
    // staged change, which is what lets the reader try again.
    QCOMPARE(session.headTipId(), m_first);
    QCOMPARE(stagedCount(session), 1);
    QVERIFY(session.canRedo());

    QVERIFY2(session.redo(&label, &error), qPrintable(error));
    QCOMPARE(session.headTipId(), newCommit);
    QCOMPARE(stagedCount(session), 0);
}

void OplogTest::undoAPutsBackACheckout() {
    QVERIFY(git(m_path, {"branch", "feature"}));

    RepoSession session;
    QVERIFY(session.open(m_path));

    QString error;
    QVERIFY2(session.checkoutBranch("feature", &error), qPrintable(error));
    QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("feature"));

    QString label;
    QVERIFY2(session.undo(&label, &error), qPrintable(error));
    QCOMPARE(label, QStringLiteral("Checkout feature"));
    QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("main"));

    QVERIFY2(session.redo(&label, &error), qPrintable(error));
    QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("feature"));
}

void OplogTest::anUndoThatCannotCheckOutMovesNothing() {
    // `feature` has a different a.txt, so going back to main over a local edit of it
    // has to be refused — before HEAD moves, not after.
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(writeFile(m_path + "/a.txt", "feature\n"));
    QVERIFY(git(m_path, {"commit", "-qam", "F"}));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));

    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.checkoutBranch("feature", &error), qPrintable(error));
    QVERIFY(writeFile(m_path + "/a.txt", "local edit\n"));

    QString label;
    QVERIFY(!session.undo(&label, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("feature"));
    QCOMPARE(revParse(m_path, "HEAD"), revParse(m_path, "feature"));
    QCOMPARE(revParse(m_path, "main"), m_first);
    QFile file(m_path + "/a.txt");
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("local edit\n"));
}

void OplogTest::undoRestoresADeletedBranch() {
    QVERIFY(git(m_path, {"branch", "topic"}));

    RepoSession session;
    QVERIFY(session.open(m_path));

    QString error;
    QVERIFY2(session.deleteBranchForced("topic", &error), qPrintable(error));
    QVERIFY(!refExists(session, QStringLiteral("refs/heads/topic")));

    QString label;
    QVERIFY2(session.undo(&label, &error), qPrintable(error));
    QVERIFY(refExists(session, QStringLiteral("refs/heads/topic")));

    QVERIFY2(session.redo(&label, &error), qPrintable(error));
    QVERIFY(!refExists(session, QStringLiteral("refs/heads/topic")));
}

void OplogTest::undoARename() {
    QVERIFY(git(m_path, {"branch", "topic"}));

    RepoSession session;
    QVERIFY(session.open(m_path));

    QString error;
    QVERIFY2(session.renameBranch("topic", "renamed", &error), qPrintable(error));
    QVERIFY(refExists(session, QStringLiteral("refs/heads/renamed")));

    QString label;
    QVERIFY2(session.undo(&label, &error), qPrintable(error));
    QVERIFY(refExists(session, QStringLiteral("refs/heads/topic")));
    QVERIFY(!refExists(session, QStringLiteral("refs/heads/renamed")));
}

void OplogTest::undoAMixedResetRestoresTheIndex() {
    QVERIFY(writeFile(m_path + "/a.txt", "base\nsecond\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "B"}));
    const QString second = revParse(m_path, "HEAD");

    RepoSession session;
    QVERIFY(session.open(m_path));

    QString error;
    QVERIFY2(session.resetTo(m_first, ResetMode::Mixed, &error), qPrintable(error));
    QCOMPARE(session.headTipId(), m_first);
    QVERIFY(session.status().size() > 0); // B's change is now unstaged

    QString label;
    QVERIFY2(session.undo(&label, &error), qPrintable(error));
    QCOMPARE(label, QStringLiteral("Reset --mixed to %1").arg(m_first.left(7)));
    QCOMPARE(session.headTipId(), second);
    QCOMPARE(stagedCount(session), 0);
    QCOMPARE(session.status().size(), std::size_t(0));
}

void OplogTest::aNewOperationDropsTheRedoStack() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    QVERIFY(writeFile(m_path + "/a.txt", "base\none\n"));
    QString error;
    QVERIFY(session.stageAll(&error));
    QString first;
    QVERIFY2(session.createCommit("B", &first, &error), qPrintable(error));

    QString label;
    QVERIFY2(session.undo(&label, &error), qPrintable(error));
    QVERIFY(session.canRedo());

    QVERIFY(writeFile(m_path + "/a.txt", "base\ntwo\n"));
    QVERIFY(session.stageAll(&error));
    QString second;
    QVERIFY2(session.createCommit("C", &second, &error), qPrintable(error));
    QVERIFY(!session.canRedo());
}

void OplogTest::nothingToUndoOnAFreshSession() {
    QVERIFY(git(m_path, {"commit", "-q", "--allow-empty", "-m", "B"}));

    RepoSession session;
    QVERIFY(session.open(m_path));

    QVERIFY(!session.canUndo());
    QVERIFY(!session.canRedo());
    QString label;
    QString error;
    QVERIFY(!session.undo(&label, &error));
    QVERIFY(!error.isEmpty());
}

void OplogTest::refusesWhileAnOperationIsOpen() {
    // Two branches that touch the same line, so the merge stops on a conflict and
    // stays open.
    QVERIFY(git(m_path, {"checkout", "-qb", "feature"}));
    QVERIFY(writeFile(m_path + "/a.txt", "feature\n"));
    QVERIFY(git(m_path, {"commit", "-qam", "feature change"}));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(m_path + "/a.txt", "main\n"));
    QVERIFY(git(m_path, {"commit", "-qam", "main change"}));

    RepoSession session;
    QVERIFY(session.open(m_path));

    MergeResult result;
    QString error;
    QVERIFY2(session.mergeBranch("feature", &result, &error), qPrintable(error));
    QCOMPARE(result.status, MergeStatus::Conflicted);
    QVERIFY(session.mergeInProgress());

    QString label;
    QVERIFY(!session.undo(&label, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(session.mergeInProgress());
}

void OplogTest::refusesWhenThePointerMovedSince() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    QVERIFY(writeFile(m_path + "/a.txt", "base\none\n"));
    QString error;
    QVERIFY(session.stageAll(&error));
    QVERIFY2(session.createCommit("B", nullptr, &error), qPrintable(error));

    // Somebody moves the same branch in a terminal. Putting it back would
    // overwrite their work silently, so the undo refuses instead.
    QVERIFY(git(m_path, {"reset", "-q", "--hard", "HEAD~1"}));

    QString label;
    QVERIFY(!session.undo(&label, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(revParse(m_path, "HEAD"), m_first);
}

void OplogTest::anUnrelatedRefDoesNotBlockTheUndo() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    QVERIFY(writeFile(m_path + "/a.txt", "base\none\n"));
    QString error;
    QVERIFY(session.stageAll(&error));
    QString committed;
    QVERIFY2(session.createCommit("B", &committed, &error), qPrintable(error));

    // A branch that appears from elsewhere — a fetch would do the same — is not
    // one the operation touched, so it must not stop the undo.
    QVERIFY(git(m_path, {"branch", "fetched"}));

    QString label;
    QVERIFY2(session.undo(&label, &error), qPrintable(error));
    QCOMPARE(session.headTipId(), m_first);
    QVERIFY(refExists(session, QStringLiteral("refs/heads/fetched")));
}

void OplogTest::undoTheFirstCommitLeavesAnUnbornBranch() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path();
    QVERIFY(git(path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(path, {"config", "user.name", "Demo"}));
    QVERIFY(git(path, {"config", "user.email", "demo@example.com"}));
    QVERIFY(writeFile(path + "/a.txt", "root\n"));

    RepoSession session;
    QVERIFY(session.open(path));
    QString error;
    QVERIFY(session.stageAll(&error));
    QString created;
    QVERIFY2(session.createCommit("root", &created, &error), qPrintable(error));
    QCOMPARE(session.headTipId(), created);

    QString label;
    QVERIFY2(session.undo(&label, &error), qPrintable(error));
    QVERIFY(session.headTipId().isEmpty());
    QVERIFY(session.canRedo());

    QVERIFY2(session.redo(&label, &error), qPrintable(error));
    QCOMPARE(session.headTipId(), created);
}

void OplogTest::theWindowUndoesThroughItsOwnPath() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    QVERIFY(writeFile(m_path + "/a.txt", "base\nwindow\n"));
    const QStringList committed = window.commitPanelForDiagnostics(QStringList(), QStringLiteral("W"));
    QVERIFY2(committed.join(QLatin1Char('\n')).contains(QStringLiteral("status: Committed")),
             qPrintable(committed.join(QLatin1Char('\n'))));
    const QString tip = revParse(m_path, "HEAD");
    QVERIFY(tip != m_first);

    const QStringList undone = window.undoForDiagnostics();
    const QString joined = undone.join(QLatin1Char('\n'));
    QVERIFY2(joined.contains(QStringLiteral("undo: Commit")), qPrintable(joined));
    QVERIFY2(joined.contains(QStringLiteral("canRedo=yes")), qPrintable(joined));
    QCOMPARE(revParse(m_path, "HEAD"), m_first);

    const QStringList redone = window.redoForDiagnostics();
    QVERIFY2(redone.join(QLatin1Char('\n')).contains(QStringLiteral("redo: Commit")),
             qPrintable(redone.join(QLatin1Char('\n'))));
    QCOMPARE(revParse(m_path, "HEAD"), tip);
}

QTEST_MAIN(OplogTest)
#include "test_oplog.moc"
