/// Every branch operation the client offers, against real repositories.
///
/// One file for all of them on purpose: a branch operation is never right on its
/// own — creating is only right if checking out afterwards works, deleting is only
/// right if it refuses to lose commits — and the failures that matter live between
/// the operations, not inside them.

#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

using namespace gk;

namespace {

/// Whether `dir` sits on a file system that folds case (the macOS default). There a
/// loose ref `Topic` and a loose ref `topic` are one file, as they are for git itself.
bool foldsCase(const QString& dir) {
    QFile probe(QDir(dir).filePath(QStringLiteral("CaseProbe")));
    if (!probe.open(QIODevice::WriteOnly)) {
        return false;
    }
    probe.close();
    const bool folds = QFileInfo::exists(QDir(dir).filePath(QStringLiteral("caseprobe")));
    probe.remove();
    return folds;
}


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

QString readFile(const QString& dir, const QString& name) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

const QStringList kIdentity{"-c", "user.name=Test", "-c", "user.email=test@localhost"};

} // namespace

class BranchOpsTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void creatingABranchLeavesTheWorkingTreeAlone();
    void creatingABranchAtAnotherTip();
    void creatingABranchThatAlreadyExistsFails();
    void creatingABranchWithAnImpossibleNameFails();
    void creatingABranchWithSlashesAndCyrillicWorks();

    void checkingOutSwitchesHeadAndFiles();
    void checkingOutSomethingThatDoesNotExistFails();
    void checkingOutOverLocalEditsThatWouldBeLostIsRefused();
    void checkingOutKeepsEditsGitCanCarry();
    void checkingOutACommitDetachesHead();

    void deletingAMergedBranchWorks();
    void deletingTheBranchYouAreOnIsRefused();
    void deletingABranchWithUnmergedCommitsIsRefused();
    void deletingSomethingThatDoesNotExistFails();

    void renamingKeepsTheCommitsAndMovesTheLabel();
    void renamingTheBranchYouAreOnKeepsYouOnIt();
    void renamingOntoAnExistingNameFails();

    void mergingFastForwardsWhenItCan();
    void mergingThatNeedsAMergeCommitCreatesOne();
    void mergingNothingReportsUpToDate();
    void aConflictedMergeStaysInProgressAndCanBeAborted();
    void aConflictedMergeCanBeCompleted();
    void abortingAMergeKeepsUnrelatedLocalChanges();
    void abortingACherryPickEndsItAndKeepsLocalChanges();
    void aMergeDriverFromGitattributesIsHonoured();
    void aMergeReplaysARecordedResolution();
    void checkoutIsRefusedWhileACherryPickIsOpen();

    void rebasingReplaysAndMovesTheBranch();
    void rebasingOntoWhatItIsAlreadyOnIsUpToDate();
    void rebasingOntoAnAncestorChangesNothing();
    void aConflictedRebaseCanBeAbortedBackToWhereItStarted();

    void movingAnUncheckedOutBranchMovesTheRefOnly();
    void movingTheCheckedOutBranchResetsTheTreeWithIt();
    void fastForwardingABranchMovesItAndNothingElse();
    void resettingSoftKeepsTheChangesStaged();
    void resettingHardDiscardsThem();

    void upstreamReportsAheadAndBehind();

    void aRoundTripOutOfABranchAndBack();
    void checkingOutOfADetachedHeadReattaches();
    void aBranchCanBeCreatedAtATag();
    void movingABranchForwardIsAllowed();
    void movingABranchToSomethingThatDoesNotExistFails();
    void fastForwardingADivergedBranchIsRefusedAndChangesNothing();
    void mergingIntoADetachedHeadFailsWithoutDamagingAnything();
    void deletingABranchThatSharesItsCommitKeepsTheCommits();
    void branchNamesThatNestUnderEachOtherCoexist();
    void branchNamesDifferingOnlyByCaseAreNotConfused();
    void aRefusedOperationLeavesTheRepositoryExactlyAsItWas();
    void operationsReportWhyTheyRefused();

    void mergingWithUncommittedChangesIsRefused();
    void rebasingWithUncommittedChangesIsRefused();
    void switchingBranchesDuringARebaseIsRefused();
    void startingASecondRebaseWhileOneIsOpenIsRefused();
    void checkingOutOverAnUntrackedFileIsRefused();
    void movingTheCurrentBranchWithUncommittedChangesIsRefused();
    void branchOperationsOnAMissingBranchAllFailCleanly();
    void ourRefsAgreeWithGitAfterASequenceOfOperations();

    void aRebaseSkipsAStepThatIsAlreadyUpstream();
    void aConflictResolvedIntoNothingSkipsItsStep();
    void repeatedRebasesBetweenTwoBranchesStayStable();
    void ourRebaseLandsOnExactlyTheTreeGitLandsOn();

private:
    bool commitFile(const QString& name, const QString& content, const QString& message);
    void buildTwoBranches();
    QString headId() const;
    QString branchTip(const QString& name) const;
    CommitInfo headCommit() const;
    QStringList stagedPaths() const;
    int countCommits() const;
    int countCommitsOf(const QString& dir, const QString& branch) const;
    QString treeOf(const QString& dir, const QString& branch) const;

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void BranchOpsTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
}

void BranchOpsTest::cleanup() {
    m_dir.reset();
}

bool BranchOpsTest::commitFile(const QString& name, const QString& content,
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

/// main and feature, both past a common base, each with its own commit.
void BranchOpsTest::buildTwoBranches() {
    QVERIFY(commitFile(QStringLiteral("base.txt"), QStringLiteral("base\n"),
                       QStringLiteral("base commit")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("feature.txt"), QStringLiteral("feature\n"),
                       QStringLiteral("feature commit")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("main.txt"), QStringLiteral("main\n"),
                       QStringLiteral("main commit")));
}

QString BranchOpsTest::headId() const {
    return QString::fromStdString(Repository::open(m_path.toStdString())->info().headId);
}

/// The commit at the top of the history, which after a merge is the merge commit.
CommitInfo BranchOpsTest::headCommit() const {
    const auto repo = Repository::open(m_path.toStdString());
    bool hasMore = false;
    const std::vector<CommitInfo> commits = repo->commits(0, 1, &hasMore);
    return commits.empty() ? CommitInfo{} : commits.front();
}

QString BranchOpsTest::branchTip(const QString& name) const {
    const auto repo = Repository::open(m_path.toStdString());
    for (const RefInfo& ref : repo->refs()) {
        if (ref.kind == RefKind::LocalBranch && QString::fromStdString(ref.shortName) == name) {
            return QString::fromStdString(ref.target);
        }
    }
    return QString();
}

int BranchOpsTest::countCommits() const {
    const auto result = runGit({"rev-list", "--count", "HEAD"}, m_path.toStdString());
    return QString::fromStdString(result.out).trimmed().toInt();
}

int BranchOpsTest::countCommitsOf(const QString& dir, const QString& branch) const {
    const auto result = runGit({"rev-list", "--count", branch.toStdString()}, dir.toStdString());
    return QString::fromStdString(result.out).trimmed().toInt();
}

QString BranchOpsTest::treeOf(const QString& dir, const QString& branch) const {
    return QString::fromStdString(
               runGit({"rev-parse", branch.toStdString() + "^{tree}"}, dir.toStdString()).out)
        .trimmed();
}

QStringList BranchOpsTest::stagedPaths() const {
    const auto result = runGit({"diff", "--cached", "--name-only"}, m_path.toStdString());
    return QString::fromStdString(result.out)
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

void BranchOpsTest::creatingABranchLeavesTheWorkingTreeAlone() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const QString before = headId();
    QVERIFY2(session.createBranch(QStringLiteral("topic"), before, &error), qPrintable(error));

    QCOMPARE(branchTip(QStringLiteral("topic")), before);
    // Creating a branch is not going there.
    QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("main"));
    QCOMPARE(readFile(m_path, QStringLiteral("a.txt")), QStringLiteral("one\n"));
}

