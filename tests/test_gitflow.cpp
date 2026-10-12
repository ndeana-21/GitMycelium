#include "git/cli.h"
#include "git/gitflow.h"
#include "repo/repo_session.h"

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

bool writeFile(const QString& path, const QByteArray& content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return false;
    }
    return f.write(content) == content.size();
}

} // namespace

class GitFlowTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void kindNamesRoundTrip();
    void initWritesTheConfigAndCreatesDevelop();
    void startFeatureBranchesFromDevelop();
    void finishFeatureMergesIntoDevelopAndDeletesTheBranch();
    void startHotfixBranchesFromMaster();
    void finishReleaseTagsMasterAndMergesBackIntoDevelop();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void GitFlowTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "master"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    QVERIFY(writeFile(m_path + "/a.txt", "base\n"));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, {"commit", "-qm", "base"}));
    QVERIFY(git(m_path, {"branch", "develop"}));
}

void GitFlowTest::cleanup() {
    m_dir.reset();
}

void GitFlowTest::kindNamesRoundTrip() {
    GitFlowKind kind = GitFlowKind::Feature;
    QVERIFY(gitFlowKindFromName("hotfix", &kind));
    QCOMPARE(kind, GitFlowKind::Hotfix);
    QVERIFY(!gitFlowKindFromName("nonsense", &kind));
    QCOMPARE(QString::fromLatin1(gitFlowKindName(GitFlowKind::Release)), QStringLiteral("release"));
}

void GitFlowTest::initWritesTheConfigAndCreatesDevelop() {
    QVERIFY(git(m_path, {"branch", "-D", "develop"}));
    RepoSession session;
    QVERIFY(session.open(m_path));
    QVERIFY(!session.gitflowInitialized());

    QString error;
    QVERIFY2(session.initGitflow(QStringLiteral("master"), QStringLiteral("develop"), &error),
             qPrintable(error));
    QVERIFY(session.gitflowInitialized());
    QVERIFY(git(m_path, {"rev-parse", "--verify", "--quiet", "refs/heads/develop"}));
    QVERIFY(session.gitflowSummary().join(QLatin1Char('\n')).contains(QStringLiteral("master: master")));
}

void GitFlowTest::startFeatureBranchesFromDevelop() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.initGitflow(QStringLiteral("master"), QStringLiteral("develop"), &error),
             qPrintable(error));
    QVERIFY2(session.gitflowStart(QStringLiteral("feature"), QStringLiteral("login"), &error),
             qPrintable(error));

    QCOMPARE(gitOut(m_path, {"rev-parse", "--abbrev-ref", "HEAD"}), QStringLiteral("feature/login"));
    // No commit made yet, so the topic branch sits exactly on develop.
    QCOMPARE(gitOut(m_path, {"rev-parse", "HEAD"}),
             gitOut(m_path, {"rev-parse", "develop"}));
}

void GitFlowTest::finishFeatureMergesIntoDevelopAndDeletesTheBranch() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.initGitflow(QStringLiteral("master"), QStringLiteral("develop"), &error),
             qPrintable(error));
    QVERIFY2(session.gitflowStart(QStringLiteral("feature"), QStringLiteral("login"), &error),
             qPrintable(error));
    QVERIFY(writeFile(m_path + "/a.txt", "feature work\n"));
    QVERIFY(git(m_path, {"commit", "-qam", "feature work"}));

    QVERIFY2(session.gitflowFinish(QStringLiteral("feature"), QStringLiteral("login"), QString(), &error),
             qPrintable(error));

    QCOMPARE(gitOut(m_path, {"rev-parse", "--abbrev-ref", "HEAD"}), QStringLiteral("develop"));
    QVERIFY(!git(m_path, {"rev-parse", "--verify", "--quiet", "refs/heads/feature/login"}));
    QVERIFY(git(m_path, {"merge-base", "--is-ancestor", "HEAD", "develop"}));
    // A merge commit, not a fast-forward: the branch point is kept.
    QVERIFY(gitOut(m_path, {"log", "-1", "--format=%P", "develop"}).contains(QLatin1Char(' ')));
}

void GitFlowTest::startHotfixBranchesFromMaster() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.initGitflow(QStringLiteral("master"), QStringLiteral("develop"), &error),
             qPrintable(error));
    // A commit on develop, so the two bases can be told apart.
    QVERIFY(git(m_path, {"checkout", "-q", "develop"}));
    QVERIFY(writeFile(m_path + "/a.txt", "develop work\n"));
    QVERIFY(git(m_path, {"commit", "-qam", "develop work"}));
    QVERIFY(git(m_path, {"checkout", "-q", "master"}));

    QVERIFY2(session.gitflowStart(QStringLiteral("hotfix"), QStringLiteral("urgent"), &error),
             qPrintable(error));
    QCOMPARE(gitOut(m_path, {"rev-parse", "--abbrev-ref", "HEAD"}), QStringLiteral("hotfix/urgent"));
    QCOMPARE(gitOut(m_path, {"rev-parse", "HEAD"}), gitOut(m_path, {"rev-parse", "master"}));
}

void GitFlowTest::finishReleaseTagsMasterAndMergesBackIntoDevelop() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.initGitflow(QStringLiteral("master"), QStringLiteral("develop"), &error),
             qPrintable(error));
    QVERIFY2(session.gitflowStart(QStringLiteral("release"), QStringLiteral("1.0"), &error),
             qPrintable(error));
    QVERIFY(writeFile(m_path + "/a.txt", "release work\n"));
    QVERIFY(git(m_path, {"commit", "-qam", "release work"}));
    const QString releaseSha = gitOut(m_path, {"rev-parse", "HEAD"});

    QVERIFY2(session.gitflowFinish(QStringLiteral("release"), QStringLiteral("1.0"),
                                   QStringLiteral("1.0.0"), &error),
             qPrintable(error));

    QVERIFY(git(m_path, {"rev-parse", "--verify", "--quiet", "refs/tags/1.0.0"}));
    QCOMPARE(gitOut(m_path, {"rev-parse", "--abbrev-ref", "HEAD"}), QStringLiteral("develop"));
    QVERIFY(!git(m_path, {"rev-parse", "--verify", "--quiet", "refs/heads/release/1.0"}));
    QVERIFY(git(m_path, {"merge-base", "--is-ancestor", releaseSha, "master"}));
    QVERIFY(git(m_path, {"merge-base", "--is-ancestor", releaseSha, "develop"}));
}

QTEST_MAIN(GitFlowTest)
#include "test_gitflow.moc"
