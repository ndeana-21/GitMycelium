#include "core/types.h"
#include "git/cli.h"
#include "git/remote.h"
#include "git/repository.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>
#include <memory>

using namespace gk;

namespace {

namespace fs = std::filesystem;

bool git(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, dir.toStdString()).exitCode == 0;
}

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

const QStringList kIdentity{"-c", "user.name=Test", "-c", "user.email=test@localhost"};

} // namespace

class RebaseAndSyncTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void rebaseReplaysCommitsOntoTheTarget();
    void rebaseReportsUpToDateWhenAlreadyBased();
    void rebaseStopsOnConflictAndContinues();
    void rebaseAbortRestoresTheBranch();
    void cliReportsRealExitCodes();
    void pushConfiguresUpstreamAndFetchUpdatesBehind();
    void pushWithoutRemoteFailsCleanly();

private:
    bool commitFile(const QString& name, const QString& content, const QString& message);
    std::unique_ptr<Repository> open() const;

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void RebaseAndSyncTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
}

void RebaseAndSyncTest::cleanup() {
    m_dir.reset();
}

bool RebaseAndSyncTest::commitFile(const QString& name, const QString& content,
                                   const QString& message) {
    if (!writeFile(m_path, name, content)) {
        return false;
    }
    if (!git(m_path, {"add", name})) {
        return false;
    }
    QStringList args = kIdentity;
    args << "commit" << "-q" << "-m" << message;
    return git(m_path, args);
}

std::unique_ptr<Repository> RebaseAndSyncTest::open() const {
    return Repository::open(m_path.toStdString());
}

void RebaseAndSyncTest::rebaseReplaysCommitsOntoTheTarget() {
    QVERIFY(commitFile("base.txt", "base\n", "base"));
    auto repo = open();
    const std::string base = repo->info().headId;

    repo->createBranch("feature", base);
    repo->checkoutBranch("feature");
    QVERIFY(commitFile("feature1.txt", "one\n", "feature one"));
    QVERIFY(commitFile("feature2.txt", "two\n", "feature two"));

    repo->checkoutBranch("main");
    QVERIFY(commitFile("main.txt", "main\n", "main work"));
    const std::string mainHead = repo->info().headId;

    const RebaseResult result = repo->rebaseOnto("feature", "main");
    QCOMPARE(result.status, RebaseStatus::Completed);
    QCOMPARE(result.commitsApplied, static_cast<std::size_t>(2));
    QVERIFY(!repo->rebaseInProgress());

    QCOMPARE(QString::fromStdString(repo->info().headBranch), QStringLiteral("feature"));
    const auto history = repo->commits(0, 4, nullptr);
    QCOMPARE(history.size(), static_cast<std::size_t>(4));
    QVERIFY(history.at(0).summary.find("feature two") != std::string::npos);
    QVERIFY(history.at(1).summary.find("feature one") != std::string::npos);
    QCOMPARE(QString::fromStdString(history.at(2).id), QString::fromStdString(mainHead));
    QVERIFY(history.at(3).summary.find("base") != std::string::npos);
    QVERIFY(fs::exists(fs::path(m_path.toStdString()) / "main.txt"));
    QVERIFY(fs::exists(fs::path(m_path.toStdString()) / "feature2.txt"));
}

void RebaseAndSyncTest::rebaseReportsUpToDateWhenAlreadyBased() {
    QVERIFY(commitFile("a.txt", "a\n", "first"));
    auto repo = open();
    const std::string base = repo->info().headId;
    repo->createBranch("feature", base);

    const RebaseResult result = repo->rebaseOnto("main", "feature");
    QCOMPARE(result.status, RebaseStatus::UpToDate);
    QCOMPARE(result.commitsApplied, static_cast<std::size_t>(0));
    QVERIFY(!repo->rebaseInProgress());
    QCOMPARE(QString::fromStdString(repo->info().headId), QString::fromStdString(base));
}

