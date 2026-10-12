/// Stash, against real repositories.
///
/// libgit2 has no stash, so every operation runs as `git stash ...`; the git
/// binary is therefore both the implementation and the oracle, and the tests
/// compare against it directly rather than against a second reimplementation.
///
/// Entries are addressed by oid, not by `stash@{n}`. The test that drops an
/// earlier entry and then applies a later one by its oid is the point of that
/// choice: the index the reader saw is stale the moment the list moves.

#include "git/cli.h"
#include "repo/repo_session.h"
#include "ui/main_window.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace gk;

namespace {

ProcessResult gitRun(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, dir.toStdString());
}

bool git(const QString& dir, const QStringList& args) {
    return gitRun(dir, args).exitCode == 0;
}

QString gitOut(const QString& dir, const QStringList& args) {
    return QString::fromStdString(gitRun(dir, args).out);
}

bool writeFile(const QString& dir, const QString& name, const QByteArray& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content) == content.size();
}

QByteArray readFile(const QString& dir, const QString& name) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QString porcelain(const QString& dir) {
    return gitOut(dir, {"status", "--porcelain"});
}

QString qs(const std::string& value) {
    return QString::fromStdString(value);
}

} // namespace

class StashTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;

private slots:
    void init();
    void cleanup();

    void pushMovesChangesOffTheWorkingTree();
    void pushOnACleanTreeCreatesNothing();
    void pushCanIncludeUntracked();
    void pushCanTakeOnlySomePaths();
    void applyRestoresChangesAndKeepsTheStash();
    void popRestoresChangesAndDropsTheStash();
    void dropRemovesOneEntry();
    void branchFromStashChecksOutANewBranch();
    void anOidAnchorSurvivesAnEarlierDrop();
    void unbornHeadRefusesCleanly();
    void aConflictingPopKeepsTheStash();
    void theWindowStashesThroughItsOwnPath();
};

void StashTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();

    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    QVERIFY(writeFile(m_path, "file.txt", "line1\nline2\nline3\n"));
    QVERIFY(writeFile(m_path, "other.txt", "alpha\nbeta\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "base"}));
}

void StashTest::cleanup() {
    m_dir.reset();
}

void StashTest::pushMovesChangesOffTheWorkingTree() {
    QVERIFY(writeFile(m_path, "file.txt", "line1\nchanged\nline3\n"));

    RepoSession session;
    QVERIFY(session.open(m_path));
    QVERIFY(session.stashList().empty());

    QString error;
    QVERIFY2(session.stashPush("work in progress", false, {}, &error), qPrintable(error));

    QCOMPARE(session.stashList().size(), std::size_t(1));
    QVERIFY(porcelain(m_path).trimmed().isEmpty());
    QCOMPARE(readFile(m_path, "file.txt"), QByteArray("line1\nline2\nline3\n"));
    QVERIFY(gitOut(m_path, {"stash", "list"}).contains("work in progress"));
}

void StashTest::pushOnACleanTreeCreatesNothing() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    QString error;
    // git exits 0 with "No local changes to save"; that is not a failure.
    QVERIFY2(session.stashPush("nothing", false, {}, &error), qPrintable(error));
    QVERIFY(session.stashList().empty());
}

void StashTest::pushCanIncludeUntracked() {
    QVERIFY(writeFile(m_path, "brand-new.txt", "never committed\n"));

    RepoSession session;
    QVERIFY(session.open(m_path));

    QString error;
    QVERIFY2(session.stashPush("with untracked", true, {}, &error), qPrintable(error));
    QCOMPARE(session.stashList().size(), std::size_t(1));
    QVERIFY(!QFile::exists(QDir(m_path).filePath("brand-new.txt")));
    // Restoring it puts the untracked file back.
    QVERIFY2(session.stashPop(qs(session.stashList().front().oid), &error), qPrintable(error));
    QVERIFY(QFile::exists(QDir(m_path).filePath("brand-new.txt")));
}