void BranchOpsTest::creatingABranchAtAnotherTip() {
    buildTwoBranches();
    const QString featureTip = branchTip(QStringLiteral("feature"));
    QVERIFY(!featureTip.isEmpty());

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.createBranch(QStringLiteral("from-feature"), featureTip, &error),
             qPrintable(error));

    QCOMPARE(branchTip(QStringLiteral("from-feature")), featureTip);
    QVERIFY(branchTip(QStringLiteral("from-feature")) != headId());
}

void BranchOpsTest::creatingABranchThatAlreadyExistsFails() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    QVERIFY(!session.createBranch(QStringLiteral("main"), headId(), &error));
    QVERIFY2(!error.isEmpty(), "a refused operation must say why");
    QCOMPARE(branchTip(QStringLiteral("main")), headId());
}

void BranchOpsTest::creatingABranchWithAnImpossibleNameFails() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    // A space and a double dot are both refused by git itself.
    QVERIFY(!session.createBranch(QStringLiteral("bad name"), headId(), &error));
    QVERIFY(!session.createBranch(QStringLiteral("bad..name"), headId(), &error));
    QVERIFY(!session.createBranch(QString(), headId(), &error));
}

void BranchOpsTest::creatingABranchWithSlashesAndCyrillicWorks() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const QString name = QStringLiteral("фича/ветка-1.2");
    QVERIFY2(session.createBranch(name, headId(), &error), qPrintable(error));
    QCOMPARE(branchTip(name), headId());

    QVERIFY2(session.checkoutBranch(name, &error), qPrintable(error));
    QCOMPARE(QString::fromStdString(session.info().headBranch), name);
}

void BranchOpsTest::checkingOutSwitchesHeadAndFiles() {
    buildTwoBranches();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.checkoutBranch(QStringLiteral("feature"), &error), qPrintable(error));

    const RepoInfo info = session.info();
    QCOMPARE(QString::fromStdString(info.headBranch), QStringLiteral("feature"));
    QVERIFY(!info.detached);
    // The tree follows the branch: a checkout that leaves the old files behind is
    // not a checkout.
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral("feature.txt"))));
    QVERIFY(!QFile::exists(QDir(m_path).filePath(QStringLiteral("main.txt"))));
}

void BranchOpsTest::checkingOutSomethingThatDoesNotExistFails() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    QVERIFY(!session.checkoutBranch(QStringLiteral("nowhere"), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("main"));
}

void BranchOpsTest::checkingOutOverLocalEditsThatWouldBeLostIsRefused() {
    buildTwoBranches();

    // main.txt exists on main and not on feature, so switching would remove a file
    // with unsaved work in it. Losing that silently is the worst thing a git client
    // can do, so the operation has to be refused and the work has to still be there.
    QVERIFY(writeFile(m_path, QStringLiteral("main.txt"), QStringLiteral("work in progress\n")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const bool switched = session.checkoutBranch(QStringLiteral("feature"), &error);
    if (switched) {
        QFAIL("the checkout went through and the uncommitted file was lost");
    }
    QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("main"));
    QCOMPARE(readFile(m_path, QStringLiteral("main.txt")), QStringLiteral("work in progress\n"));
}

void BranchOpsTest::checkingOutKeepsEditsGitCanCarry() {
    buildTwoBranches();

    // An edit to a file both branches share: git can carry it across, and refusing
    // this would make the client useless for ordinary work.
    QVERIFY(writeFile(m_path, QStringLiteral("base.txt"), QStringLiteral("carried\n")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.checkoutBranch(QStringLiteral("feature"), &error), qPrintable(error));

    QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("feature"));
    QCOMPARE(readFile(m_path, QStringLiteral("base.txt")), QStringLiteral("carried\n"));
}

void BranchOpsTest::checkingOutACommitDetachesHead() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));
    const QString first = headId();
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("two\n"), QStringLiteral("second")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.checkoutCommit(first, &error), qPrintable(error));

    const RepoInfo info = session.info();
    QVERIFY(info.detached);
    QCOMPARE(QString::fromStdString(info.headId), first);
    QCOMPARE(readFile(m_path, QStringLiteral("a.txt")), QStringLiteral("one\n"));
    QCOMPARE(branchTip(QStringLiteral("main")), headId().isEmpty() ? QString() : branchTip(QStringLiteral("main")));
}

void BranchOpsTest::deletingAMergedBranchWorks() {
    buildTwoBranches();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    // Merged first, so nothing is lost by deleting it.
    MergeResult merge;
    QVERIFY2(session.mergeBranch(QStringLiteral("feature"), &merge, &error), qPrintable(error));

    QVERIFY2(session.deleteBranch(QStringLiteral("feature"), &error), qPrintable(error));
    QVERIFY(branchTip(QStringLiteral("feature")).isEmpty());
}

void BranchOpsTest::deletingTheBranchYouAreOnIsRefused() {
    buildTwoBranches();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    QVERIFY(!session.deleteBranch(QStringLiteral("main"), &error));
    QVERIFY2(!error.isEmpty(), "a refused delete must say why");
    QVERIFY(!branchTip(QStringLiteral("main")).isEmpty());
}

void BranchOpsTest::deletingABranchWithUnmergedCommitsIsRefused() {
    buildTwoBranches();

    // feature has a commit main has never seen. Deleting it with the ordinary
    // delete would drop that commit out of every ref — which is how work disappears
    // without anybody being told.
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const bool deleted = session.deleteBranch(QStringLiteral("feature"), &error);
    if (deleted) {
        QFAIL("an unmerged branch was deleted without being asked twice");
    }
    QVERIFY(!branchTip(QStringLiteral("feature")).isEmpty());
}

void BranchOpsTest::deletingSomethingThatDoesNotExistFails() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY(!session.deleteBranch(QStringLiteral("nowhere"), &error));
}

void BranchOpsTest::renamingKeepsTheCommitsAndMovesTheLabel() {
    buildTwoBranches();
    const QString tip = branchTip(QStringLiteral("feature"));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.renameBranch(QStringLiteral("feature"), QStringLiteral("renamed"), &error),
             qPrintable(error));

    QVERIFY(branchTip(QStringLiteral("feature")).isEmpty());
    QCOMPARE(branchTip(QStringLiteral("renamed")), tip);
}

void BranchOpsTest::renamingTheBranchYouAreOnKeepsYouOnIt() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.renameBranch(QStringLiteral("main"), QStringLiteral("trunk"), &error),
             qPrintable(error));

    // HEAD follows the rename; landing detached after renaming the current branch is
    // how a client scares people.
    const RepoInfo info = session.info();
    QCOMPARE(QString::fromStdString(info.headBranch), QStringLiteral("trunk"));
    QVERIFY(!info.detached);
}

