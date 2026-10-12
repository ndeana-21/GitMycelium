#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"

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

QString gitOut(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, dir.toStdString()).out).trimmed();
}

QStringList identity() {
    return {QStringLiteral("-c"), QStringLiteral("user.name=Test"), QStringLiteral("-c"),
            QStringLiteral("user.email=test@localhost")};
}

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

const WorktreeEntry* findWorktree(const std::vector<WorktreeEntry>& list, const QString& path) {
    for (const WorktreeEntry& entry : list) {
        if (QDir::cleanPath(QString::fromStdString(entry.path)) == QDir::cleanPath(path)) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

/// Worktrees, against real repositories: a linked working tree shares refs and
/// objects but has its own HEAD, index and operation markers, which is the part
/// that is easy to get wrong.
class WorktreeTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void addingANewBranchStartsItWhereAsked();
    void addingAnExistingBranchChecksItOut();
    void theCurrentWorktreeIsMarked();
    void removeRefusesADirtyWorktreeWithoutForce();
    void lockAndUnlockRoundTrip();
    void pruneForgetsADeletedWorktree();
    void aConflictInAWorktreeIsSeenByItsOwnSession();
    void aWorktreeSharesTheCommonGitDirWithItsRepository();
    void worktreeWipFollowsEachCheckoutsChanges();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
    QString m_wtPath;
};

void WorktreeTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    m_wtPath = QDir(m_path).filePath(QStringLiteral("wt"));

    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(writeFile(m_path, "a.txt", "line1\nline2\nline3\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "base"}));

    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(writeFile(m_path, "a.txt", "line1\nFEATURE\nline3\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "feature"}));

    // `wt-branch` branches from the base, not from feature, so merging them
    // conflicts — each side changed the same line.
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "wt-branch"}));
    QVERIFY(writeFile(m_path, "a.txt", "line1\nWT\nline3\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "wt"}));

    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
}

void WorktreeTest::cleanup() {
    m_dir.reset();
}

void WorktreeTest::addingANewBranchStartsItWhereAsked() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    QString error;
    // A branch that does not exist yet: created at `feature`, not at HEAD.
    QVERIFY2(session.addWorktree(m_wtPath, QStringLiteral("fresh"), true,
                                 QStringLiteral("feature"), &error),
             qPrintable(error));

    QVERIFY(QDir(m_wtPath).exists());
    const std::vector<WorktreeEntry> list = session.worktrees();
    QCOMPARE(list.size(), std::size_t(2));
    const WorktreeEntry* wt = findWorktree(list, m_wtPath);
    QVERIFY(wt);
    QCOMPARE(QString::fromStdString(wt->branch), QStringLiteral("refs/heads/fresh"));

    // Its own session lands on the new branch at feature's tip.
    RepoSession wtSession;
    QVERIFY(wtSession.open(m_wtPath));
    QCOMPARE(QString::fromStdString(wtSession.info().headBranch), QStringLiteral("fresh"));
    QCOMPARE(gitOut(m_wtPath, {"rev-parse", "HEAD"}),
             gitOut(m_path, {"rev-parse", "feature"}));
}

void WorktreeTest::addingAnExistingBranchChecksItOut() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    QString error;
    QVERIFY2(session.addWorktree(m_wtPath, QStringLiteral("wt-branch"), false, QString(), &error),
             qPrintable(error));
    QCOMPARE(gitOut(m_wtPath, {"rev-parse", "--abbrev-ref", "HEAD"}),
             QStringLiteral("wt-branch"));

    // The refs are shared: a commit made in the worktree is visible from the main
    // checkout without a fetch.
    QVERIFY(writeFile(m_wtPath, "new.txt", "from the worktree\n"));
    QVERIFY(git(m_wtPath, {"add", "-A"}));
    QVERIFY(git(m_wtPath, identity() + QStringList{"commit", "-q", "-m", "from worktree"}));
    QCOMPARE(gitOut(m_path, {"rev-parse", "wt-branch"}),
             gitOut(m_wtPath, {"rev-parse", "HEAD"}));
}

void WorktreeTest::theCurrentWorktreeIsMarked() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY(session.addWorktree(m_wtPath, QStringLiteral("wt-branch"), false, QString(), &error));

    const std::vector<WorktreeEntry> fromMain = session.worktrees();
    const WorktreeEntry* main = findWorktree(fromMain, m_path);
    const WorktreeEntry* wt = findWorktree(fromMain, m_wtPath);
    QVERIFY(main && wt);
    QVERIFY(main->current);
    QVERIFY(!wt->current);

    RepoSession wtSession;
    QVERIFY(wtSession.open(m_wtPath));
    const std::vector<WorktreeEntry> fromWt = wtSession.worktrees();
    QVERIFY(findWorktree(fromWt, m_wtPath)->current);
    QVERIFY(!findWorktree(fromWt, m_path)->current);
}

void WorktreeTest::removeRefusesADirtyWorktreeWithoutForce() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY(session.addWorktree(m_wtPath, QStringLiteral("wt-branch"), false, QString(), &error));

    // An uncommitted change is the safety net, not a nuisance: it stops a removal
    // from taking work with it.
    QVERIFY(writeFile(m_wtPath, "a.txt", "line1\nDIRTY\nline3\n"));
    QVERIFY(!session.removeWorktree(m_wtPath, false, &error));
    QVERIFY(QDir(m_wtPath).exists());

    QVERIFY2(session.removeWorktree(m_wtPath, true, &error), qPrintable(error));
    QVERIFY(!QDir(m_wtPath).exists());
    // The branch survives the worktree.
    QVERIFY(!gitOut(m_path, {"rev-parse", "--verify", "wt-branch"}).isEmpty());
}