void RebaseAndSyncTest::rebaseStopsOnConflictAndContinues() {
    QVERIFY(commitFile("a.txt", "line1\nline2\nline3\n", "base"));
    auto repo = open();
    const std::string base = repo->info().headId;

    repo->createBranch("feature", base);
    repo->checkoutBranch("feature");
    QVERIFY(commitFile("a.txt", "line1\nfeature\nline3\n", "feature edit"));

    repo->checkoutBranch("main");
    QVERIFY(commitFile("a.txt", "line1\nmain\nline3\n", "main edit"));

    const RebaseResult conflicted = repo->rebaseOnto("feature", "main");
    QCOMPARE(conflicted.status, RebaseStatus::Conflicted);
    QCOMPARE(conflicted.conflictCount, static_cast<std::size_t>(1));
    QVERIFY(repo->rebaseInProgress());
    QCOMPARE(repo->info().operation, RepoOperation::Rebase);

    RebaseResult stillConflicted;
    const bool continued = [&] {
        try {
            stillConflicted = repo->rebaseContinue();
            return true;
        } catch (const GitError&) {
            return false;
        }
    }();
    QVERIFY(continued);
    QCOMPARE(stillConflicted.status, RebaseStatus::Conflicted);

    repo->resolveConflict("a.txt", "line1\nresolved\nline3\n");
    const RebaseResult finished = repo->rebaseContinue();
    QCOMPARE(finished.status, RebaseStatus::Completed);
    QVERIFY(!repo->rebaseInProgress());
    QCOMPARE(repo->info().operation, RepoOperation::None);

    QFile file(QDir(m_path).filePath("a.txt"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(file.readAll()), QStringLiteral("line1\nresolved\nline3\n"));
}

void RebaseAndSyncTest::rebaseAbortRestoresTheBranch() {
    QVERIFY(commitFile("a.txt", "line1\nline2\nline3\n", "base"));
    auto repo = open();
    const std::string base = repo->info().headId;

    repo->createBranch("feature", base);
    repo->checkoutBranch("feature");
    QVERIFY(commitFile("a.txt", "line1\nfeature\nline3\n", "feature edit"));
    const std::string featureHead = repo->info().headId;

    repo->checkoutBranch("main");
    QVERIFY(commitFile("a.txt", "line1\nmain\nline3\n", "main edit"));

    QVERIFY(repo->rebaseOnto("feature", "main").status == RebaseStatus::Conflicted);
    repo->rebaseAbort();

    QVERIFY(!repo->rebaseInProgress());
    QCOMPARE(repo->info().operation, RepoOperation::None);
    for (const RefInfo& ref : repo->refs()) {
        if (ref.shortName == "feature") {
            QCOMPARE(QString::fromStdString(ref.target), QString::fromStdString(featureHead));
        }
    }
}

void RebaseAndSyncTest::cliReportsRealExitCodes() {
    QVERIFY(commitFile("a.txt", "a\n", "first"));

    const ProcessResult ok = runGit({"rev-parse", "--verify", "HEAD"}, m_path.toStdString());
    QCOMPARE(ok.exitCode, 0);

    // A missing object makes git exit 128. Before the WEXITSTATUS fix this
    // surfaced as 32768 and every "did it fail?" check was meaningless.
    const ProcessResult missing =
        runGit({"rev-parse", "--verify", "refs/heads/nope"}, m_path.toStdString());
    QCOMPARE(missing.exitCode, 128);
}

void RebaseAndSyncTest::pushConfiguresUpstreamAndFetchUpdatesBehind() {
    QVERIFY(commitFile("a.txt", "a\n", "first"));

    // A bare repository plays the role of the remote: fully local, no network.
    // Its HEAD must point at the branch we are going to push, otherwise a clone
    // ends up with no checked-out branch and every later push fails.
    const QString remotePath = QDir(m_dir->path()).filePath("remote.git");
    QVERIFY(git(m_dir->path(), {"init", "-q", "--bare", "-b", "main", remotePath}));
    QVERIFY(git(m_path, {"remote", "add", "origin", remotePath}));

    const RemoteOps ops(m_path.toStdString());
    const auto remotes = ops.remotes();
    QCOMPARE(remotes.size(), static_cast<std::size_t>(1));
    QCOMPARE(QString::fromStdString(remotes.at(0).name), QStringLiteral("origin"));

    const SyncResult pushed = ops.push("origin", "main", true);
    QVERIFY2(pushed.ok, pushed.output.c_str());

    auto repo = open();
    const UpstreamInfo tracked = repo->upstreamInfo();
    QVERIFY(tracked.configured);
    QCOMPARE(QString::fromStdString(tracked.name), QStringLiteral("origin/main"));
    QCOMPARE(tracked.ahead, static_cast<std::size_t>(0));
    QCOMPARE(tracked.behind, static_cast<std::size_t>(0));

    const QString otherPath = QDir(m_dir->path()).filePath("other");
    QVERIFY(git(m_dir->path(), {"clone", "-q", "-b", "main", remotePath, otherPath}));
    QVERIFY(writeFile(otherPath, "b.txt", "b\n"));
    QVERIFY(git(otherPath, {"add", "b.txt"}));
    QStringList commitArgs = kIdentity;
    commitArgs << "commit" << "-q" << "-m" << "from elsewhere";
    QVERIFY(git(otherPath, commitArgs));
    QVERIFY(git(otherPath, {"push", "-q", "origin", "main"}));

    const SyncResult fetched = ops.fetch("origin", true);
    QVERIFY2(fetched.ok, fetched.output.c_str());

    const UpstreamInfo after = repo->upstreamInfo();
    QCOMPARE(after.behind, static_cast<std::size_t>(1));
    QCOMPARE(after.ahead, static_cast<std::size_t>(0));

    const SyncResult pulled = ops.pull("origin", "main", PullMode::FfOnly);
    QVERIFY2(pulled.ok, pulled.output.c_str());
    QVERIFY(fs::exists(fs::path(m_path.toStdString()) / "b.txt"));
    QCOMPARE(repo->upstreamInfo().behind, static_cast<std::size_t>(0));
}

void RebaseAndSyncTest::pushWithoutRemoteFailsCleanly() {
    QVERIFY(commitFile("a.txt", "a\n", "first"));
    const RemoteOps ops(m_path.toStdString());
    QVERIFY(ops.remotes().empty());

    const SyncResult result = ops.push("origin", "main", true);
    QVERIFY(!result.ok);
    // The real exit code must survive, not the raw wait status.
    QVERIFY(result.exitCode > 0 && result.exitCode < 256);
    QVERIFY(!result.output.empty());
}

QTEST_MAIN(RebaseAndSyncTest)
#include "test_rebase_and_sync.moc"