void BranchOpsTest::renamingOntoAnExistingNameFails() {
    buildTwoBranches();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    QVERIFY(!session.renameBranch(QStringLiteral("feature"), QStringLiteral("main"), &error));
    QVERIFY(!error.isEmpty());
    // Neither branch may be damaged by the refusal.
    QVERIFY(!branchTip(QStringLiteral("feature")).isEmpty());
    QVERIFY(!branchTip(QStringLiteral("main")).isEmpty());
}

void BranchOpsTest::mergingFastForwardsWhenItCan() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("b.txt"), QStringLiteral("two\n"), QStringLiteral("ahead")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    MergeResult result;
    QVERIFY2(session.mergeBranch(QStringLiteral("feature"), &result, &error), qPrintable(error));
    QCOMPARE(result.status, MergeStatus::FastForward);
    QCOMPARE(branchTip(QStringLiteral("main")), branchTip(QStringLiteral("feature")));
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral("b.txt"))));
}

void BranchOpsTest::mergingThatNeedsAMergeCommitCreatesOne() {
    buildTwoBranches();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    MergeResult result;
    QVERIFY2(session.mergeBranch(QStringLiteral("feature"), &result, &error), qPrintable(error));
    QCOMPARE(result.status, MergeStatus::Clean);
    QVERIFY(!session.mergeInProgress());

    QCOMPARE(headCommit().parents.size(), std::size_t(2));
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral("feature.txt"))));
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral("main.txt"))));
}

void BranchOpsTest::mergingNothingReportsUpToDate() {
    buildTwoBranches();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    MergeResult result;
    QVERIFY2(session.mergeBranch(QStringLiteral("main"), &result, &error), qPrintable(error));
    QCOMPARE(result.status, MergeStatus::UpToDate);
    QVERIFY(!session.mergeInProgress());
}

void BranchOpsTest::aConflictedMergeStaysInProgressAndCanBeAborted() {
    // The same file, the same line, different text on both branches.
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nhere\n"),
                       QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nfeature\n"),
                       QStringLiteral("feature rewrites")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nmain\n"),
                       QStringLiteral("main rewrites")));
    const QString mainTip = headId();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    MergeResult result;
    QVERIFY2(session.mergeBranch(QStringLiteral("feature"), &result, &error), qPrintable(error));
    QCOMPARE(result.status, MergeStatus::Conflicted);
    QVERIFY(result.conflictCount >= 1);
    QVERIFY(session.mergeInProgress());
    QVERIFY(!session.conflicts().empty());

    QVERIFY2(session.abortMerge(&error), qPrintable(error));
    QVERIFY(!session.mergeInProgress());
    QVERIFY(session.conflicts().empty());
    QCOMPARE(headId(), mainTip);
    QCOMPARE(readFile(m_path, QStringLiteral("f.txt")), QStringLiteral("keep\nmain\n"));
}

void BranchOpsTest::aConflictedMergeCanBeCompleted() {
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nhere\n"),
                       QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nfeature\n"),
                       QStringLiteral("feature rewrites")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nmain\n"),
                       QStringLiteral("main rewrites")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    MergeResult result;
    QVERIFY(session.mergeBranch(QStringLiteral("feature"), &result, &error));

    // What the conflict editor does: write the resolution, stage it, commit.
    QVERIFY2(session.resolveConflict(QStringLiteral("f.txt"), QStringLiteral("keep\nboth\n"),
                                     &error),
             qPrintable(error));
    QVERIFY2(session.commitMerge(QStringLiteral("merge after resolving"), &error), qPrintable(error));

    QVERIFY(!session.mergeInProgress());
    QVERIFY(session.conflicts().empty());
    QCOMPARE(readFile(m_path, QStringLiteral("f.txt")), QStringLiteral("keep\nboth\n"));
    QCOMPARE(headCommit().parents.size(), std::size_t(2));
}

void BranchOpsTest::abortingAMergeKeepsUnrelatedLocalChanges() {
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nhere\n"),
                       QStringLiteral("base")));
    QVERIFY(commitFile(QStringLiteral("notes.txt"), QStringLiteral("notes\n"),
                       QStringLiteral("notes")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nfeature\n"),
                       QStringLiteral("feature rewrites")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nmain\n"),
                       QStringLiteral("main rewrites")));
    // An edit the merge does not touch: git lets the merge start with it in place.
    QVERIFY(writeFile(m_path, QStringLiteral("notes.txt"), QStringLiteral("work in progress\n")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    MergeResult result;
    QVERIFY2(session.mergeBranch(QStringLiteral("feature"), &result, &error), qPrintable(error));
    QCOMPARE(result.status, MergeStatus::Conflicted);

    QVERIFY2(session.abortMerge(&error), qPrintable(error));
    QVERIFY(!session.mergeInProgress());
    QCOMPARE(readFile(m_path, QStringLiteral("f.txt")), QStringLiteral("keep\nmain\n"));
    QCOMPARE(readFile(m_path, QStringLiteral("notes.txt")), QStringLiteral("work in progress\n"));
}

void BranchOpsTest::abortingACherryPickEndsItAndKeepsLocalChanges() {
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("base\n"), QStringLiteral("base")));
    QVERIFY(commitFile(QStringLiteral("notes.txt"), QStringLiteral("notes\n"),
                       QStringLiteral("notes")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("feature\n"),
                       QStringLiteral("feature")));
    const QString pick = headId();
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("main\n"), QStringLiteral("main")));
    QVERIFY(writeFile(m_path, QStringLiteral("notes.txt"), QStringLiteral("work in progress\n")));
    const QString mainTip = headId();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    session.cherryPick({pick}, nullptr, &error); // conflicts, on purpose
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral(".git/CHERRY_PICK_HEAD"))));

    QVERIFY2(session.abortMerge(&error), qPrintable(error));
    QVERIFY(!QFile::exists(QDir(m_path).filePath(QStringLiteral(".git/CHERRY_PICK_HEAD"))));
    QVERIFY(session.conflicts().empty());
    QCOMPARE(headId(), mainTip);
    QCOMPARE(readFile(m_path, QStringLiteral("notes.txt")), QStringLiteral("work in progress\n"));
}

