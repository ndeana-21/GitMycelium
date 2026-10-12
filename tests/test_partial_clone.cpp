/// A partial (promisor) clone: the fixture is a local bare origin plus a
/// `--filter=blob:none --no-local` clone, so nothing touches the network.

#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QSignalSpy>
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

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

/// Every object id present in the local packs, read without asking the promisor
/// remote (`--batch-all-objects` only lists what is on disk). This is how the
/// test proves a blob is genuinely absent rather than merely unreadable.
QStringList localObjectIds(const QString& dir) {
    const QString out = gitOut(dir, {"cat-file", "--batch-check", "--batch-all-objects"});
    QStringList ids;
    const QStringList lines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QStringList fields = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (!fields.isEmpty() && fields.at(0).size() == 40) {
            ids << fields.at(0);
        }
    }
    return ids;
}

} // namespace

class PartialCloneTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void theCloneIsReallyPartial();
    void libgit2ReadsCommitsAndTrees();
    void fileAtReadsABlobOutsideTheCheckout();
    void fileAtReturnsNoTextForABinaryBlobAndForAMissingPath();
    void fileDiffReadsThroughTheBinary();
    void stagedAndUnstagedDiffsReadThroughTheBinary();
    void theGitBinaryLazyFetchesTheMissingBlob();
    void aTreelessCloneStillListsChangesAndDiffs();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_clone;
    QString m_origin;
    QString m_base; ///< HEAD~2, whose a.txt blob is not checked out
    QString m_tip;
};

void PartialCloneTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    // A file:// clone is git's `file` transport; allow it for this process only.
    qputenv("GIT_ALLOW_PROTOCOL", "file");

    const QString work = QDir(m_dir->path()).filePath(QStringLiteral("work"));
    const QString origin = QDir(m_dir->path()).filePath(QStringLiteral("origin.git"));
    m_clone = QDir(m_dir->path()).filePath(QStringLiteral("clone"));

    QVERIFY(QDir().mkpath(work));
    QVERIFY(git(work, {"init", "-q", "-b", "main"}));
    QVERIFY(git(work, {"config", "user.name", "Test"}));
    QVERIFY(git(work, {"config", "user.email", "test@localhost"}));
    QVERIFY(writeFile(work, QStringLiteral("a.txt"), QStringLiteral("one\ntwo\nthree\n")));
    QVERIFY(git(work, {"add", "-A"}));
    QVERIFY(git(work, {"commit", "-q", "-m", "c1"}));
    QVERIFY(writeFile(work, QStringLiteral("a.txt"), QStringLiteral("one\nTWO\nthree\n")));
    QVERIFY(git(work, {"add", "-A"}));
    QVERIFY(git(work, {"commit", "-q", "-m", "c2"}));
    QVERIFY(QDir(work).mkpath(QStringLiteral("sub")));
    QVERIFY(writeFile(work, QStringLiteral("sub/b.txt"), QStringLiteral("x\ny\n")));
    // A binary blob (contains a NUL) to check the text/binary answer matches libgit2.
    QString binary;
    binary.append(QLatin1Char('B')).append(QChar(0)).append(QLatin1Char('X'));
    QVERIFY(writeFile(work, QStringLiteral("bin.dat"), binary));
    QVERIFY(git(work, {"add", "-A"}));
    QVERIFY(git(work, {"commit", "-q", "-m", "c3"}));

    QVERIFY(git(m_dir->path(), {"clone", "-q", "--bare", work, origin}));
    QVERIFY(git(origin, {"config", "uploadpack.allowFilter", "true"}));
    m_origin = origin;
    // --no-local forces the real transport, so the filter actually applies.
    QVERIFY(git(m_dir->path(),
                {"clone", "-q", "--filter=blob:none", "--no-local",
                 QStringLiteral("file://") + origin, m_clone}));

    m_tip = gitOut(m_clone, {"rev-parse", "HEAD"});
    m_base = gitOut(m_clone, {"rev-parse", "HEAD~2"});
    QCOMPARE(m_tip.size(), 40);
    QCOMPARE(m_base.size(), 40);
}

void PartialCloneTest::cleanup() {
    m_dir.reset();
}

