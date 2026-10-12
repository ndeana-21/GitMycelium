#include "core/types.h"
#include "git/cli.h"
#include "git/repository.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

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

bool writeFile(const QString& path, const QString& content) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content.toUtf8()) == content.toUtf8().size();
}

QStringList pathsOf(const std::vector<ChangedFile>& files) {
    QStringList out;
    for (const ChangedFile& file : files) {
        out << QString::fromStdString(file.path);
    }
    out.sort();
    return out;
}

bool hasPath(const std::vector<FileChange>& changes, const QString& path) {
    return std::any_of(changes.begin(), changes.end(), [&](const FileChange& change) {
        return QString::fromStdString(change.path) == path;
    });
}

bool hasPath(const std::vector<ChangedFile>& changes, const QString& path) {
    return std::any_of(changes.begin(), changes.end(), [&](const ChangedFile& change) {
        return QString::fromStdString(change.path) == path;
    });
}

} // namespace

class CompareTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;

private slots:
    void initTestCase();
    void init();

    void twoDotCountsBothSides();
    void theMergeBaseRangeExcludesTheOtherSide();
    void workdirCompareShowsTheTrackedChange();
    void workdirCompareLeavesUntrackedAlone();
    void workdirPathDiffHasTheChange();
};

void CompareTest::initTestCase() {
    Repository::initLibrary();
}

void CompareTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Demo Dev"}));
    QVERIFY(git(m_path, {"config", "user.email", "demo@example.com"}));
    QVERIFY(git(m_path, {"config", "commit.gpgsign", "false"}));
    QVERIFY(writeFile(QDir(m_path).filePath(QStringLiteral("a.txt")), QStringLiteral("a\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "base"}));
    QVERIFY(writeFile(QDir(m_path).filePath(QStringLiteral("m.txt")), QStringLiteral("m\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "on main"}));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature", "HEAD~1"}));
    QVERIFY(writeFile(QDir(m_path).filePath(QStringLiteral("f.txt")), QStringLiteral("f\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "on feature"}));
    QVERIFY(writeFile(QDir(m_path).filePath(QStringLiteral("a.txt")), QStringLiteral("a2\n")));
    QVERIFY(writeFile(QDir(m_path).filePath(QStringLiteral("u.txt")), QStringLiteral("u\n")));
}

void CompareTest::twoDotCountsBothSides() {
    auto repo = Repository::open(m_path.toStdString());
    QVERIFY(repo);
    const Oid feature = repo->resolveRevision("feature");
    const Oid main = repo->resolveRevision("main");
    const std::vector<ChangedFile> files = repo->changedFilesBetween(feature, main);
    QCOMPARE(pathsOf(files), QStringList({QStringLiteral("f.txt"), QStringLiteral("m.txt")}));
}

void CompareTest::theMergeBaseRangeExcludesTheOtherSide() {
    auto repo = Repository::open(m_path.toStdString());
    QVERIFY(repo);
    const Oid feature = repo->resolveRevision("feature");
    const Oid main = repo->resolveRevision("main");
    const Oid base = repo->mergeBase(feature, main);
    QVERIFY(!base.empty());

    const std::vector<ChangedFile> files = repo->changedFilesBetween(base, main);
    QCOMPARE(pathsOf(files), QStringList({QStringLiteral("m.txt")}));
}

void CompareTest::workdirCompareShowsTheTrackedChange() {
    auto repo = Repository::open(m_path.toStdString());
    QVERIFY(repo);
    const Oid head = repo->resolveRevision("HEAD");
    const std::vector<ChangedFile> files = repo->workdirChangedFilesAgainst(head);
    QCOMPARE(pathsOf(files), QStringList({QStringLiteral("a.txt")}));
}

void CompareTest::workdirCompareLeavesUntrackedAlone() {
    auto repo = Repository::open(m_path.toStdString());
    QVERIFY(repo);
    // An untracked file is new to the tree, not a difference from it; `git diff <rev>`
    // does not list it, and neither does this. Status does, and that is the contrast.
    QVERIFY(hasPath(repo->status(), QStringLiteral("u.txt")));
    const Oid head = repo->resolveRevision("HEAD");
    QVERIFY(!hasPath(repo->workdirChangedFilesAgainst(head), QStringLiteral("u.txt")));
}

void CompareTest::workdirPathDiffHasTheChange() {
    auto repo = Repository::open(m_path.toStdString());
    QVERIFY(repo);
    const Oid head = repo->resolveRevision("HEAD");
    const FileDiff diff = repo->workdirFileDiffAgainst(head, "a.txt");
    QCOMPARE(diff.added, 1);
    QCOMPARE(diff.removed, 1);
    QVERIFY(!diff.hunks.empty());

    const FileDiff clean = repo->workdirFileDiffAgainst(head, "f.txt");
    QVERIFY(clean.hunks.empty());
}

QTEST_MAIN(CompareTest)
#include "test_compare.moc"