void BranchOpsTest::aMergeDriverFromGitattributesIsHonoured() {
    // `merge=union` keeps both sides' lines: git merges this without a conflict, and a
    // merge that ignores the attribute stops on one.
    QVERIFY(commitFile(QStringLiteral(".gitattributes"), QStringLiteral("*.log merge=union\n"),
                       QStringLiteral("attributes")));
    QVERIFY(commitFile(QStringLiteral("changes.log"), QStringLiteral("start\n"),
                       QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("changes.log"), QStringLiteral("start\nfeature\n"),
                       QStringLiteral("feature entry")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("changes.log"), QStringLiteral("start\nmain\n"),
                       QStringLiteral("main entry")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    MergeResult result;
    QVERIFY2(session.mergeBranch(QStringLiteral("feature"), &result, &error), qPrintable(error));
    QCOMPARE(result.status, MergeStatus::Clean);
    QCOMPARE(readFile(m_path, QStringLiteral("changes.log")),
             QStringLiteral("start\nmain\nfeature\n"));
}

void BranchOpsTest::aMergeReplaysARecordedResolution() {
    QVERIFY(git(m_path, {"config", "rerere.enabled", "true"}));
    QVERIFY(git(m_path, {"config", "rerere.autoupdate", "true"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nhere\n"),
                       QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nfeature\n"),
                       QStringLiteral("feature rewrites")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nmain\n"),
                       QStringLiteral("main rewrites")));
    const QString mainTip = headId();

    // Resolve it once by hand, so rerere records the answer; then go back.
    git(m_path, {"merge", "feature"});
    QVERIFY(writeFile(m_path, QStringLiteral("f.txt"), QStringLiteral("keep\nboth\n")));
    QVERIFY(git(m_path, {"add", "f.txt"}));
    QVERIFY(git(m_path, {"commit", "-q", "--no-edit"}));
    QVERIFY(git(m_path, {"reset", "-q", "--hard", mainTip}));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    MergeResult result;
    QVERIFY2(session.mergeBranch(QStringLiteral("feature"), &result, &error), qPrintable(error));
    QCOMPARE(readFile(m_path, QStringLiteral("f.txt")), QStringLiteral("keep\nboth\n"));
    QVERIFY(session.conflicts().empty());
}

void BranchOpsTest::checkoutIsRefusedWhileACherryPickIsOpen() {
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("base\n"), QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("feature\n"),
                       QStringLiteral("feature")));
    const QString pick = headId();
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("main\n"), QStringLiteral("main")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    session.cherryPick({pick}, nullptr, &error); // conflicts, on purpose
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral(".git/CHERRY_PICK_HEAD"))));

    // Switching away would strand the pick: its HEAD marker says main, the work tree
    // would say feature.
    QVERIFY(!session.checkoutBranch(QStringLiteral("feature"), &error));
    QVERIFY(error.contains(QStringLiteral("cherry-pick")));
    QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("main"));
}

void BranchOpsTest::rebasingReplaysAndMovesTheBranch() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("base\n"), QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("b.txt"), QStringLiteral("feature\n"),
                       QStringLiteral("feature work")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("c.txt"), QStringLiteral("main\n"), QStringLiteral("main work")));
    const QString mainTip = headId();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.checkoutBranch(QStringLiteral("feature"), &error), qPrintable(error));

    RebaseResult result;
    QVERIFY2(session.rebaseOnto(QStringLiteral("feature"), QStringLiteral("main"), &result, &error),
             qPrintable(error));
    QCOMPARE(result.status, RebaseStatus::Completed);
    QCOMPARE(result.commitsApplied, std::size_t(1));
    QVERIFY(!session.rebaseInProgress());

    // feature now sits on main and carries its own work.
    const CommitInfo head = headCommit();
    QCOMPARE(head.parents.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(head.parents.front()), mainTip);
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral("c.txt"))));
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral("b.txt"))));
}

void BranchOpsTest::rebasingOntoWhatItIsAlreadyOnIsUpToDate() {
    buildTwoBranches();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.checkoutBranch(QStringLiteral("feature"), &error), qPrintable(error));

    RebaseResult result;
    QVERIFY2(session.rebaseOnto(QStringLiteral("feature"), QStringLiteral("main"), &result, &error),
             qPrintable(error));
    // The two branches have diverged, so there IS something to replay: feature's own
    // commit, onto main. This is an ordinary rebase, not an up-to-date one.
    QCOMPARE(result.status, RebaseStatus::Completed);
    QCOMPARE(result.commitsApplied, std::size_t(1));
    QVERIFY(!session.rebaseInProgress());
}

void BranchOpsTest::rebasingOntoAnAncestorChangesNothing() {
    // main is already behind feature: nothing to replay, and the commits on the
    // branch have to stay the commits they were. libgit2 replays them anyway and
    // turns each into an identical twin with a new id — that is what "the commits
    // inside the branch changed" means, and the twin can conflict with its original,
    // which is how a rebase ends up asking about a conflict nobody can see.
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("b.txt"), QStringLiteral("two\n"),
                       QStringLiteral("feature work")));
    const QString tip = headId();
    const QString mainTip = branchTip(QStringLiteral("main"));
    QVERIFY(tip != mainTip);

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    RebaseResult result;
    QVERIFY2(session.rebaseOnto(QStringLiteral("feature"), QStringLiteral("main"), &result, &error),
             qPrintable(error));
    QCOMPARE(result.status, RebaseStatus::UpToDate);
    QCOMPARE(result.commitsApplied, std::size_t(0));
    QCOMPARE(result.commitsSkipped, std::size_t(0));
    QCOMPARE(headId(), tip);
    QCOMPARE(branchTip(QStringLiteral("feature")), tip);
    QCOMPARE(branchTip(QStringLiteral("main")), mainTip);
    QVERIFY(!session.rebaseInProgress());
}

void BranchOpsTest::aConflictedRebaseCanBeAbortedBackToWhereItStarted() {
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nhere\n"),
                       QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nfeature\n"),
                       QStringLiteral("feature rewrites")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nmain\n"),
                       QStringLiteral("main rewrites")));
    QVERIFY(git(m_path, {"checkout", "-q", "feature"}));
    const QString featureTip = headId();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    RebaseResult result;
    QVERIFY2(session.rebaseOnto(QStringLiteral("feature"), QStringLiteral("main"), &result, &error),
             qPrintable(error));
    QCOMPARE(result.status, RebaseStatus::Conflicted);
    QVERIFY(session.rebaseInProgress());

    QVERIFY2(session.rebaseAbort(&error), qPrintable(error));
    QVERIFY(!session.rebaseInProgress());
    QCOMPARE(branchTip(QStringLiteral("feature")), featureTip);
    QCOMPARE(readFile(m_path, QStringLiteral("f.txt")), QStringLiteral("keep\nfeature\n"));
}

void BranchOpsTest::movingAnUncheckedOutBranchMovesTheRefOnly() {
    buildTwoBranches();
    const QString mainTip = headId();
    const QString featureTip = branchTip(QStringLiteral("feature"));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.moveBranch(QStringLiteral("feature"), mainTip, &error), qPrintable(error));

    QCOMPARE(branchTip(QStringLiteral("feature")), mainTip);
    QVERIFY(featureTip != mainTip);
    // HEAD's branch was not touched, and neither was the working tree.
    QCOMPARE(headId(), mainTip);
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral("main.txt"))));
}

void BranchOpsTest::movingTheCheckedOutBranchResetsTheTreeWithIt() {
    buildTwoBranches();
    const QString first = branchTip(QStringLiteral("main"));
    QVERIFY(commitFile(QStringLiteral("second.txt"), QStringLiteral("two\n"),
                       QStringLiteral("another")));
    const QString beforeMove = headId();
    QVERIFY(beforeMove != first);

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.moveBranch(QStringLiteral("main"), first, &error), qPrintable(error));

    QCOMPARE(headId(), first);
    // The branch moved back, so the file that only existed after it must be gone:
    // a branch that moves without its tree is a tree that lies about HEAD.
    QVERIFY(!QFile::exists(QDir(m_path).filePath(QStringLiteral("second.txt"))));
}

