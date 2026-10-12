/// A clean/smudge filter only runs when git writes the file. libgit2 runs none, so
/// every path that rewrites the working tree has to go through the binary once a
/// filter is configured — git-lfs is the one readers have, and without it a file
/// comes back as its pointer. These use a stand-in filter that changes the case
/// (clean lowers, smudge raises), so a file that skipped the smudge is visible in
/// any environment, git-lfs installed or not.

#include "core/types.h"
#include "git/cli.h"
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
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, dir.toStdString()).out);
}

bool writeFile(const QString& dir, const QString& name, const QByteArray& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content);
    return true;
}

QByteArray readFile(const QString& dir, const QString& name) {
    QFile file(QDir(dir).filePath(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class FiltersTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void theRepositoryHoldsTheCleanedText();
    void discardingRunsTheSmudge();
    void aHardResetRunsTheSmudge();
    void undoingACheckoutRunsTheSmudge();
    void movingTheCheckedOutBranchRunsTheSmudge();
    void aLocalChangeStillStopsTheMove();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
    QString m_first;
    QString m_second;
};

void FiltersTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    QVERIFY(git(m_path, {"config", "filter.upper.clean", "tr A-Z a-z"}));
    QVERIFY(git(m_path, {"config", "filter.upper.smudge", "tr a-z A-Z"}));
    QVERIFY(writeFile(m_path, ".gitattributes", "*.up filter=upper\n"));
    QVERIFY(writeFile(m_path, "a.up", "HELLO\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-q", "-m", "hello"}));
    m_first = gitOut(m_path, {"rev-parse", "HEAD"}).trimmed();
    QVERIFY(writeFile(m_path, "a.up", "WORLD\n"));
    QVERIFY(git(m_path, {"commit", "-q", "-am", "world"}));
    m_second = gitOut(m_path, {"rev-parse", "HEAD"}).trimmed();
}

void FiltersTest::cleanup() {
    m_dir.reset();
}

void FiltersTest::theRepositoryHoldsTheCleanedText() {
    QCOMPARE(gitOut(m_path, {"cat-file", "-p", "HEAD:a.up"}), QStringLiteral("world\n"));
    QCOMPARE(readFile(m_path, "a.up"), QByteArray("WORLD\n"));
}

void FiltersTest::discardingRunsTheSmudge() {
    QVERIFY(writeFile(m_path, "a.up", "CHANGED\n"));
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.discardFileChanges(QStringLiteral("a.up"), &error), qPrintable(error));
    QCOMPARE(readFile(m_path, "a.up"), QByteArray("WORLD\n"));
}

void FiltersTest::aHardResetRunsTheSmudge() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.resetTo(m_first, ResetMode::Hard, &error), qPrintable(error));
    QCOMPARE(gitOut(m_path, {"rev-parse", "HEAD"}).trimmed(), m_first);
    QCOMPARE(readFile(m_path, "a.up"), QByteArray("HELLO\n"));
    QVERIFY(gitOut(m_path, {"status", "--porcelain"}).trimmed().isEmpty());
}

void FiltersTest::undoingACheckoutRunsTheSmudge() {
    QVERIFY(git(m_path, {"branch", "older", m_first}));
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.checkoutBranch(QStringLiteral("older"), &error), qPrintable(error));
    QCOMPARE(readFile(m_path, "a.up"), QByteArray("HELLO\n"));

    QString label;
    QVERIFY2(session.undo(&label, &error), qPrintable(error));
    QCOMPARE(gitOut(m_path, {"symbolic-ref", "--short", "HEAD"}).trimmed(), QStringLiteral("main"));
    QCOMPARE(readFile(m_path, "a.up"), QByteArray("WORLD\n"));
    QVERIFY(gitOut(m_path, {"status", "--porcelain"}).trimmed().isEmpty());
}

void FiltersTest::movingTheCheckedOutBranchRunsTheSmudge() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY2(session.moveBranch(QStringLiteral("main"), m_first, &error), qPrintable(error));
    QCOMPARE(gitOut(m_path, {"rev-parse", "main"}).trimmed(), m_first);
    QCOMPARE(readFile(m_path, "a.up"), QByteArray("HELLO\n"));
    QVERIFY(gitOut(m_path, {"status", "--porcelain"}).trimmed().isEmpty());
}

void FiltersTest::aLocalChangeStillStopsTheMove() {
    // The binary path must refuse what the libgit2 one refused: a move that would
    // overwrite an uncommitted edit leaves both the branch and the edit alone.
    QVERIFY(writeFile(m_path, "a.up", "MINE\n"));
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY(!session.moveBranch(QStringLiteral("main"), m_first, &error));
    QCOMPARE(gitOut(m_path, {"rev-parse", "main"}).trimmed(), m_second);
    QCOMPARE(readFile(m_path, "a.up"), QByteArray("MINE\n"));

    QVERIFY2(session.moveBranchForced(QStringLiteral("main"), m_first, &error), qPrintable(error));
    QCOMPARE(readFile(m_path, "a.up"), QByteArray("HELLO\n"));
}

QTEST_GUILESS_MAIN(FiltersTest)
#include "test_filters.moc"
