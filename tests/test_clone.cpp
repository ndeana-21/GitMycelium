#include "core/types.h"
#include "git/cli.h"
#include "git/clone.h"
#include "git/repository.h"

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

bool writeFile(const QString& path, const QString& content) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content.toUtf8()) == content.toUtf8().size();
}

} // namespace

class CloneTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_origin;

private slots:
    void initTestCase();
    void init();

    void aCloneLandsAndOpens();
    void theBranchOptionChecksOutThatBranch();
    void aPartialCloneIsPromisor();
    void aShallowSinceCloneLimitsHistoryByDate();
    void aBadUrlFailsWithGitsOwnMessage();
};

void CloneTest::initTestCase() {
    Repository::initLibrary();
}

void CloneTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());

    const QString work = QDir(m_dir->path()).filePath(QStringLiteral("work"));
    QVERIFY(QDir().mkpath(work));
    QVERIFY(git(work, {"init", "-q", "-b", "main"}));
    QVERIFY(git(work, {"config", "user.name", "Demo"}));
    QVERIFY(git(work, {"config", "user.email", "demo@example.com"}));
    QVERIFY(git(work, {"config", "commit.gpgsign", "false"}));
    QVERIFY(writeFile(QDir(work).filePath(QStringLiteral("a.txt")), QStringLiteral("a\n")));
    QVERIFY(git(work, {"add", "-A"}));
    QVERIFY(git(work, {"commit", "-qm", "one"}));
    QVERIFY(writeFile(QDir(work).filePath(QStringLiteral("a.txt")), QStringLiteral("a\nb\n")));
    QVERIFY(git(work, {"commit", "-qam", "two"}));
    QVERIFY(writeFile(QDir(work).filePath(QStringLiteral("c.txt")), QStringLiteral("c\n")));
    QVERIFY(git(work, {"add", "-A"}));
    QVERIFY(git(work, {"commit", "-qm", "three"}));
    QVERIFY(git(work, {"branch", "feature"}));

    m_origin = QDir(m_dir->path()).filePath(QStringLiteral("origin.git"));
    QVERIFY(git(m_dir->path(), {"clone", "-q", "--bare", work, m_origin}));
    // The bare origin keeps the source's HEAD, which is the first branch created;
    // pin it so the default branch is not an accident of the fixture.
    QVERIFY(git(m_origin, {"symbolic-ref", "HEAD", "refs/heads/main"}));
    // A file:// clone only filters when the origin advertises it.
    QVERIFY(git(m_origin, {"config", "uploadpack.allowFilter", "true"}));
}

void CloneTest::aCloneLandsAndOpens() {
    const QString dest = QDir(m_dir->path()).filePath(QStringLiteral("clone"));
    const CloneResult result = cloneRepository(m_origin.toStdString(), dest.toStdString());
    QVERIFY2(result.ok, result.output.c_str());

    auto repo = Repository::open(dest.toStdString());
    QVERIFY(repo);
    QCOMPARE(QString::fromStdString(repo->info().headBranch), QStringLiteral("main"));
    QCOMPARE(gitOut(dest, {"rev-list", "--count", "HEAD"}), QStringLiteral("3"));
    QCOMPARE(gitOut(dest, {"remote", "get-url", "origin"}), m_origin);
    QVERIFY(!repo->info().partialClone);
}

void CloneTest::theBranchOptionChecksOutThatBranch() {
    const QString dest = QDir(m_dir->path()).filePath(QStringLiteral("branch-clone"));
    CloneOptions options;
    options.branch = "feature";
    const CloneResult result =
        cloneRepository(m_origin.toStdString(), dest.toStdString(), options);
    QVERIFY2(result.ok, result.output.c_str());

    auto repo = Repository::open(dest.toStdString());
    QVERIFY(repo);
    QCOMPARE(QString::fromStdString(repo->info().headBranch), QStringLiteral("feature"));
}