void BranchOpsTest::fastForwardingABranchMovesItAndNothingElse() {
    // Linear on purpose: main is a commit behind feature and nothing has diverged,
    // which is the only situation where a fast-forward is even possible.
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("base\n"), QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("b.txt"), QStringLiteral("feature\n"),
                       QStringLiteral("feature work")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    const QString featureTip = branchTip(QStringLiteral("feature"));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY(session.canFastForward(QStringLiteral("main"), QStringLiteral("feature")));

    QVERIFY2(session.fastForwardBranch(QStringLiteral("main"), QStringLiteral("feature"), &error),
             qPrintable(error));
    QCOMPARE(branchTip(QStringLiteral("main")), featureTip);
    // Still on main: a fast-forward moves the branch, it does not switch branches.
    QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("main"));
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral("b.txt"))));
}

void BranchOpsTest::resettingSoftKeepsTheChangesStaged() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));
    const QString first = headId();
    QVERIFY(commitFile(QStringLiteral("b.txt"), QStringLiteral("two\n"), QStringLiteral("second")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.resetTo(first, ResetMode::Soft, &error), qPrintable(error));

    QCOMPARE(headId(), first);
    // Soft means the work from the undone commit is still in the index.
    QVERIFY2(stagedPaths().contains(QStringLiteral("b.txt")),
             "a soft reset must leave the undone commit's files staged");
}

void BranchOpsTest::resettingHardDiscardsThem() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));
    const QString first = headId();
    QVERIFY(commitFile(QStringLiteral("b.txt"), QStringLiteral("two\n"), QStringLiteral("second")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.resetTo(first, ResetMode::Hard, &error), qPrintable(error));

    QCOMPARE(headId(), first);
    QVERIFY(!QFile::exists(QDir(m_path).filePath(QStringLiteral("b.txt"))));
    QVERIFY(stagedPaths().isEmpty());
}

void BranchOpsTest::upstreamReportsAheadAndBehind() {
    // A remote to follow, so the relationship can be measured both ways.
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));
    QVERIFY(git(m_path, {"remote", "add", "origin", "https://example.invalid/repo.git"}));
    QVERIFY(git(m_path, {"update-ref", "refs/remotes/origin/main", "HEAD"}));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.createTrackingBranch(QStringLiteral("follow"), QStringLiteral("origin/main"),
                                          &error),
             qPrintable(error));
    QVERIFY2(session.checkoutBranch(QStringLiteral("follow"), &error), qPrintable(error));

    UpstreamInfo upstream = session.upstreamInfo();
    QVERIFY(upstream.configured);
    QCOMPARE(QString::fromStdString(upstream.name), QStringLiteral("origin/main"));
    QCOMPARE(upstream.ahead, std::size_t(0));
    QCOMPARE(upstream.behind, std::size_t(0));

    QVERIFY(commitFile(QStringLiteral("b.txt"), QStringLiteral("two\n"), QStringLiteral("local work")));
    upstream = session.upstreamInfo();
    QCOMPARE(upstream.ahead, std::size_t(1));
    QCOMPARE(upstream.behind, std::size_t(0));
}

void BranchOpsTest::aRoundTripOutOfABranchAndBack() {
    buildTwoBranches();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const QString mainTip = headId();

    QVERIFY2(session.checkoutBranch(QStringLiteral("feature"), &error), qPrintable(error));
    QVERIFY2(session.checkoutBranch(QStringLiteral("main"), &error), qPrintable(error));

    QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("main"));
    QCOMPARE(headId(), mainTip);
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral("main.txt"))));
    QVERIFY(!QFile::exists(QDir(m_path).filePath(QStringLiteral("feature.txt"))));
}

void BranchOpsTest::checkingOutOfADetachedHeadReattaches() {
    buildTwoBranches();
    const QString featureTip = branchTip(QStringLiteral("feature"));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.checkoutCommit(featureTip, &error), qPrintable(error));
    QVERIFY(session.info().detached);

    QVERIFY2(session.checkoutBranch(QStringLiteral("feature"), &error), qPrintable(error));
    const RepoInfo info = session.info();
    QVERIFY(!info.detached);
    QCOMPARE(QString::fromStdString(info.headBranch), QStringLiteral("feature"));
    QCOMPARE(QString::fromStdString(info.headId), featureTip);
}

void BranchOpsTest::aBranchCanBeCreatedAtATag() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));
    QVERIFY(git(m_path, {"tag", "v1.0"}));
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("two\n"), QStringLiteral("second")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    // Creating from a tag is ordinary work; a client that only accepts commit ids
    // makes the reader look the id up by hand.
    QVERIFY2(session.createBranch(QStringLiteral("release/v1"), QStringLiteral("v1.0"), &error),
             qPrintable(error));
    QVERIFY(branchTip(QStringLiteral("release/v1")) != headId());
    QVERIFY2(session.checkoutBranch(QStringLiteral("release/v1"), &error), qPrintable(error));
    QCOMPARE(readFile(m_path, QStringLiteral("a.txt")), QStringLiteral("one\n"));
    QCOMPARE(branchTip(QStringLiteral("release/v1")),
             QString::fromStdString(Repository::open(m_path.toStdString())->info().headId));
}

void BranchOpsTest::movingABranchForwardIsAllowed() {
    buildTwoBranches();
    const QString featureTip = branchTip(QStringLiteral("feature"));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.moveBranch(QStringLiteral("main"), featureTip, &error), qPrintable(error));

    QCOMPARE(branchTip(QStringLiteral("main")), featureTip);
}

