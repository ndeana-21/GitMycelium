/// The git binary is the reference throughout: a staging call that says it worked
/// but leaves `git diff --cached` empty is worse than one that fails.

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
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, dir.toStdString()).out);
}

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    const QString full = QDir(dir).filePath(name);
    QDir().mkpath(QFileInfo(full).absolutePath());
    QFile file(full);
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

const QStringList kIdentity{"-c", "user.name=Test", "-c", "user.email=test@localhost"};

} // namespace

class StagingTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void stagingAModifiedFileShowsItAsStaged();
    void stagingAnUntrackedFileAddsIt();
    void stagingADeletionStagesTheRemoval();
    void stagingAGitStagedPathIsVisible();

    void unstagingAModifiedFileReturnsItToUnstaged();
    void unstagingANewFileRemovesItFromTheIndexAndKeepsItOnDisk();

    void stageAllTakesNewModifiedAndDeletedAtOnce();

    void discardingRestoresTheIndexVersionOnDisk();
    void discardingAnUntrackedFileDeletesIt();
    void discardingEverythingLeavesHeadAndKeepsIgnoredFiles();
    void discardingEverythingIsRefusedMidOperation();

    void committingTheIndexCreatesACommitGitAgreesWith();
    void aCommitOnAnUnbornHeadIsARootCommit();
    void anEmptyMessageIsRefused();
    void theFullCycleLeavesACleanStatus();
    void amendingFoldsTheIndexIntoHead();

private:
    bool commitFile(const QString& name, const QString& content, const QString& message);
    bool hasStatus(const QString& path, bool staged, ChangeKind kind) const;
    QString headId() const;

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
    std::unique_ptr<RepoSession> m_session;
};

void StagingTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    m_session = std::make_unique<RepoSession>();
    QString error;
    QVERIFY2(m_session->open(m_path, &error), qPrintable(error));
}

void StagingTest::cleanup() {
    m_session.reset();
    m_dir.reset();
}

bool StagingTest::commitFile(const QString& name, const QString& content,
                             const QString& message) {
    if (!writeFile(m_path, name, content)) {
        return false;
    }
    if (!git(m_path, {"add", "-A"})) {
        return false;
    }
    QStringList args = kIdentity;
    args << "commit" << "-q" << "-m" << message;
    return git(m_path, args);
}

bool StagingTest::hasStatus(const QString& path, bool staged, ChangeKind kind) const {
    for (const FileChange& change : m_session->status()) {
        if (QString::fromStdString(change.path) == path && change.staged == staged &&
            change.kind == kind) {
            return true;
        }
    }
    return false;
}

QString StagingTest::headId() const {
    return QString::fromStdString(m_session->info().headId);
}

void StagingTest::stagingAModifiedFileShowsItAsStaged() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"),
                       QStringLiteral("base")));
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("two\n")));
    QVERIFY(hasStatus(QStringLiteral("a.txt"), false, ChangeKind::Modified));

    QString error;
    QVERIFY2(m_session->stageFile(QStringLiteral("a.txt"), &error), qPrintable(error));

    QVERIFY(hasStatus(QStringLiteral("a.txt"), true, ChangeKind::Modified));
    // git must agree, or "staged" is only our opinion.
    QCOMPARE(gitOut(m_path, {"diff", "--cached", "--name-only"}).trimmed(),
             QStringLiteral("a.txt"));
}

void StagingTest::stagingAnUntrackedFileAddsIt() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"),
                       QStringLiteral("base")));
    QVERIFY(writeFile(m_path, QStringLiteral("new.txt"), QStringLiteral("fresh\n")));

    QString error;
    QVERIFY2(m_session->stageFile(QStringLiteral("new.txt"), &error), qPrintable(error));

    QVERIFY(hasStatus(QStringLiteral("new.txt"), true, ChangeKind::Added));
    QCOMPARE(gitOut(m_path, {"diff", "--cached", "--name-only"}).trimmed(),
             QStringLiteral("new.txt"));
}

