#include "git/cli.h"
#include "git/repository.h"
#include "repo/repo_session.h"

#include <QDir>
#include <QFileInfo>
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

QString headBranch(const QString& repo) {
    return QString::fromStdString(runGit({"symbolic-ref", "--short", "HEAD"}, repo.toStdString()).out)
        .trimmed();
}

} // namespace

class CreateRepositoryTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void createsARepositoryGitCanOpen();
    void setsTheRequestedInitialBranch();
    void makesTheMissingDirectoryChain();
    void refusesToReinitialiseAnExistingRepository();
};

void CreateRepositoryTest::initTestCase() {
    Repository::initLibrary();
}

void CreateRepositoryTest::createsARepositoryGitCanOpen() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("new-repo"));

    QString error;
    QVERIFY(RepoSession::createRepository(path, QStringLiteral("main"), &error));
    QVERIFY(error.isEmpty());
    QVERIFY(QFileInfo(QDir(path).filePath(QStringLiteral(".git"))).exists());
    QVERIFY(git(path, {"rev-parse", "--git-dir"}));

    bool hasMore = true;
    QVERIFY(Repository::open(path.toStdString())->commits(0, 10, &hasMore).empty());
}

void CreateRepositoryTest::setsTheRequestedInitialBranch() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("trunked"));
    QVERIFY(RepoSession::createRepository(path, QStringLiteral("trunk")));
    QCOMPARE(headBranch(path), QStringLiteral("trunk"));
}

void CreateRepositoryTest::makesTheMissingDirectoryChain() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("a/b/c/deep"));
    QVERIFY(RepoSession::createRepository(path, QStringLiteral("main")));
    QVERIFY(QFileInfo(QDir(path).filePath(QStringLiteral(".git"))).exists());
}

void CreateRepositoryTest::refusesToReinitialiseAnExistingRepository() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("once"));
    QVERIFY(RepoSession::createRepository(path, QStringLiteral("main")));
    QString error;
    QVERIFY(!RepoSession::createRepository(path, QStringLiteral("main"), &error));
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(CreateRepositoryTest)
#include "test_create_repository.moc"