void StashTest::pushCanTakeOnlySomePaths() {
    QVERIFY(writeFile(m_path, "file.txt", "line1\nchanged\nline3\n"));
    QVERIFY(writeFile(m_path, "other.txt", "alpha\nBETA\n"));

    RepoSession session;
    QVERIFY(session.open(m_path));

    QString error;
    QVERIFY2(session.stashPush("only file.txt", false, {"file.txt"}, &error), qPrintable(error));

    QCOMPARE(session.stashList().size(), std::size_t(1));
    const QString status = porcelain(m_path);
    QVERIFY(!status.contains("file.txt"));
    QVERIFY(status.contains("other.txt"));
    QCOMPARE(readFile(m_path, "file.txt"), QByteArray("line1\nline2\nline3\n"));
    QCOMPARE(readFile(m_path, "other.txt"), QByteArray("alpha\nBETA\n"));
}

void StashTest::applyRestoresChangesAndKeepsTheStash() {
    QVERIFY(writeFile(m_path, "file.txt", "line1\nchanged\nline3\n"));

    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.stashPush("keep me", false, {}, &error), qPrintable(error));

    const std::vector<StashEntry> entries = session.stashList();
    QCOMPARE(entries.size(), std::size_t(1));
    QVERIFY2(session.stashApply(qs(entries.front().oid), &error), qPrintable(error));

    QVERIFY(porcelain(m_path).contains("file.txt"));
    QCOMPARE(readFile(m_path, "file.txt"), QByteArray("line1\nchanged\nline3\n"));
    QCOMPARE(session.stashList().size(), std::size_t(1));
}

void StashTest::popRestoresChangesAndDropsTheStash() {
    QVERIFY(writeFile(m_path, "file.txt", "line1\nchanged\nline3\n"));

    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.stashPush("pop me", false, {}, &error), qPrintable(error));

    const std::vector<StashEntry> entries = session.stashList();
    QCOMPARE(entries.size(), std::size_t(1));
    QVERIFY2(session.stashPop(qs(entries.front().oid), &error), qPrintable(error));

    QCOMPARE(readFile(m_path, "file.txt"), QByteArray("line1\nchanged\nline3\n"));
    QVERIFY(session.stashList().empty());
}

void StashTest::dropRemovesOneEntry() {
    QVERIFY(writeFile(m_path, "file.txt", "line1\nfirst\nline3\n"));

    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.stashPush("first", false, {}, &error), qPrintable(error));

    QVERIFY(writeFile(m_path, "other.txt", "alpha\nsecond\n"));
    QVERIFY2(session.stashPush("second", false, {}, &error), qPrintable(error));

    std::vector<StashEntry> entries = session.stashList();
    QCOMPARE(entries.size(), std::size_t(2));
    // Newest first: entries[0] is "second", entries[1] is "first".
    const std::string keeper = entries.front().oid;
    QVERIFY2(session.stashDrop(qs(entries.back().oid), &error), qPrintable(error));

    entries = session.stashList();
    QCOMPARE(entries.size(), std::size_t(1));
    QCOMPARE(entries.front().oid, keeper);
}

void StashTest::branchFromStashChecksOutANewBranch() {
    QVERIFY(writeFile(m_path, "file.txt", "line1\nrecovered\nline3\n"));

    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.stashPush("recover", false, {}, &error), qPrintable(error));

    const std::vector<StashEntry> entries = session.stashList();
    QCOMPARE(entries.size(), std::size_t(1));
    QVERIFY2(session.stashBranch(qs(entries.front().oid), "recovered", &error), qPrintable(error));

    QCOMPARE(gitOut(m_path, {"rev-parse", "--abbrev-ref", "HEAD"}).trimmed(),
             QStringLiteral("recovered"));
    QCOMPARE(readFile(m_path, "file.txt"), QByteArray("line1\nrecovered\nline3\n"));
    QVERIFY(session.stashList().empty());
}