void StagingTest::stagingADeletionStagesTheRemoval() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"),
                       QStringLiteral("base")));
    QVERIFY(QFile::remove(QDir(m_path).filePath(QStringLiteral("a.txt"))));

    QString error;
    QVERIFY2(m_session->stageFile(QStringLiteral("a.txt"), &error), qPrintable(error));

    QVERIFY(hasStatus(QStringLiteral("a.txt"), true, ChangeKind::Deleted));
    QCOMPARE(gitOut(m_path, {"diff", "--cached", "--name-status", "--diff-filter=D"}).trimmed(),
             QStringLiteral("D\ta.txt"));
}

void StagingTest::stagingAGitStagedPathIsVisible() {
    // libgit2 caches the index it read when the repository was opened; a file the
    // git binary staged must still be seen. Without the forced re-read the
    // unstaging below would find nothing.
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"),
                       QStringLiteral("base")));
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("two\n")));
    QVERIFY(git(m_path, {"add", "a.txt"}));

    QString error;
    QVERIFY2(m_session->unstageFile(QStringLiteral("a.txt"), &error), qPrintable(error));
    QVERIFY(hasStatus(QStringLiteral("a.txt"), false, ChangeKind::Modified));
}

void StagingTest::unstagingAModifiedFileReturnsItToUnstaged() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"),
                       QStringLiteral("base")));
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("two\n")));
    QString error;
    QVERIFY2(m_session->stageFile(QStringLiteral("a.txt"), &error), qPrintable(error));
    QVERIFY(hasStatus(QStringLiteral("a.txt"), true, ChangeKind::Modified));

    QVERIFY2(m_session->unstageFile(QStringLiteral("a.txt"), &error), qPrintable(error));

    QVERIFY(hasStatus(QStringLiteral("a.txt"), false, ChangeKind::Modified));
    QCOMPARE(gitOut(m_path, {"diff", "--cached", "--name-only"}).trimmed(), QString());
    // The working-tree content survives an unstage: it was never the index's to lose.
    QCOMPARE(readFile(m_path, QStringLiteral("a.txt")), QStringLiteral("two\n"));
}

void StagingTest::unstagingANewFileRemovesItFromTheIndexAndKeepsItOnDisk() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"),
                       QStringLiteral("base")));
    QVERIFY(writeFile(m_path, QStringLiteral("new.txt"), QStringLiteral("fresh\n")));
    QString error;
    QVERIFY2(m_session->stageFile(QStringLiteral("new.txt"), &error), qPrintable(error));
    QVERIFY(hasStatus(QStringLiteral("new.txt"), true, ChangeKind::Added));

    QVERIFY2(m_session->unstageFile(QStringLiteral("new.txt"), &error), qPrintable(error));

    QVERIFY(hasStatus(QStringLiteral("new.txt"), false, ChangeKind::Untracked));
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral("new.txt"))));
}

void StagingTest::stageAllTakesNewModifiedAndDeletedAtOnce() {
    QVERIFY(commitFile(QStringLiteral("keep.txt"), QStringLiteral("keep\n"),
                       QStringLiteral("base")));
    QVERIFY(commitFile(QStringLiteral("gone.txt"), QStringLiteral("gone\n"),
                       QStringLiteral("second")));
    writeFile(m_path, QStringLiteral("keep.txt"), QStringLiteral("changed\n"));
    QVERIFY(writeFile(m_path, QStringLiteral("added.txt"), QStringLiteral("added\n")));
    QVERIFY(QFile::remove(QDir(m_path).filePath(QStringLiteral("gone.txt"))));

    QString error;
    QVERIFY2(m_session->stageAll(&error), qPrintable(error));

    QCOMPARE(gitOut(m_path, {"status", "--porcelain"}).trimmed().isEmpty(), false);
    // Every change is staged, so the unstaged side of status is empty.
    QCOMPARE(gitOut(m_path, {"diff", "--name-only"}).trimmed(), QString());
    const QString staged = gitOut(m_path, {"diff", "--cached", "--name-status"});
    QVERIFY(staged.contains(QStringLiteral("added.txt")));
    QVERIFY(staged.contains(QStringLiteral("keep.txt")));
    QVERIFY(staged.contains(QStringLiteral("gone.txt")));
    QVERIFY(hasStatus(QStringLiteral("gone.txt"), true, ChangeKind::Deleted));
}