void PartialCloneTest::theCloneIsReallyPartial() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_clone, &error), qPrintable(error));
    QVERIFY(session.info().partialClone);

    // The clone has a promisor pack...
    QVERIFY(!QDir(QDir(m_clone).filePath(QStringLiteral(".git/objects/pack")))
                 .entryList(QStringList{QStringLiteral("*.promisor")})
                 .isEmpty());

    // ...and HEAD~2's a.txt blob is genuinely not among the local objects: only the
    // checked-out revision's blobs were fetched.
    const QString oldBlob = gitOut(m_clone, {"rev-parse", "HEAD~2:a.txt"});
    const QString tipBlob = gitOut(m_clone, {"rev-parse", "HEAD:a.txt"});
    QCOMPARE(oldBlob.size(), 40);
    QVERIFY(oldBlob != tipBlob);
    const QStringList ids = localObjectIds(m_clone);
    QVERIFY2(!ids.contains(oldBlob), "the old blob should not be on disk");
    QVERIFY2(ids.contains(tipBlob), "the checked-out blob should be on disk");
}

void PartialCloneTest::libgit2ReadsCommitsAndTrees() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_clone, &error), qPrintable(error));

    // The commit objects and trees are present in a blob:none clone, so the graph
    // read works and must keep working.
    bool hasMore = true;
    const std::vector<CommitInfo> commits = session.commitsBlocking(0, 10, &hasMore);
    QCOMPARE(commits.size(), std::size_t(3));
    QVERIFY(!hasMore);
    QCOMPARE(QString::fromStdString(commits.front().id), m_tip);
    QCOMPARE(QString::fromStdString(commits.back().id), m_base);
    QCOMPARE(QString::fromStdString(commits.front().summary), QStringLiteral("c3"));

    // The file list between two commits is a tree diff, which needs no blobs.
    const std::vector<ChangedFile> files = session.changedFilesBetween(m_base, m_tip);
    QStringList paths;
    for (const ChangedFile& file : files) {
        paths << QString::fromStdString(file.path);
    }
    paths.sort();
    QCOMPARE(paths, QStringList({QStringLiteral("a.txt"), QStringLiteral("bin.dat"),
                                 QStringLiteral("sub/b.txt")}));
}

void PartialCloneTest::fileAtReadsABlobOutsideTheCheckout() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_clone, &error), qPrintable(error));

    // H-2: `fileAtRevision` now routes through the git binary for a promisor clone, so
    // a blob that was filtered out is fetched and returned — no more empty success.
    std::string bytes;
    QString readError;
    QVERIFY2(session.fileAtRevision(m_base, QStringLiteral("a.txt"), &bytes, &readError),
             qPrintable(readError));
    QCOMPARE(QString::fromStdString(bytes), QStringLiteral("one\ntwo\nthree\n"));

    // The binary is the oracle for the same read.
    const QString viaGit = gitOut(m_clone, {"show", "HEAD~2:a.txt"});
    QCOMPARE(QString::fromStdString(bytes), viaGit + QLatin1Char('\n'));

    std::string tip;
    QVERIFY(session.fileAtRevision(m_tip, QStringLiteral("a.txt"), &tip, nullptr));
    QCOMPARE(QString::fromStdString(tip), QStringLiteral("one\nTWO\nthree\n"));
}

void PartialCloneTest::fileAtReturnsNoTextForABinaryBlobAndForAMissingPath() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_clone, &error), qPrintable(error));

    // A binary blob is no text — the same answer libgit2 gives — so the read succeeds
    // but yields nothing rather than a truncated blob.
    std::string bytes;
    QVERIFY(session.fileAtRevision(m_tip, QStringLiteral("bin.dat"), &bytes, nullptr));
    QVERIFY2(bytes.empty(), "a blob with a NUL is not text");

    std::string absent;
    QVERIFY(session.fileAtRevision(m_base, QStringLiteral("sub/b.txt"), &absent, nullptr));
    QVERIFY(absent.empty());
}

void PartialCloneTest::fileDiffReadsThroughTheBinary() {
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_clone, &error), qPrintable(error));

    // H-3: `fileDiffBetween` routes through the git binary on a promisor clone, so a
    // path whose old blob is missing produces the diff instead of throwing.
    const FileDiff diff = session.fileDiffBetween(m_base, m_tip, QStringLiteral("a.txt"));
    QCOMPARE(diff.removed, 1);
    QCOMPARE(diff.added, 1);
    QCOMPARE(diff.hunks.size(), std::size_t(1));
    QVERIFY(!diff.binary);

    const DiffHunk& hunk = diff.hunks.front();
    bool sawRemovedTwo = false;
    bool sawAddedTwo = false;
    for (const DiffLine& line : hunk.lines) {
        if (line.kind == DiffLineKind::Removed && line.text == "two") {
            sawRemovedTwo = true;
        }
        if (line.kind == DiffLineKind::Added && line.text == "TWO") {
            sawAddedTwo = true;
        }
    }
    QVERIFY(sawRemovedTwo);
    QVERIFY(sawAddedTwo);

    // commitFileDiff takes the same path: c2 changed a.txt, and c2's parent blob is
    // missing from this clone.
    const QString c2 = gitOut(m_clone, {"rev-parse", "HEAD~1"});
    const FileDiff commitDiff = session.commitFileDiff(c2, QStringLiteral("a.txt"));
    QCOMPARE(commitDiff.removed, 1);
    QCOMPARE(commitDiff.added, 1);
}