void StashTest::anOidAnchorSurvivesAnEarlierDrop() {
    QVERIFY(writeFile(m_path, "file.txt", "line1\nfrom-A\nline3\n"));

    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.stashPush("A", false, {}, &error), qPrintable(error));

    QVERIFY(writeFile(m_path, "other.txt", "alpha\nfrom-B\n"));
    QVERIFY2(session.stashPush("B", false, {}, &error), qPrintable(error));

    std::vector<StashEntry> entries = session.stashList();
    QCOMPARE(entries.size(), std::size_t(2));
    const std::string oidOfA = entries.back().oid; // "A" is the older entry, stash@{1}

    // Dropping "B" moves "A" to stash@{0}: an index recorded earlier would now
    // name the wrong entry, an oid cannot.
    QVERIFY2(session.stashDrop(qs(entries.front().oid), &error), qPrintable(error));

    entries = session.stashList();
    QCOMPARE(entries.size(), std::size_t(1));
    QCOMPARE(entries.front().oid, oidOfA);

    QVERIFY2(session.stashApply(qs(oidOfA), &error), qPrintable(error));
    QCOMPARE(readFile(m_path, "file.txt"), QByteArray("line1\nfrom-A\nline3\n"));
}

void StashTest::unbornHeadRefusesCleanly() {
    QTemporaryDir fresh;
    QVERIFY(fresh.isValid());
    const QString path = fresh.path();
    QVERIFY(git(path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(path, {"config", "user.name", "Test"}));
    QVERIFY(git(path, {"config", "user.email", "test@localhost"}));
    QVERIFY(writeFile(path, "file.txt", "no commit yet\n"));

    RepoSession session;
    QVERIFY(session.open(path));

    QString error;
    QVERIFY(!session.stashPush("nope", false, {}, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(session.stashList().empty());
}

void StashTest::aConflictingPopKeepsTheStash() {
    QVERIFY(writeFile(m_path, "file.txt", "stashed\nline2\nline3\n"));

    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.stashPush("conflicting", false, {}, &error), qPrintable(error));

    const std::vector<StashEntry> entries = session.stashList();
    QCOMPARE(entries.size(), std::size_t(1));

    // A committed change to the same line, so the stashed patch has to merge
    // rather than sit on top of an unchanged file — that is what conflicts.
    QVERIFY(writeFile(m_path, "file.txt", "committed\nline2\nline3\n"));
    QVERIFY(git(m_path, {"commit", "-aqm", "committed"}));

    QVERIFY(!session.stashPop(qs(entries.front().oid), &error));
    QVERIFY(!error.isEmpty());

    // git keeps the stash when the pop conflicts, and leaves unmerged entries.
    QCOMPARE(session.stashList().size(), std::size_t(1));
    QVERIFY(!gitOut(m_path, {"ls-files", "-u"}).trimmed().isEmpty());
}

void StashTest::theWindowStashesThroughItsOwnPath() {
    QVERIFY(writeFile(m_path, "file.txt", "line1\nwindow\nline3\n"));

    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    const QStringList pushed =
        window.stashForDiagnostics(QStringLiteral("push"), QStringLiteral("from the window"));
    const QString pushedText = pushed.join(QLatin1Char('\n'));
    QVERIFY2(pushedText.contains(QStringLiteral("stashes after: 1")), qPrintable(pushedText));
    QVERIFY(porcelain(m_path).trimmed().isEmpty());

    const QString oid = gitOut(m_path, {"rev-parse", "refs/stash"}).trimmed();
    QVERIFY(!oid.isEmpty());
    const QStringList popped = window.stashForDiagnostics(QStringLiteral("pop"), oid);
    const QString poppedText = popped.join(QLatin1Char('\n'));
    QVERIFY2(poppedText.contains(QStringLiteral("stashes after: 0")), qPrintable(poppedText));
    QCOMPARE(readFile(m_path, "file.txt"), QByteArray("line1\nwindow\nline3\n"));
}

QTEST_MAIN(StashTest)
#include "test_stash.moc"