void StagingTest::discardingRestoresTheIndexVersionOnDisk() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("committed\n"),
                       QStringLiteral("base")));
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("scribble\n")));

    QString error;
    QVERIFY2(m_session->discardFileChanges(QStringLiteral("a.txt"), &error), qPrintable(error));

    QCOMPARE(readFile(m_path, QStringLiteral("a.txt")), QStringLiteral("committed\n"));
    QVERIFY(!hasStatus(QStringLiteral("a.txt"), false, ChangeKind::Modified));
}

void StagingTest::discardingAnUntrackedFileDeletesIt() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"),
                       QStringLiteral("base")));
    QVERIFY(writeFile(m_path, QStringLiteral("scratch.txt"), QStringLiteral("temp\n")));

    QString error;
    QVERIFY2(m_session->discardFileChanges(QStringLiteral("scratch.txt"), &error),
             qPrintable(error));

    QVERIFY(!QFile::exists(QDir(m_path).filePath(QStringLiteral("scratch.txt"))));
}

void StagingTest::committingTheIndexCreatesACommitGitAgreesWith() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"),
                       QStringLiteral("base")));
    const QString base = headId();
    QVERIFY(writeFile(m_path, QStringLiteral("b.txt"), QStringLiteral("two\n")));
    QString error;
    QVERIFY2(m_session->stageAll(&error), qPrintable(error));

    QString newId;
    QVERIFY2(m_session->createCommit(QStringLiteral("add b"), &newId, &error),
             qPrintable(error));

    QVERIFY(!newId.isEmpty());
    QCOMPARE(QString::fromStdString(m_session->info().headId), newId);
    QVERIFY(newId != base);
    QCOMPARE(gitOut(m_path, {"log", "-1", "--format=%s"}).trimmed(), QStringLiteral("add b"));
    // The parent is the base commit, and b.txt is really in the new tree.
    QCOMPARE(gitOut(m_path, {"rev-parse", "HEAD~1"}).trimmed(), base);
    QCOMPARE(gitOut(m_path, {"show", "--name-only", "--format=", "HEAD"}).trimmed(),
             QStringLiteral("b.txt"));
    QCOMPARE(gitOut(m_path, {"status", "--porcelain"}).trimmed(), QString());
}

void StagingTest::aCommitOnAnUnbornHeadIsARootCommit() {
    // No commits yet: HEAD is unborn and the commit has no parent.
    QVERIFY(writeFile(m_path, QStringLiteral("first.txt"), QStringLiteral("hello\n")));
    QString error;
    QVERIFY2(m_session->stageAll(&error), qPrintable(error));

    QString newId;
    QVERIFY2(m_session->createCommit(QStringLiteral("root"), &newId, &error), qPrintable(error));

    QVERIFY(!newId.isEmpty());
    QCOMPARE(gitOut(m_path, {"rev-list", "--count", "HEAD"}).trimmed(), QStringLiteral("1"));
    // A root commit's object has no parent line at all.
    QVERIFY(!gitOut(m_path, {"cat-file", "-p", "HEAD"}).contains(QStringLiteral("parent ")));
    QCOMPARE(git(m_path, {"rev-parse", "--verify", "HEAD^"}), false);
}

void StagingTest::anEmptyMessageIsRefused() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"),
                       QStringLiteral("base")));
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("two\n")));
    QString error;
    QVERIFY2(m_session->stageFile(QStringLiteral("a.txt"), &error), qPrintable(error));

    QString newId;
    QVERIFY(!m_session->createCommit(QString(), &newId, &error));
    QVERIFY(!error.isEmpty());
}

void StagingTest::theFullCycleLeavesACleanStatus() {
    QVERIFY(writeFile(m_path, QStringLiteral("one.txt"), QStringLiteral("1\n")));
    QString error;
    QVERIFY2(m_session->stageAll(&error), qPrintable(error));
    QString id;
    QVERIFY2(m_session->createCommit(QStringLiteral("first"), &id, &error), qPrintable(error));

    QVERIFY(writeFile(m_path, QStringLiteral("two.txt"), QStringLiteral("2\n")));
    QVERIFY2(m_session->stageAll(&error), qPrintable(error));
    QVERIFY2(m_session->createCommit(QStringLiteral("second"), &id, &error), qPrintable(error));

    QVERIFY(writeFile(m_path, QStringLiteral("three.txt"), QStringLiteral("3\n")));
    QVERIFY2(m_session->stageAll(&error), qPrintable(error));
    QVERIFY2(m_session->createCommit(QStringLiteral("third"), &id, &error), qPrintable(error));

    QCOMPARE(gitOut(m_path, {"status", "--porcelain"}).trimmed(), QString());
    QCOMPARE(gitOut(m_path, {"rev-list", "--count", "HEAD"}).trimmed(), QStringLiteral("3"));
}