void WorktreeTest::lockAndUnlockRoundTrip() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY(session.addWorktree(m_wtPath, QStringLiteral("wt-branch"), false, QString(), &error));

    QVERIFY2(session.lockWorktree(m_wtPath, QStringLiteral("on a slow disk"), &error),
             qPrintable(error));
    // The list is bound to a local: a pointer into the temporary would dangle.
    const std::vector<WorktreeEntry> afterLock = session.worktrees();
    const WorktreeEntry* locked = findWorktree(afterLock, m_wtPath);
    QVERIFY(locked);
    QVERIFY(locked->locked);
    QCOMPARE(QString::fromStdString(locked->lockReason), QStringLiteral("on a slow disk"));

    QVERIFY2(session.unlockWorktree(m_wtPath, &error), qPrintable(error));
    const std::vector<WorktreeEntry> afterUnlock = session.worktrees();
    const WorktreeEntry* unlocked = findWorktree(afterUnlock, m_wtPath);
    QVERIFY(unlocked);
    QVERIFY(!unlocked->locked);
}

void WorktreeTest::pruneForgetsADeletedWorktree() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY(session.addWorktree(m_wtPath, QStringLiteral("wt-branch"), false, QString(), &error));
    QCOMPARE(session.worktrees().size(), std::size_t(2));

    // Deleting the directory by hand leaves the entry behind; prune is how it goes.
    QVERIFY(QDir(m_wtPath).removeRecursively());
    QVERIFY2(session.pruneWorktrees(&error), qPrintable(error));
    QCOMPARE(session.worktrees().size(), std::size_t(1));
}

void WorktreeTest::aConflictInAWorktreeIsSeenByItsOwnSession() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY(session.addWorktree(m_wtPath, QStringLiteral("wt-branch"), false, QString(), &error));

    // A merge in the worktree writes MERGE_HEAD into the worktree's own git dir,
    // not the common one. If we read the common dir, the conflict would be
    // invisible — the same class of bug as libgit2's cached index.
    const int rc = runGit({"merge", "feature"}, m_wtPath.toStdString()).exitCode;
    QVERIFY(rc != 0);

    RepoSession wtSession;
    QVERIFY(wtSession.open(m_wtPath));
    QVERIFY(wtSession.mergeInProgress());
    QVERIFY(!wtSession.conflicts().empty());

    // The main checkout is not mid-merge: the marker belongs to the worktree.
    QVERIFY(!session.mergeInProgress());
    QVERIFY(session.conflicts().empty());
}

void WorktreeTest::aWorktreeSharesTheCommonGitDirWithItsRepository() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    QString error;
    QVERIFY(session.addWorktree(m_wtPath, QStringLiteral("wt-branch"), false, QString(), &error));

    RepoSession wtSession;
    QVERIFY(wtSession.open(m_wtPath));

    // A linked worktree owns its HEAD, index and markers (its own git dir) but
    // shares refs and objects — and so the view settings keyed by the common dir.
    // If these two disagreed, a worktree would inherit nothing.
    QCOMPARE(QString::fromStdString(wtSession.info().commonDir),
             QString::fromStdString(session.info().commonDir));
    QVERIFY(QString::fromStdString(wtSession.info().gitDir) !=
            QString::fromStdString(session.info().gitDir));
    QVERIFY(!session.info().commonDir.empty());
}

void WorktreeTest::worktreeWipFollowsEachCheckoutsChanges() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    // Outside the main working tree on purpose: a worktree directory *inside* the
    // checkout would itself be an untracked entry and the "clean checkout" half of
    // this test could never hold.
    QTemporaryDir wtDir;
    QVERIFY(wtDir.isValid());
    const QString wt = QDir(wtDir.path()).filePath(QStringLiteral("wt"));
    QString error;
    QVERIFY(session.addWorktree(wt, QStringLiteral("wt-branch"), false, QString(), &error));

    // Both checkouts dirty: one node per checkout, untracked files count, and the
    // session's own checkout comes first.
    QVERIFY(writeFile(m_path, "main-dirty.txt", "x\n"));
    QVERIFY(writeFile(wt, "wt-dirty.txt", "y\n"));
    const std::vector<WorktreeWip> wips = session.worktreeWip();
    QCOMPARE(wips.size(), std::size_t(2));
    QVERIFY(wips.front().current);
    QCOMPARE(QDir::cleanPath(QString::fromStdString(wips.front().path)),
             QDir::cleanPath(m_path));
    QCOMPARE(QDir::cleanPath(QString::fromStdString(wips.back().path)), QDir::cleanPath(wt));
    QVERIFY(wips.front().unstaged >= 1);
    QVERIFY(wips.back().unstaged >= 1);

    // A clean checkout drops out; the dirty one stays.
    QVERIFY(QFile::remove(QDir(m_path).filePath(QStringLiteral("main-dirty.txt"))));
    const std::vector<WorktreeWip> onlyWorktree = session.worktreeWip();
    QCOMPARE(onlyWorktree.size(), std::size_t(1));
    QCOMPARE(QDir::cleanPath(QString::fromStdString(onlyWorktree.front().path)),
             QDir::cleanPath(wt));
    QVERIFY(!onlyWorktree.front().current);
}

QTEST_MAIN(WorktreeTest)
#include "test_worktrees.moc"