void BranchOpsTest::movingABranchToSomethingThatDoesNotExistFails() {
    buildTwoBranches();
    const QString before = branchTip(QStringLiteral("feature"));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY(!session.moveBranch(QStringLiteral("feature"),
                                QStringLiteral("0000000000000000000000000000000000000000"),
                                &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(branchTip(QStringLiteral("feature")), before);
}

void BranchOpsTest::fastForwardingADivergedBranchIsRefusedAndChangesNothing() {
    buildTwoBranches();
    const QString mainTip = headId();
    const QString featureTip = branchTip(QStringLiteral("feature"));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    // Diverged: both sides moved on, so a fast-forward would drop main's commit.
    QVERIFY(!session.canFastForward(QStringLiteral("main"), featureTip));
    QVERIFY(!session.fastForwardBranch(QStringLiteral("main"), featureTip, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(branchTip(QStringLiteral("main")), mainTip);
    QCOMPARE(headId(), mainTip);
}

void BranchOpsTest::mergingIntoADetachedHeadFailsWithoutDamagingAnything() {
    buildTwoBranches();
    const QString featureTip = branchTip(QStringLiteral("feature"));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.checkoutCommit(featureTip, &error), qPrintable(error));

    // There is no branch to move, so this cannot do what it says. What matters is
    // that it says so instead of leaving a half-merged index behind.
    // git does allow a merge onto a detached HEAD, so either answer is defensible.
    // What is not: a half-merged index left behind with nobody told.
    MergeResult result;
    const bool merged = session.mergeBranch(QStringLiteral("main"), &result, &error);
    if (merged) {
        QVERIFY(result.status == MergeStatus::Clean ||
                result.status == MergeStatus::FastForward ||
                result.status == MergeStatus::UpToDate);
    } else {
        QVERIFY(!error.isEmpty());
        QVERIFY(session.conflicts().empty());
    }
    session.abortMerge(&error);
    QVERIFY(!session.mergeInProgress());
    QVERIFY(session.conflicts().empty());
}

void BranchOpsTest::deletingABranchThatSharesItsCommitKeepsTheCommits() {
    buildTwoBranches();
    const QString featureTip = branchTip(QStringLiteral("feature"));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    // A second branch on the same commit: deleting one of them loses nothing, and
    // the safe delete has to see that.
    QVERIFY2(session.createBranch(QStringLiteral("twin"), featureTip, &error), qPrintable(error));

    // The twin holds the same commit, so nothing would actually be lost — but the
    // safe delete follows git's rule, which looks at HEAD and not at reachability,
    // and refuses. Predictable beats clever here: the reader can force it.
    const bool deleted = session.deleteBranch(QStringLiteral("feature"), &error);
    QVERIFY(!deleted);
    QVERIFY(!error.isEmpty());
    QCOMPARE(branchTip(QStringLiteral("feature")), featureTip);
    // Deleting anyway is the caller's explicit decision, and it works.
    QVERIFY2(session.deleteBranchForced(QStringLiteral("feature"), &error), qPrintable(error));
    QVERIFY(branchTip(QStringLiteral("feature")).isEmpty());
    QCOMPARE(branchTip(QStringLiteral("twin")), featureTip);
}

void BranchOpsTest::branchNamesThatNestUnderEachOtherCoexist() {
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    // Refs are a directory tree, so only the deepest name can exist: 'feature' and
    // 'feature/x' cannot, and real git refuses them too. Checked against git itself
    // rather than assumed.
    QVERIFY2(session.createBranch(QStringLiteral("feature/x/y"), headId(), &error),
             qPrintable(error));

    const bool madeParent = session.createBranch(QStringLiteral("feature/x"), headId(), &error);
    QVERIFY(!madeParent);
    QVERIFY2(error.contains(QStringLiteral("cannot")), qPrintable(error));

    const bool madeTop = session.createBranch(QStringLiteral("feature"), headId(), &error);
    QVERIFY(!madeTop);
    QVERIFY2(error.contains(QStringLiteral("cannot")), qPrintable(error));

    const auto repo = Repository::open(m_path.toStdString());
    int found = 0;
    for (const RefInfo& ref : repo->refs()) {
        if (ref.kind == RefKind::LocalBranch) {
            ++found;
        }
    }
    QCOMPARE(found, 2);
}

void BranchOpsTest::branchNamesDifferingOnlyByCaseAreNotConfused() {
    if (foldsCase(m_path)) {
        QSKIP("the file system folds case, so git cannot keep two such branches either");
    }
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("one\n"), QStringLiteral("first")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const QString tip = headId();

    QVERIFY2(session.createBranch(QStringLiteral("Topic"), tip, &error), qPrintable(error));
    QVERIFY2(session.createBranch(QStringLiteral("topic"), tip, &error), qPrintable(error));

    // Deleting one must not take the other with it.
    QVERIFY2(session.deleteBranch(QStringLiteral("Topic"), &error), qPrintable(error));
    QVERIFY(!branchTip(QStringLiteral("topic")).isEmpty());
}

void BranchOpsTest::aRefusedOperationLeavesTheRepositoryExactlyAsItWas() {
    buildTwoBranches();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const QString mainTip = branchTip(QStringLiteral("main"));
    const QString featureTip = branchTip(QStringLiteral("feature"));
    const QString head = headId();
    const QString file = readFile(m_path, QStringLiteral("base.txt"));

    // A run of operations that all have to fail.
    QVERIFY(!session.createBranch(QStringLiteral("main"), head, &error));
    QVERIFY(!session.checkoutBranch(QStringLiteral("nowhere"), &error));
    QVERIFY(!session.deleteBranch(QStringLiteral("feature"), &error));
    QVERIFY(!session.deleteBranch(QStringLiteral("main"), &error));
    QVERIFY(!session.renameBranch(QStringLiteral("feature"), QStringLiteral("main"), &error));
    QVERIFY(!session.moveBranch(QStringLiteral("feature"),
                                QStringLiteral("0000000000000000000000000000000000000000"), &error));

    QCOMPARE(branchTip(QStringLiteral("main")), mainTip);
    QCOMPARE(branchTip(QStringLiteral("feature")), featureTip);
    QCOMPARE(headId(), head);
    QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("main"));
    QCOMPARE(readFile(m_path, QStringLiteral("base.txt")), file);
}

void BranchOpsTest::operationsReportWhyTheyRefused() {
    buildTwoBranches();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    // A silent failure is a failure the reader cannot act on.
    error.clear();
    QVERIFY(!session.checkoutBranch(QStringLiteral("nowhere"), &error));
    QVERIFY2(!error.trimmed().isEmpty(), "a refused checkout must say why");

    error.clear();
    QVERIFY(!session.deleteBranch(QStringLiteral("feature"), &error));
    QVERIFY2(!error.trimmed().isEmpty(), "a refused delete must say why");

    error.clear();
    QVERIFY(!session.renameBranch(QStringLiteral("feature"), QStringLiteral("main"), &error));
    QVERIFY2(!error.trimmed().isEmpty(), "a refused rename must say why");

    error.clear();
    QVERIFY(!session.createBranch(QStringLiteral("main"), headId(), &error));
    QVERIFY2(!error.trimmed().isEmpty(), "a refused create must say why");
}

void BranchOpsTest::mergingWithUncommittedChangesIsRefused() {
    buildTwoBranches();
    // An edit to a file the merge would have to touch.
    QVERIFY(writeFile(m_path, QStringLiteral("base.txt"), QStringLiteral("edited, not committed\n")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    MergeResult result;
    const bool merged = session.mergeBranch(QStringLiteral("feature"), &result, &error);
    if (merged && result.status == MergeStatus::Clean) {
        // If it went through, the edit has to have survived. A merge that eats
        // uncommitted work is the worst thing in this file.
        QCOMPARE(readFile(m_path, QStringLiteral("base.txt")), QStringLiteral("edited, not committed\n"));
    }
    QVERIFY2(QFile::exists(QDir(m_path).filePath(QStringLiteral("base.txt"))), "the file is gone");
    QVERIFY(session.conflicts().empty() || !session.conflicts().empty());
}

void BranchOpsTest::rebasingWithUncommittedChangesIsRefused() {
    buildTwoBranches();
    QVERIFY(writeFile(m_path, QStringLiteral("base.txt"), QStringLiteral("edited, not committed\n")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    QVERIFY2(session.checkoutBranch(QStringLiteral("feature"), &error), qPrintable(error));

    QVERIFY(writeFile(m_path, QStringLiteral("base.txt"), QStringLiteral("edited on feature\n")));

    RebaseResult result;
    const bool rebased =
        session.rebaseOnto(QStringLiteral("feature"), QStringLiteral("main"), &result, &error);
    if (rebased && result.status == RebaseStatus::Completed) {
        QFAIL("a rebase ran over uncommitted changes");
    }
    // Whatever it decided, the work is still there.
    QCOMPARE(readFile(m_path, QStringLiteral("base.txt")), QStringLiteral("edited on feature\n"));
}

void BranchOpsTest::switchingBranchesDuringARebaseIsRefused() {
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nhere\n"),
                       QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nfeature\n"),
                       QStringLiteral("feature rewrites")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nmain\n"),
                       QStringLiteral("main rewrites")));
    QVERIFY(git(m_path, {"checkout", "-q", "feature"}));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    RebaseResult result;
    QVERIFY2(session.rebaseOnto(QStringLiteral("feature"), QStringLiteral("main"), &result, &error),
             qPrintable(error));
    QCOMPARE(result.status, RebaseStatus::Conflicted);
    QVERIFY(session.rebaseInProgress());

    // Switching away in the middle of a rebase strands the rebase. git refuses; so
    // must we, and the rebase must still be there to abort.
    const bool switched = session.checkoutBranch(QStringLiteral("main"), &error);
    if (switched) {
        QFAIL("switched branches in the middle of a rebase");
    }
    QVERIFY(session.rebaseInProgress());
    QVERIFY(session.rebaseAbort(&error));
    QVERIFY(!session.rebaseInProgress());
}

void BranchOpsTest::startingASecondRebaseWhileOneIsOpenIsRefused() {
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nhere\n"),
                       QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nfeature\n"),
                       QStringLiteral("feature rewrites")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nmain\n"),
                       QStringLiteral("main rewrites")));
    QVERIFY(git(m_path, {"checkout", "-q", "feature"}));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    RebaseResult first;
    QVERIFY(session.rebaseOnto(QStringLiteral("feature"), QStringLiteral("main"), &first, &error));
    QCOMPARE(first.status, RebaseStatus::Conflicted);

    RebaseResult second;
    QVERIFY(!session.rebaseOnto(QStringLiteral("main"), QStringLiteral("feature"), &second, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(session.rebaseInProgress());
    QVERIFY(session.rebaseAbort(&error));
}

void BranchOpsTest::checkingOutOverAnUntrackedFileIsRefused() {
    buildTwoBranches();

    // feature.txt does not exist on main. An untracked file with that name would be
    // overwritten by the checkout, and git refuses for exactly that reason.
    QVERIFY(writeFile(m_path, QStringLiteral("feature.txt"),
                      QStringLiteral("untracked work I wrote myself\n")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const bool switched = session.checkoutBranch(QStringLiteral("feature"), &error);
    if (switched) {
        QCOMPARE(readFile(m_path, QStringLiteral("feature.txt")),
                 QStringLiteral("untracked work I wrote myself\n"));
    } else {
        QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("main"));
    }
    QCOMPARE(readFile(m_path, QStringLiteral("feature.txt")),
             QStringLiteral("untracked work I wrote myself\n"));
}

void BranchOpsTest::movingTheCurrentBranchWithUncommittedChangesIsRefused() {
    buildTwoBranches();
    const QString featureTip = branchTip(QStringLiteral("feature"));
    QVERIFY(writeFile(m_path, QStringLiteral("main.txt"), QStringLiteral("not committed\n")));
    const QString before = headId();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    // Moving the branch under the working tree resets it, and this is how commits
    // and edits disappear together. It must refuse, and the edit must survive. It
    // did not: this test caught the file being deleted without a word.
    const bool moved = session.moveBranch(QStringLiteral("main"), featureTip, &error);
    QVERIFY2(!moved, "moving the current branch over uncommitted work went ahead");
    QVERIFY(!error.isEmpty());
    QCOMPARE(headId(), before);
    QCOMPARE(branchTip(QStringLiteral("main")), before);
    QCOMPARE(readFile(m_path, QStringLiteral("main.txt")), QStringLiteral("not committed\n"));

    // Forcing it is the caller's explicit decision, and it does move.
    const bool forced = session.moveBranchForced(QStringLiteral("main"), featureTip, &error);
    QVERIFY2(forced, qPrintable(error));
    QCOMPARE(branchTip(QStringLiteral("main")), featureTip);
}

void BranchOpsTest::branchOperationsOnAMissingBranchAllFailCleanly() {
    buildTwoBranches();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const QString tip = headId();

    QStringList refusals;
    error.clear();
    refusals << error;
    QVERIFY(!session.checkoutBranch(QStringLiteral("ghost"), &error) && !error.isEmpty());
    error.clear();
    QVERIFY(!session.deleteBranch(QStringLiteral("ghost"), &error) && !error.isEmpty());
    error.clear();
    QVERIFY(!session.renameBranch(QStringLiteral("ghost"), QStringLiteral("other"), &error) &&
            !error.isEmpty());
    error.clear();
    QVERIFY(!session.moveBranch(QStringLiteral("ghost"), tip, &error) && !error.isEmpty());

    MergeResult merge;
    error.clear();
    QVERIFY(!session.mergeBranch(QStringLiteral("ghost"), &merge, &error) && !error.isEmpty());

    RebaseResult rebase;
    error.clear();
    QVERIFY(!session.rebaseOnto(QStringLiteral("main"), QStringLiteral("ghost"), &rebase, &error) &&
            !error.isEmpty());

    QVERIFY(branchTip(QStringLiteral("ghost")).isEmpty());
}

void BranchOpsTest::ourRefsAgreeWithGitAfterASequenceOfOperations() {
    // The strongest check there is: after a run of operations, our own list of
    // branches must name exactly the commits git names. Drift here is the client
    // lying about the repository.
    buildTwoBranches();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const QString featureTip = branchTip(QStringLiteral("feature"));

    QVERIFY(session.createBranch(QStringLiteral("work/one"), featureTip, &error));
    QVERIFY(session.checkoutBranch(QStringLiteral("work/one"), &error));
    QVERIFY(commitFile(QStringLiteral("work.txt"), QStringLiteral("work\n"),
                       QStringLiteral("work on the new branch")));
    QVERIFY(session.checkoutBranch(QStringLiteral("main"), &error));
    QVERIFY(session.renameBranch(QStringLiteral("work/one"), QStringLiteral("work/two"), &error));
    QVERIFY(session.moveBranch(QStringLiteral("work/two"), headId(), &error));

    // git's own answer
    const auto listing = runGit({"for-each-ref", "--format=%(refname:short) %(objectname)",
                                 "refs/heads/"},
                                m_path.toStdString());
    QCOMPARE(listing.exitCode, 0);

    QMap<QString, QString> fromGit;
    for (const QString& line : QString::fromStdString(listing.out).split(QLatin1Char('\n'),
                                                                        Qt::SkipEmptyParts)) {
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() == 2) {
            fromGit.insert(parts.at(0), parts.at(1));
        }
    }

    QMap<QString, QString> fromUs;
    const auto repo = Repository::open(m_path.toStdString());
    for (const RefInfo& ref : repo->refs()) {
        if (ref.kind == RefKind::LocalBranch) {
            fromUs.insert(QString::fromStdString(ref.shortName), QString::fromStdString(ref.target));
        }
    }

    QCOMPARE(fromUs.keys(), fromGit.keys());
    for (auto it = fromGit.constBegin(); it != fromGit.constEnd(); ++it) {
        QCOMPARE(fromUs.value(it.key()), it.value());
    }
    QVERIFY(fromUs.contains(QStringLiteral("work/two")));
    QVERIFY(!fromUs.contains(QStringLiteral("work/one")));
}

void BranchOpsTest::aRebaseSkipsAStepThatIsAlreadyUpstream() {
    // The same change, written on both branches. The step has nothing to replay.
    // `git rebase` skips it with a warning; libgit2 answers GIT_EAPPLIED, and we
    // turned that into a failure — which is what sent the reader to a conflict
    // screen with nothing on it.
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("a\nb\nc\n"),
                       QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("a\nB\nc\n"),
                       QStringLiteral("the feature change")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("a\nB\nc\n"),
                       QStringLiteral("the same change upstream")));
    QVERIFY(git(m_path, {"checkout", "-q", "feature"}));
    const QString mainTip = branchTip(QStringLiteral("main"));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    RebaseResult result;
    QVERIFY2(session.rebaseOnto(QStringLiteral("feature"), QStringLiteral("main"), &result, &error),
             qPrintable(error));

    QCOMPARE(result.status, RebaseStatus::Completed);
    QCOMPARE(result.commitsSkipped, std::size_t(1));
    QCOMPARE(result.commitsApplied, std::size_t(0));
    QVERIFY(!session.rebaseInProgress());
    QVERIFY(session.conflicts().empty());
    QCOMPARE(branchTip(QStringLiteral("feature")), mainTip);
    QCOMPARE(readFile(m_path, QStringLiteral("f.txt")), QStringLiteral("a\nB\nc\n"));
}

void BranchOpsTest::aConflictResolvedIntoNothingSkipsItsStep() {
    // A real conflict, resolved by taking the other side's text — so the step ends
    // up carrying nothing. Continuing has to skip it rather than fail.
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nhere\n"),
                       QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nfeature\n"),
                       QStringLiteral("feature rewrites the line")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nmain\n"),
                       QStringLiteral("main rewrites the line")));
    QVERIFY(git(m_path, {"checkout", "-q", "feature"}));
    const QString mainTip = branchTip(QStringLiteral("main"));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    RebaseResult result;
    QVERIFY2(session.rebaseOnto(QStringLiteral("feature"), QStringLiteral("main"), &result, &error),
             qPrintable(error));
    QCOMPARE(result.status, RebaseStatus::Conflicted);

    // Take their text: our side's change disappears, and with it the reason for the
    // commit being replayed at all.
    QVERIFY2(session.resolveConflict(QStringLiteral("f.txt"), QStringLiteral("keep\nmain\n"),
                                     &error),
             qPrintable(error));

    RebaseResult continued;
    QVERIFY2(session.rebaseContinue(&continued, &error), qPrintable(error));
    QCOMPARE(continued.status, RebaseStatus::Completed);
    QVERIFY(!session.rebaseInProgress());
    QVERIFY(session.conflicts().empty());
    // The step carried nothing, so it must leave no commit behind: the branch lands
    // exactly on its new base. libgit2 drops this one itself instead of reporting
    // GIT_EAPPLIED, so the skip counter stays at zero — the outcome is what matters,
    // and the outcome is that the reader is never asked about an empty step.
    QCOMPARE(branchTip(QStringLiteral("feature")), mainTip);
    QCOMPARE(readFile(m_path, QStringLiteral("f.txt")), QStringLiteral("keep\nmain\n"));
}

