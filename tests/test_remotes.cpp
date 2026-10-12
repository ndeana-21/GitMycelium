/// A bare repository plays the role of every remote: fully local, no network.
/// `RemoteOps` *is* the git binary, so the tests compare against it directly.

#include "git/cli.h"
#include "git/remote.h"
#include "repo/repo_session.h"
#include "ui/main_window.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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

/// Clones `bare` into `path`, with an identity so commits made there are allowed.
bool cloneAt(const QString& bare, const QString& path) {
    return git(QFileInfo(path).absolutePath(), {"clone", "-q", bare, path}) &&
           git(path, {"config", "user.name", "Test"}) &&
           git(path, {"config", "user.email", "test@localhost"});
}

int parentCount(const QString& dir) {
    // "rev-list --parents -n 1 HEAD" prints "<commit> <parent> <parent> ...".
    return gitOut(dir, {"rev-list", "--parents", "-n", "1", "HEAD"}).trimmed().split(' ').size() - 1;
}

} // namespace

class RemotesTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;  ///< the working repository, with `origin` and `backup`
    QString m_bare;  ///< origin, a local bare repository
    QString m_bare2; ///< backup

    /// Pushes a change on the remote and makes a disjoint local commit: a branch
    /// that has genuinely diverged. The files differ so the merge and the rebase
    /// have nothing to conflict over — the test is about the mode, not about text.
    void diverge(const QString& sideName, const QString& remoteFile, const QString& localFile);

private slots:
    void init();
    void cleanup();

    void fetchAllFetchesEveryRemote();
    void aRemoteTrackingBranchCanBeMerged();
    void pullFfOnlyRefusesDivergedHistory();
    void pullMergeJoinsDivergedHistory();
    void pullRebaseReplaysLocalCommits();
    void pushSetsUpstreamOnFirstPush();
    void pushTagPushesOneAndPushTagsPushesAll();
    void setUpstreamMakesTheBranchFollow();
    void deleteTagRemovesTheName();
    void deleteRemoteTagRemovesItOnEveryRemote();
    void forceWithLeaseOverwritesARewrittenBranch();
    void forceWithLeaseRefusesAStaleLease();
    void autoFetchFetchesOnlyTheChosenRemote();
    void theWindowRunsNetworkOperations();
    void theWindowForcePushesAndAutoFetches();
};

void RemotesTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    const QString root = m_dir->path();
    m_path = root + "/work";
    m_bare = root + "/origin.git";
    m_bare2 = root + "/backup.git";

    QVERIFY(git(root, {"init", "-q", "--bare", "-b", "main", m_bare}));
    QVERIFY(git(root, {"init", "-q", "--bare", "-b", "main", m_bare2}));
    QVERIFY(git(root, {"init", "-q", "-b", "main", m_path}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    QVERIFY(writeFile(m_path, "f.txt", "base\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "base"}));
    QVERIFY(git(m_path, {"remote", "add", "origin", m_bare}));
    QVERIFY(git(m_path, {"remote", "add", "backup", m_bare2}));
    QVERIFY(git(m_path, {"push", "-q", "-u", "origin", "main"}));
    QVERIFY(git(m_path, {"push", "-q", "backup", "main"}));
}

void RemotesTest::cleanup() {
    m_dir.reset();
}

void RemotesTest::diverge(const QString& sideName, const QString& remoteFile,
                          const QString& localFile) {
    const QString side = m_dir->path() + "/" + sideName;
    QVERIFY(cloneAt(m_bare, side));
    QVERIFY(writeFile(side, remoteFile, "remote\n"));
    QVERIFY(git(side, {"add", "-A"}));
    QVERIFY(git(side, {"commit", "-qm", "remote change"}));
    QVERIFY(git(side, {"push", "-q", "origin", "main"}));

    QVERIFY(writeFile(m_path, localFile, "local\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "local change"}));
}

void RemotesTest::aRemoteTrackingBranchCanBeMerged() {
    // The merge a pull makes, done by hand: fetch, then merge origin/main. The name is
    // a remote-tracking branch, which the merge used to look up as a local one and
    // refuse with "no such branch".
    diverge(QStringLiteral("side"), QStringLiteral("remote.txt"), QStringLiteral("local.txt"));
    QVERIFY(git(m_path, {"fetch", "-q", "origin"}));
    const QString theirs = gitOut(m_path, {"rev-parse", "origin/main"}).trimmed();
    const QString ours = gitOut(m_path, {"rev-parse", "main"}).trimmed();

    RepoSession session;
    QVERIFY(session.open(m_path));
    MergeResult result;
    QString error;
    QVERIFY2(session.mergeBranch(QStringLiteral("origin/main"), &result, &error),
             qPrintable(error));
    QCOMPARE(result.status, MergeStatus::Clean);
    QCOMPARE(gitOut(m_path, {"rev-parse", "HEAD^1"}).trimmed(), ours);
    QCOMPARE(gitOut(m_path, {"rev-parse", "HEAD^2"}).trimmed(), theirs);
    QCOMPARE(gitOut(m_path, {"symbolic-ref", "--short", "HEAD"}).trimmed(), QStringLiteral("main"));

    // And the fast-forward case: nothing local since, so main simply advances.
    const QString side = m_dir->path() + "/side";
    QVERIFY(writeFile(side, "more.txt", "more\n"));
    QVERIFY(git(side, {"add", "-A"}));
    QVERIFY(git(side, {"commit", "-qm", "more"}));
    QVERIFY(git(side, {"pull", "-q", "--no-rebase", "origin", "main"}));
    QVERIFY(git(m_path, {"push", "-q", "origin", "main"}));
    QVERIFY(git(side, {"pull", "-q", "--no-rebase", "origin", "main"}));
    QVERIFY(git(side, {"push", "-q", "origin", "main"}));
    QVERIFY(git(m_path, {"fetch", "-q", "origin"}));
    QVERIFY2(session.mergeBranch(QStringLiteral("origin/main"), &result, &error),
             qPrintable(error));
    QCOMPARE(result.status, MergeStatus::FastForward);
    QCOMPARE(gitOut(m_path, {"rev-parse", "main"}).trimmed(),
             gitOut(m_path, {"rev-parse", "origin/main"}).trimmed());
}

void RemotesTest::fetchAllFetchesEveryRemote() {
    const QString side = m_dir->path() + "/side";
    QVERIFY(cloneAt(m_bare2, side));
    QVERIFY(writeFile(side, "s.txt", "from backup\n"));
    QVERIFY(git(side, {"add", "-A"}));
    QVERIFY(git(side, {"commit", "-qm", "backup work"}));
    QVERIFY(git(side, {"push", "-q", "origin", "main"}));
    const QString sideHead = gitOut(side, {"rev-parse", "HEAD"}).trimmed();

    const RemoteOps ops(m_path.toStdString());
    const SyncResult result = ops.fetchAll(true);
    QVERIFY2(result.ok, result.output.c_str());
    QCOMPARE(gitOut(m_path, {"rev-parse", "refs/remotes/backup/main"}).trimmed(), sideHead);
}

void RemotesTest::pullFfOnlyRefusesDivergedHistory() {
    diverge(QStringLiteral("side-ff"), "r.txt", "l.txt");

    const RemoteOps ops(m_path.toStdString());
    const SyncResult result = ops.pull("origin", "main", PullMode::FfOnly);
    QVERIFY(!result.ok);
    // Nothing moved: an ff-only pull that cannot fast-forward leaves the branch be.
    QCOMPARE(parentCount(m_path), 1);
    QCOMPARE(gitOut(m_path, {"rev-parse", "--abbrev-ref", "HEAD"}).trimmed(), QStringLiteral("main"));
}

void RemotesTest::pullMergeJoinsDivergedHistory() {
    diverge(QStringLiteral("side-merge"), "r.txt", "l.txt");

    const RemoteOps ops(m_path.toStdString());
    const SyncResult result = ops.pull("origin", "main", PullMode::Merge);
    QVERIFY2(result.ok, result.output.c_str());
    // A merge commit has two parents; that is what "--no-rebase" bought.
    QCOMPARE(parentCount(m_path), 2);
}

void RemotesTest::pullRebaseReplaysLocalCommits() {
    diverge(QStringLiteral("side-rebase"), "r.txt", "l.txt");

    const RemoteOps ops(m_path.toStdString());
    const SyncResult result = ops.pull("origin", "main", PullMode::Rebase);
    QVERIFY2(result.ok, result.output.c_str());
    // Linear: one parent, and the local commit is still in the history.
    QCOMPARE(parentCount(m_path), 1);
    QVERIFY(gitOut(m_path, {"log", "--format=%s"}).contains(QStringLiteral("local change")));
}

void RemotesTest::pushSetsUpstreamOnFirstPush() {
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(writeFile(m_path, "feat.txt", "feat\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "feat"}));

    RepoSession session;
    QVERIFY(session.open(m_path));
    QVERIFY(!session.upstreamInfo().configured);

    const SyncResult result =
        session.syncBlocking(SyncKind::Push, QStringLiteral("origin"), QStringLiteral("feature"));
    QVERIFY2(result.ok, result.output.c_str());

    const UpstreamInfo upstream = session.upstreamInfo();
    QVERIFY(upstream.configured);
    QCOMPARE(QString::fromStdString(upstream.name), QStringLiteral("origin/feature"));
}

void RemotesTest::pushTagPushesOneAndPushTagsPushesAll() {
    QVERIFY(git(m_path, {"tag", "v1"}));
    QVERIFY(git(m_path, {"tag", "v2"}));

    const RemoteOps ops(m_path.toStdString());
    const SyncResult one = ops.pushTag("origin", "v1");
    QVERIFY2(one.ok, one.output.c_str());
    QCOMPARE(gitOut(m_bare, {"tag"}).trimmed(), QStringLiteral("v1"));

    const SyncResult all = ops.pushTags("origin");
    QVERIFY2(all.ok, all.output.c_str());
    QCOMPARE(gitOut(m_bare, {"tag"}).trimmed().split('\n').size(), 2);
}

void RemotesTest::setUpstreamMakesTheBranchFollow() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    QString error;
    QVERIFY2(session.setUpstream(QStringLiteral("main"), QStringLiteral("origin/main"), &error),
             qPrintable(error));
    const UpstreamInfo upstream = session.upstreamInfo();
    QVERIFY(upstream.configured);
    QCOMPARE(QString::fromStdString(upstream.name), QStringLiteral("origin/main"));

    // A remote branch that does not exist is refused, with a reason rather than a
    // follow that goes nowhere.
    QVERIFY(!session.setUpstream(QStringLiteral("main"), QStringLiteral("origin/nope"), &error));
    QVERIFY(!error.isEmpty());
}

void RemotesTest::deleteTagRemovesTheName() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    const QString head = gitOut(m_path, {"rev-parse", "HEAD"}).trimmed();
    QString error;
    QVERIFY2(session.createTag(QStringLiteral("keep"), head, QString(), &error), qPrintable(error));
    QVERIFY2(session.deleteTag(QStringLiteral("keep"), &error), qPrintable(error));
    QVERIFY(gitOut(m_path, {"tag", "-l"}).trimmed().isEmpty());
}

void RemotesTest::deleteRemoteTagRemovesItOnEveryRemote() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    const QString head = gitOut(m_path, {"rev-parse", "HEAD"}).trimmed();
    QString error;
    QVERIFY2(session.createTag(QStringLiteral("v1"), head, QString(), &error), qPrintable(error));
    QVERIFY(git(m_path, {"push", "-q", "origin", "refs/tags/v1"}));
    QVERIFY(git(m_path, {"push", "-q", "backup", "refs/tags/v1"}));
    QCOMPARE(gitOut(m_bare, {"tag", "-l"}).trimmed(), QStringLiteral("v1"));
    QCOMPARE(gitOut(m_bare2, {"tag", "-l"}).trimmed(), QStringLiteral("v1"));

    // One remote at a time…
    const SyncResult origin = session.deleteRemoteTagBlocking(QStringLiteral("origin"),
                                                              QStringLiteral("v1"));
    QVERIFY2(origin.ok, origin.output.c_str());
    QVERIFY(gitOut(m_bare, {"tag", "-l"}).trimmed().isEmpty());
    QCOMPARE(gitOut(m_bare2, {"tag", "-l"}).trimmed(), QStringLiteral("v1"));

    // …and the local tag is untouched by a remote deletion.
    QCOMPARE(gitOut(m_path, {"tag", "-l"}).trimmed(), QStringLiteral("v1"));

    const SyncResult backup = session.deleteRemoteTagBlocking(QStringLiteral("backup"),
                                                              QStringLiteral("v1"));
    QVERIFY2(backup.ok, backup.output.c_str());
    QVERIFY(gitOut(m_bare2, {"tag", "-l"}).trimmed().isEmpty());
}

void RemotesTest::forceWithLeaseOverwritesARewrittenBranch() {
    const QString before = gitOut(m_bare, {"rev-parse", "refs/heads/main"}).trimmed();

    // Amend the tip so a plain push cannot fast-forward.
    QVERIFY(writeFile(m_path, "f.txt", "amended\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "--amend", "-qm", "amended"}));
    const QString amended = gitOut(m_path, {"rev-parse", "HEAD"}).trimmed();

    const RemoteOps ops(m_path.toStdString());
    const SyncResult normal = ops.push("origin", "main", false);
    QVERIFY(!normal.ok);
    QCOMPARE(gitOut(m_bare, {"rev-parse", "refs/heads/main"}).trimmed(), before);

    const SyncResult forced = ops.push("origin", "main", false, PushMode::ForceWithLease);
    QVERIFY2(forced.ok, forced.output.c_str());
    QCOMPARE(gitOut(m_bare, {"rev-parse", "refs/heads/main"}).trimmed(), amended);
}

void RemotesTest::forceWithLeaseRefusesAStaleLease() {
    QVERIFY(writeFile(m_path, "f.txt", "amended\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "--amend", "-qm", "amended"}));

    // Somebody else pushes after our last fetch, so our remote-tracking ref is stale.
    const QString side = m_dir->path() + "/side-lease";
    QVERIFY(cloneAt(m_bare, side));
    QVERIFY(writeFile(side, "s.txt", "side\n"));
    QVERIFY(git(side, {"add", "-A"}));
    QVERIFY(git(side, {"commit", "-qm", "side work"}));
    QVERIFY(git(side, {"push", "-q", "origin", "main"}));

    const RemoteOps ops(m_path.toStdString());
    const SyncResult stale = ops.push("origin", "main", false, PushMode::ForceWithLease);
    QVERIFY(!stale.ok);

    // Fetch first, and the lease now matches what we are overwriting.
    const SyncResult fetched = ops.fetch("origin", true);
    QVERIFY2(fetched.ok, fetched.output.c_str());
    const SyncResult forced = ops.push("origin", "main", false, PushMode::ForceWithLease);
    QVERIFY2(forced.ok, forced.output.c_str());
}

void RemotesTest::autoFetchFetchesOnlyTheChosenRemote() {
    const QString side = m_dir->path() + "/side-origin";
    QVERIFY(cloneAt(m_bare, side));
    QVERIFY(writeFile(side, "o.txt", "o\n"));
    QVERIFY(git(side, {"add", "-A"}));
    QVERIFY(git(side, {"commit", "-qm", "origin work"}));
    QVERIFY(git(side, {"push", "-q", "origin", "main"}));
    const QString originHead = gitOut(side, {"rev-parse", "HEAD"}).trimmed();

    const QString side2 = m_dir->path() + "/side-backup";
    QVERIFY(cloneAt(m_bare2, side2));
    QVERIFY(writeFile(side2, "b.txt", "b\n"));
    QVERIFY(git(side2, {"add", "-A"}));
    QVERIFY(git(side2, {"commit", "-qm", "backup work"}));
    QVERIFY(git(side2, {"push", "-q", "origin", "main"}));
    const QString backupHead = gitOut(side2, {"rev-parse", "HEAD"}).trimmed();

    RepoSession session;
    QVERIFY(session.open(m_path));
    const SyncResult result = session.fetchBlocking({QStringLiteral("origin")}, true);
    QVERIFY2(result.ok, result.output.c_str());

    QCOMPARE(gitOut(m_path, {"rev-parse", "refs/remotes/origin/main"}).trimmed(), originHead);
    // `backup` was not in the selection, so its remote-tracking ref stays where it was.
    QVERIFY(gitOut(m_path, {"rev-parse", "refs/remotes/backup/main"}).trimmed() != backupHead);
}

void RemotesTest::theWindowRunsNetworkOperations() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    const QStringList fetched = window.syncForDiagnostics(QStringLiteral("fetch"), QString());
    const QString fetchedText = fetched.join(QLatin1Char('\n'));
    QVERIFY2(fetchedText.contains(QStringLiteral("result: ok")), qPrintable(fetchedText));

    QVERIFY(git(m_path, {"tag", "w1"}));
    const QStringList pushed =
        window.syncForDiagnostics(QStringLiteral("push-tag"), QStringLiteral("w1"));
    const QString pushedText = pushed.join(QLatin1Char('\n'));
    QVERIFY2(pushedText.contains(QStringLiteral("result: ok")), qPrintable(pushedText));
    QVERIFY(gitOut(m_bare, {"tag"}).contains(QStringLiteral("w1")));

    const QStringList upstream =
        window.setUpstreamForDiagnostics(QStringLiteral("origin/main"));
    const QString upstreamText = upstream.join(QLatin1Char('\n'));
    QVERIFY2(upstreamText.contains(QStringLiteral("configured: origin/main")),
             qPrintable(upstreamText));
}

void RemotesTest::theWindowForcePushesAndAutoFetches() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    // Force push through the window's own path: amend, then rewrite with a lease.
    QVERIFY(writeFile(m_path, "f.txt", "amended\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "--amend", "-qm", "amended"}));
    const QString amended = gitOut(m_path, {"rev-parse", "HEAD"}).trimmed();

    const QStringList forced =
        window.syncForDiagnostics(QStringLiteral("push-force"), QStringLiteral("origin"));
    const QString forcedText = forced.join(QLatin1Char('\n'));
    QVERIFY2(forcedText.contains(QStringLiteral("result: ok")), qPrintable(forcedText));
    QCOMPARE(gitOut(m_bare, {"rev-parse", "refs/heads/main"}).trimmed(), amended);

    // Auto-fetch: it is off until asked, then a tick brings a pushed commit in.
    QVERIFY(!window.autoFetchEnabled());
    window.setAutoFetchIntervalMs(50);
    window.setAutoFetch(true);
    QVERIFY(window.autoFetchEnabled());
    QVERIFY(window.autoFetchRemotes().isEmpty()); // empty means every remote

    const QString side = m_dir->path() + "/side-auto";
    QVERIFY(cloneAt(m_bare, side));
    QVERIFY(writeFile(side, "auto.txt", "auto\n"));
    QVERIFY(git(side, {"add", "-A"}));
    QVERIFY(git(side, {"commit", "-qm", "auto work"}));
    QVERIFY(git(side, {"push", "-q", "origin", "main"}));
    const QString sideHead = gitOut(side, {"rev-parse", "HEAD"}).trimmed();

    QTRY_VERIFY_WITH_TIMEOUT(
        gitOut(m_path, {"rev-parse", "refs/remotes/origin/main"}).trimmed() == sideHead, 5000);

    window.setAutoFetch(false);
    QVERIFY(!window.autoFetchEnabled());
}

QTEST_MAIN(RemotesTest)
#include "test_remotes.moc"