void StagingTest::discardingEverythingLeavesHeadAndKeepsIgnoredFiles() {
    QVERIFY(commitFile(QStringLiteral(".gitignore"), QStringLiteral("*.log\n"),
                       QStringLiteral("ignore logs")));
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("base")));
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("staged\n")));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("and then more\n")));
    QVERIFY(writeFile(m_path, QStringLiteral("dir/new.txt"), QStringLiteral("untracked\n")));
    QVERIFY(writeFile(m_path, QStringLiteral("build.log"), QStringLiteral("ignored\n")));

    QString error;
    QVERIFY2(m_session->discardAllChanges(&error), qPrintable(error));

    // Staged and unstaged edits and the untracked folder are gone; an ignored file
    // is not a change, so "discard all changes" leaves it where it is.
    QCOMPARE(gitOut(m_path, {"status", "--porcelain"}).trimmed(), QString());
    QCOMPARE(readFile(m_path, QStringLiteral("a.txt")), QStringLiteral("one\n"));
    QVERIFY(!QFile::exists(QDir(m_path).filePath(QStringLiteral("dir/new.txt"))));
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral("build.log"))));
}

void StagingTest::discardingEverythingIsRefusedMidOperation() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "side"}));
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("side\n"), QStringLiteral("side")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("main\n"), QStringLiteral("main")));
    QStringList merge = kIdentity;
    merge << "merge" << "side";
    QVERIFY(!git(m_path, merge)); // conflicts

    // A reset would end the merge without a word; the reader aborts it instead.
    QString error;
    QVERIFY(!m_session->discardAllChanges(&error));
    QVERIFY(error.contains(QStringLiteral("in progress")));
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral(".git/MERGE_HEAD"))));
    QVERIFY(readFile(m_path, QStringLiteral("a.txt")).contains(QStringLiteral("<<<<<<<")));
}

void StagingTest::amendingFoldsTheIndexIntoHead() {
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("base")));
    QVERIFY(commitFile(QStringLiteral("b.txt"), QStringLiteral("b\n"), QStringLiteral("tip")));
    const QString parent = gitOut(m_path, {"rev-parse", "HEAD^"}).trimmed();
    QVERIFY(writeFile(m_path, QStringLiteral("c.txt"), QStringLiteral("c\n")));
    QString error;
    QVERIFY2(m_session->stageFile(QStringLiteral("c.txt"), &error), qPrintable(error));

    QString newId;
    QVERIFY2(m_session->amendHead(QStringLiteral("tip, amended\n\nWith a body"), false, &newId,
                                  &error),
             qPrintable(error));

    // The same parent, a new tip carrying the staged file and the new message.
    QCOMPARE(gitOut(m_path, {"rev-parse", "HEAD"}).trimmed(), newId);
    QCOMPARE(gitOut(m_path, {"rev-parse", "HEAD^"}).trimmed(), parent);
    QCOMPARE(gitOut(m_path, {"ls-tree", "--name-only", "HEAD"}).split(QLatin1Char('\n'),
                                                                     Qt::SkipEmptyParts),
             QStringList({QStringLiteral("a.txt"), QStringLiteral("b.txt"),
                          QStringLiteral("c.txt")}));
    QCOMPARE(gitOut(m_path, {"log", "-1", "--format=%B"}).trimmed(),
             QStringLiteral("tip, amended\n\nWith a body"));
    QCOMPARE(gitOut(m_path, {"rev-list", "--count", "HEAD"}).trimmed(), QStringLiteral("2"));
    QVERIFY(m_session->canUndo());
}

QTEST_MAIN(StagingTest)
#include "test_staging.moc"