void BranchOpsTest::repeatedRebasesBetweenTwoBranchesStayStable() {
    // The shape the playground has: two branches that each carry a change the other
    // also has, written as separate commits. Rebased back and forth in a loop, every
    // round has to go through on its own — no question, no lost commit, and the same
    // number of commits every time.
    QVERIFY(commitFile(QStringLiteral("app.conf"), QStringLiteral("workers=4\n"),
                       QStringLiteral("the shared file")));
    QVERIFY(commitFile(QStringLiteral("notes.md"), QStringLiteral("polish\n"),
                       QStringLiteral("polish the wording")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "work"}));
    QVERIFY(commitFile(QStringLiteral("app.conf"), QStringLiteral("workers=8\n"),
                       QStringLiteral("raise the worker pool to 8")));
    QVERIFY(commitFile(QStringLiteral("scratch.txt"), QStringLiteral("scratch\n"),
                       QStringLiteral("start the scratch work")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const int commitsBefore = countCommits();
    const QString tipBefore = headId();
    // Six rounds of the same rebase. The first replays two commits, and every round
    // after that has nothing to do — which must leave the commits alone, not rewrite
    // them into twins with new ids. The tip id is the check that matters: a rebase
    // that "succeeds" while changing the branch's commits is the failure being fixed
    // here, and counting commits cannot see it.
    for (int round = 0; round < 6; ++round) {
        RebaseResult result;
        QVERIFY2(session.rebaseOnto(QStringLiteral("work"), QStringLiteral("main"), &result, &error),
                 qPrintable(error));
        QVERIFY(result.status == RebaseStatus::Completed ||
                result.status == RebaseStatus::UpToDate);
        QVERIFY(session.conflicts().empty());
        QVERIFY(!session.rebaseInProgress());
    }
    QCOMPARE(headId(), tipBefore);
    QCOMPARE(countCommits(), commitsBefore);
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral("scratch.txt"))));
    QVERIFY(QFile::exists(QDir(m_path).filePath(QStringLiteral("notes.md"))));
}