void CloneTest::aPartialCloneIsPromisor() {
    const QString dest = QDir(m_dir->path()).filePath(QStringLiteral("partial"));
    CloneOptions options;
    options.partial = true;
    const std::string url = "file://" + m_origin.toStdString();
    const CloneResult result = cloneRepository(url, dest.toStdString(), options);
    QVERIFY2(result.ok, result.output.c_str());

    auto repo = Repository::open(dest.toStdString());
    QVERIFY(repo);
    QVERIFY(repo->info().partialClone);
}

void CloneTest::aShallowSinceCloneLimitsHistoryByDate() {
    const QString work = QDir(m_dir->path()).filePath(QStringLiteral("dated"));
    QVERIFY(QDir().mkpath(work));
    QVERIFY(git(work, {"init", "-q", "-b", "main"}));
    QVERIFY(git(work, {"config", "user.name", "D"}));
    QVERIFY(git(work, {"config", "user.email", "d@example.com"}));

    const QByteArray oldAuthor = qgetenv("GIT_AUTHOR_DATE");
    const QByteArray oldCommit = qgetenv("GIT_COMMITTER_DATE");
    qputenv("GIT_AUTHOR_DATE", "2018-01-01T00:00:00");
    qputenv("GIT_COMMITTER_DATE", "2018-01-01T00:00:00");
    QVERIFY(writeFile(QDir(work).filePath(QStringLiteral("a.txt")), QStringLiteral("a\n")));
    QVERIFY(git(work, {"add", "-A"}));
    QVERIFY(git(work, {"commit", "-qm", "old"}));
    qputenv("GIT_AUTHOR_DATE", "2026-01-01T00:00:00");
    qputenv("GIT_COMMITTER_DATE", "2026-01-01T00:00:00");
    QVERIFY(writeFile(QDir(work).filePath(QStringLiteral("b.txt")), QStringLiteral("b\n")));
    QVERIFY(git(work, {"add", "-A"}));
    QVERIFY(git(work, {"commit", "-qm", "new"}));
    if (oldAuthor.isEmpty()) {
        qunsetenv("GIT_AUTHOR_DATE");
    } else {
        qputenv("GIT_AUTHOR_DATE", oldAuthor);
    }
    if (oldCommit.isEmpty()) {
        qunsetenv("GIT_COMMITTER_DATE");
    } else {
        qputenv("GIT_COMMITTER_DATE", oldCommit);
    }

    const QString origin = QDir(m_dir->path()).filePath(QStringLiteral("dated.git"));
    QVERIFY(git(m_dir->path(), {"clone", "-q", "--bare", work, origin}));
    QVERIFY(git(origin, {"symbolic-ref", "HEAD", "refs/heads/main"}));

    const QString dest = QDir(m_dir->path()).filePath(QStringLiteral("since-clone"));
    CloneOptions options;
    options.shallowSince = "2020-01-01";
    // A depth is also set: git refuses the two together, so the date wins and the
    // clone still succeeds.
    options.depth = 5;
    const CloneResult result =
        cloneRepository("file://" + origin.toStdString(), dest.toStdString(), options);
    QVERIFY2(result.ok, result.output.c_str());
    QCOMPARE(gitOut(dest, {"rev-list", "--count", "HEAD"}), QStringLiteral("1"));
    QCOMPARE(gitOut(dest, {"rev-parse", "--is-shallow-repository"}), QStringLiteral("true"));
}

void CloneTest::aBadUrlFailsWithGitsOwnMessage() {
    const QString dest = QDir(m_dir->path()).filePath(QStringLiteral("never"));
    const std::string missing = QDir(m_dir->path()).filePath(QStringLiteral("missing.git"))
                                    .toStdString();
    const CloneResult result = cloneRepository(missing, dest.toStdString());
    QVERIFY(!result.ok);
    QVERIFY(result.exitCode != 0);
    QVERIFY2(result.output.find("does not exist") != std::string::npos, result.output.c_str());
}

QTEST_MAIN(CloneTest)
#include "test_clone.moc"