void PartialCloneTest::stagedAndUnstagedDiffsReadThroughTheBinary() {
    // The index now names HEAD~2's a.txt, a blob this clone never fetched, while the
    // working tree and HEAD hold the newer one. Both sides of the Commit Panel's diff
    // need that blob; through libgit2 the read threw, and the window went down.
    const QString oldBlob = gitOut(m_clone, {"rev-parse", m_base + QStringLiteral(":a.txt")});
    qputenv("GIT_NO_LAZY_FETCH", "1");
    const bool presentBefore = git(m_clone, {"cat-file", "-e", oldBlob});
    qunsetenv("GIT_NO_LAZY_FETCH");
    QVERIFY(!presentBefore);
    QVERIFY(git(m_clone, {"read-tree", m_base}));

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_clone, &error), qPrintable(error));
    QSignalSpy failures(&session, &RepoSession::readFailed);

    const FileDiff unstaged = session.workdirFileDiff(QStringLiteral("a.txt"));
    QCOMPARE(unstaged.removed, 1);
    QCOMPARE(unstaged.added, 1);
    QCOMPARE(unstaged.hunks.size(), std::size_t(1));

    const FileDiff staged = session.stagedFileDiff(QStringLiteral("a.txt"));
    QCOMPARE(staged.removed, 1);
    QCOMPARE(staged.added, 1);
    bool sawRemovedUpper = false;
    for (const DiffLine& line : staged.hunks.front().lines) {
        sawRemovedUpper = sawRemovedUpper ||
                          (line.kind == DiffLineKind::Removed && line.text == "TWO");
    }
    QVERIFY(sawRemovedUpper);
    QCOMPARE(failures.count(), 0);
}

void PartialCloneTest::theGitBinaryLazyFetchesTheMissingBlob() {
    // The mechanism the CLI read path will rely on: the binary fetches the missing
    // object on demand from the promisor remote, and afterwards the object is local.
    const QString oldBlob = gitOut(m_clone, {"rev-parse", "HEAD~2:a.txt"});
    QVERIFY(!localObjectIds(m_clone).contains(oldBlob));

    const QString viaGit = gitOut(m_clone, {"show", "HEAD~2:a.txt"});
    QVERIFY(viaGit.contains(QStringLiteral("two")));
    QVERIFY2(localObjectIds(m_clone).contains(oldBlob), "the binary should have fetched it");

    // Read with a fresh session, so libgit2 sees the new pack: this is the state the
    // CLI fallback leaves behind.
    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_clone, &error), qPrintable(error));
    std::string bytes;
    QVERIFY(session.fileAtRevision(m_base, QStringLiteral("a.txt"), &bytes, nullptr));
    QCOMPARE(QString::fromStdString(bytes), QStringLiteral("one\ntwo\nthree\n"));
}

void PartialCloneTest::aTreelessCloneStillListsChangesAndDiffs() {
    // A `--filter=tree:0` clone has no trees either, so libgit2 cannot even compare two
    // commits; the file list and the diffs both come from the binary.
    const QString treeless = QDir(m_dir->path()).filePath(QStringLiteral("treeless"));
    QVERIFY(git(m_dir->path(),
                {"clone", "-q", "--filter=tree:0", "--no-local",
                 QStringLiteral("file://") + m_origin, treeless}));

    RepoSession session;
    QString error;
    QVERIFY2(session.open(treeless, &error), qPrintable(error));
    QVERIFY(session.info().partialClone);

    // The graph still walks: commit objects are present.
    bool hasMore = false;
    QCOMPARE(session.commitsBlocking(0, 10, &hasMore).size(), std::size_t(3));

    const std::vector<ChangedFile> files = session.changedFilesBetween(m_base, m_tip);
    QStringList paths;
    for (const ChangedFile& file : files) {
        paths << QString::fromStdString(file.path);
    }
    paths.sort();
    QCOMPARE(paths, QStringList({QStringLiteral("a.txt"), QStringLiteral("bin.dat"),
                                 QStringLiteral("sub/b.txt")}));

    const QString c2 = gitOut(treeless, {"rev-parse", "HEAD~1"});
    const FileDiff diff = session.commitFileDiff(c2, QStringLiteral("a.txt"));
    QCOMPARE(diff.removed, 1);
    QCOMPARE(diff.added, 1);
}

QTEST_MAIN(PartialCloneTest)
#include "test_partial_clone.moc"