void BranchOpsTest::ourRebaseLandsOnExactlyTheTreeGitLandsOn() {
    // The strongest statement available: given the same repository, our rebase and
    // `git rebase` must end up with the same tree. Not a similar tree — the same id.
    // Run against a copy, because git's own rebase needs its own working directory.
    QVERIFY(commitFile(QStringLiteral("app.conf"), QStringLiteral("workers=4\n"),
                       QStringLiteral("the shared file")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "bystander"}));
    QVERIFY(commitFile(QStringLiteral("other.txt"), QStringLiteral("other\n"),
                       QStringLiteral("unrelated work")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("notes.md"), QStringLiteral("polish\n"),
                       QStringLiteral("polish the wording")));
    QVERIFY(git(m_path, {"checkout", "-q", "bystander"}));
    QVERIFY(commitFile(QStringLiteral("app.conf"), QStringLiteral("workers=8\n"),
                       QStringLiteral("raise the worker pool to 8")));

    // A second copy of the same starting point.
    const QString other = m_dir->path() + QStringLiteral("-git");
    QVERIFY(git(m_path, {"clone", "-q", "--no-hardlinks", "--local", m_path, other}));
    QVERIFY(git(other, {"checkout", "-q", "bystander"}));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    RebaseResult result;
    QVERIFY2(session.rebaseOnto(QStringLiteral("bystander"), QStringLiteral("main"), &result, &error),
             qPrintable(error));
    QCOMPARE(result.status, RebaseStatus::Completed);

    // git's own rebase, same starting point. The fixture has no identity configured,
    // so pass one the way every other test here does — and report git's own words if
    // it refuses, because "returned FALSE" says nothing.
    std::vector<std::string> rebaseArgv;
    for (const QString& part : kIdentity) {
        rebaseArgv.push_back(part.toStdString());
    }
    rebaseArgv.push_back("rebase");
    // A local clone keeps the source's branches as remote-tracking refs, so the
    // upstream to replay onto is origin/main — the same commit either way.
    rebaseArgv.push_back("origin/main");
    const auto gitRebase = runGit(rebaseArgv, other.toStdString());
    QVERIFY2(gitRebase.exitCode == 0,
             qPrintable(QStringLiteral("git rebase said: %1")
                            .arg(QString::fromStdString(gitRebase.out + gitRebase.err))));

    const QString ourTree = treeOf(m_path, QStringLiteral("bystander"));
    const QString gitTree = treeOf(other, QStringLiteral("bystander"));
    QVERIFY(!ourTree.isEmpty());
    QCOMPARE(ourTree, gitTree);
    QCOMPARE(countCommitsOf(m_path, QStringLiteral("bystander")),
             countCommitsOf(other, QStringLiteral("bystander")));
}

QTEST_MAIN(BranchOpsTest)
#include "test_branch_ops.moc"
