#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"
#include "ui/conflict_view.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace gk;

namespace {

bool git(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, dir.toStdString()).exitCode == 0;
}

QString gitOut(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, dir.toStdString()).out).trimmed();
}

QStringList identity() {
    return {QStringLiteral("-c"), QStringLiteral("user.name=Test"), QStringLiteral("-c"),
            QStringLiteral("user.email=test@localhost")};
}

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

QString readFile(const QString& dir, const QString& name) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

} // namespace

/// Squash-merge, against real repositories: git stages the change without committing
/// and deliberately writes no MERGE_HEAD, so the completion is an ordinary commit and
/// the state has to be read from SQUASH_MSG instead.
class SquashMergeTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aCleanSquashStagesAndDoesNotCommit();
    void aConflictedSquashLeavesTheIndexUnmerged();
    void theConflictPageFinishesASquashAsAnOrdinaryCommit();
    void abortingASquashResetsAndClearsTheMarker();
    void squashingAnUpToDateBranchStagesNothing();

private:
    void buildConflict();

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void SquashMergeTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    buildConflict();
}

void SquashMergeTest::cleanup() {
    m_dir.reset();
}

void SquashMergeTest::buildConflict() {
    QVERIFY(writeFile(m_path, "a.txt", "line1\nline2\nline3\n"));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "base"}));

    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(writeFile(m_path, "a.txt", "line1\nFEATURE\nline3\n"));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "feature"}));

    // Main advances too, so feature and main truly diverge and squash conflicts.
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(m_path, "a.txt", "line1\nMAIN\nline3\n"));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "main"}));
}

void SquashMergeTest::aCleanSquashStagesAndDoesNotCommit() {
    // A branch that only adds a file: squashing it stages the change and creates no
    // commit, which is the whole difference from a merge.
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "clean"}));
    QVERIFY(writeFile(m_path, "c.txt", "clean\n"));
    QVERIFY(git(m_path, {"add", "c.txt"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "clean"}));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    const QString headBefore = gitOut(m_path, {"rev-parse", "HEAD"});

    RepoSession session;
    QVERIFY(session.open(m_path));
    SquashResult result;
    QString error;
    QVERIFY2(session.squashMerge(QStringLiteral("clean"), &result, &error), qPrintable(error));

    QCOMPARE(result.status, SquashStatus::Staged);
    QCOMPARE(result.conflictCount, std::size_t(0));
    QCOMPARE(gitOut(m_path, {"rev-parse", "HEAD"}), headBefore); // nothing committed
    QVERIFY(session.squashInProgress());
    QVERIFY(!session.mergeInProgress());
    QVERIFY(gitOut(m_path, {"diff", "--cached", "--name-only"}).contains(QStringLiteral("c.txt")));
}

void SquashMergeTest::aConflictedSquashLeavesTheIndexUnmerged() {
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    RepoSession session;
    QVERIFY(session.open(m_path));

    SquashResult result;
    QString error;
    QVERIFY2(session.squashMerge(QStringLiteral("feature"), &result, &error), qPrintable(error));
    QCOMPARE(result.status, SquashStatus::Conflicted);
    QCOMPARE(result.conflictCount, std::size_t(1));

    // The whole point: a squash is in progress, yet there is no MERGE_HEAD. Anything
    // that keys off mergeInProgress() would miss it.
    QVERIFY(session.squashInProgress());
    QVERIFY(!session.mergeInProgress());
    QVERIFY(!session.conflicts().empty());
    QVERIFY(!QFile::exists(QDir(m_path).filePath(".git/MERGE_HEAD")));
    QVERIFY(QFile::exists(QDir(m_path).filePath(".git/SQUASH_MSG")));
}

void SquashMergeTest::theConflictPageFinishesASquashAsAnOrdinaryCommit() {
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    RepoSession session;
    QVERIFY(session.open(m_path));
    SquashResult result;
    QString error;
    QVERIFY(session.squashMerge(QStringLiteral("feature"), &result, &error));
    QCOMPARE(result.status, SquashStatus::Conflicted);

    const QString headBefore = gitOut(m_path, {"rev-parse", "HEAD"});

    ConflictView view(&session);
    view.reload();
    QVERIFY(view.hasConflicts());
    QVERIFY(view.setRegionChoice(0, QStringLiteral("theirs")));
    QVERIFY2(view.trySaveResolution(&error), qPrintable(error));
    QVERIFY(session.conflicts().empty());
    // The window reloads after a save; the test must too, or the page still thinks a
    // conflict is on screen and refuses to finish.
    view.reload();

    // The page's own finish path, driven as the button would.
    QVERIFY(QMetaObject::invokeMethod(&view, "completeMerge"));
    QVERIFY(!session.squashInProgress());
    QVERIFY(!session.mergeInProgress());

    // One parent — a squash is not a merge commit — and the branch moved forward.
    const QString parents = gitOut(m_path, {"rev-list", "--parents", "-n1", "HEAD"});
    QCOMPARE(parents.split(QLatin1Char(' '), Qt::SkipEmptyParts).size(), 2); // HEAD + one parent
    QVERIFY(gitOut(m_path, {"rev-parse", "HEAD"}) != headBefore);
    QCOMPARE(readFile(m_path, "a.txt"), QStringLiteral("line1\nFEATURE\nline3\n"));
}

void SquashMergeTest::abortingASquashResetsAndClearsTheMarker() {
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    RepoSession session;
    QVERIFY(session.open(m_path));
    SquashResult result;
    QString error;
    QVERIFY(session.squashMerge(QStringLiteral("feature"), &result, &error));
    QCOMPARE(result.status, SquashStatus::Conflicted);

    const QString headBefore = gitOut(m_path, {"rev-parse", "HEAD"});
    QVERIFY2(session.abortMerge(&error), qPrintable(error));

    QVERIFY(!session.squashInProgress());
    QCOMPARE(gitOut(m_path, {"rev-parse", "HEAD"}), headBefore);
    // The reset goes to HEAD, which is main: ours is restored, not the base.
    QCOMPARE(readFile(m_path, "a.txt"), QStringLiteral("line1\nMAIN\nline3\n"));
    QVERIFY(!QFile::exists(QDir(m_path).filePath(".git/SQUASH_MSG")));
}

void SquashMergeTest::squashingAnUpToDateBranchStagesNothing() {
    // main already has feature's tip? No — make a branch whose tip is an ancestor.
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    RepoSession session;
    QVERIFY(session.open(m_path));

    SquashResult result;
    QString error;
    // Squashing HEAD's own branch is a no-op git refuses to make a change for.
    QVERIFY2(session.squashMerge(QStringLiteral("main"), &result, &error), qPrintable(error));
    QCOMPARE(result.status, SquashStatus::UpToDate);
    QVERIFY(!session.squashInProgress());
}

QTEST_MAIN(SquashMergeTest)
#include "test_squash_merge.moc"
