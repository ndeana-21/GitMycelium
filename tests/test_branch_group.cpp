/// Group branch operations, against real repositories.
///
/// The rule these exist for: a mass operation must either do all of it or say
/// plainly what it did not, and it must never lose commits it did not name. So the
/// tests ask git what the branches are afterwards, and check that one undo puts a
/// whole deleted group back.

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

const QStringList kIdentity{"-c", "user.name=Test", "-c", "user.email=test@localhost"};

} // namespace

class BranchGroupTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void mergedIntoFindsOnlyTheMergedBranches();
    void mergedIntoNeverOffersHeadOrTarget();
    void unmergedAmongNamesTheRiskyBranches();
    void aBatchDeleteRemovesThemAllAndOneUndoPutsThemBack();
    void aBatchDeleteRefusesTheCheckedOutBranchAndKeepsGoing();
    void aBranchMergedOnlyIntoTheTargetIsDeletedWithoutForce();
    void setUpstreamsOnlyWhereTheRemoteTrackingBranchExists();

private:
    bool commitFile(const QString& name, const QString& content, const QString& message);
    bool branchExists(const QString& name) const;
    bool remoteTrackingExists(const QString& name) const;
    /// main (checked out), `merged-1` and `merged-2` merged into it, `unmerged`
    /// carrying a commit of its own.
    void buildMergedAndUnmerged();

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void BranchGroupTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
}

void BranchGroupTest::cleanup() {
    m_dir.reset();
}

bool BranchGroupTest::commitFile(const QString& name, const QString& content,
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

bool BranchGroupTest::branchExists(const QString& name) const {
    return git(m_path, {"rev-parse", "--verify", "--quiet", "refs/heads/" + name});
}

bool BranchGroupTest::remoteTrackingExists(const QString& name) const {
    return git(m_path, {"rev-parse", "--verify", "--quiet", "refs/remotes/" + name});
}

void BranchGroupTest::buildMergedAndUnmerged() {
    QVERIFY(commitFile(QStringLiteral("base.txt"), QStringLiteral("base\n"),
                       QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "merged-1"}));
    QVERIFY(commitFile(QStringLiteral("m1.txt"), QStringLiteral("m1\n"), QStringLiteral("m1")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(git(m_path, {"merge", "-q", "--no-ff", "-m", "merge merged-1", "merged-1"}));
    QVERIFY(git(m_path, {"branch", "merged-2", "main"}));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "unmerged"}));
    QVERIFY(commitFile(QStringLiteral("u.txt"), QStringLiteral("u\n"), QStringLiteral("u")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
}

void BranchGroupTest::mergedIntoFindsOnlyTheMergedBranches() {
    buildMergedAndUnmerged();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    QStringList found = session.branchesMergedInto(QStringLiteral("refs/heads/main"));
    found.sort();

    // The oracle: git's own answer for what is already in main.
    const QString mergedByGit = gitOut(m_path, {"branch", "--format=%(refname:short)",
                                                "--merged", "main"});
    QStringList oracle = mergedByGit.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    oracle.removeAll(QStringLiteral("main")); // the target itself is not a candidate
    oracle.sort();

    QCOMPARE(found, oracle);
    QVERIFY(found.contains(QStringLiteral("merged-1")));
    QVERIFY(found.contains(QStringLiteral("merged-2")));
    QVERIFY(!found.contains(QStringLiteral("unmerged")));
}

void BranchGroupTest::mergedIntoNeverOffersHeadOrTarget() {
    buildMergedAndUnmerged();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const QStringList found = session.branchesMergedInto(QStringLiteral("refs/heads/main"));
    QVERIFY(!found.contains(QStringLiteral("main")));
}

void BranchGroupTest::unmergedAmongNamesTheRiskyBranches() {
    buildMergedAndUnmerged();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const QStringList unmerged = session.unmergedAmong(
        {QStringLiteral("merged-1"), QStringLiteral("merged-2"), QStringLiteral("unmerged")},
        QStringLiteral("refs/heads/main"));
    QCOMPARE(unmerged, QStringList{QStringLiteral("unmerged")});
}

void BranchGroupTest::aBatchDeleteRemovesThemAllAndOneUndoPutsThemBack() {
    buildMergedAndUnmerged();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    RepoSession::BranchBatchResult result;
    QVERIFY(session.deleteBranches({QStringLiteral("merged-1"), QStringLiteral("merged-2")},
                                   QStringLiteral("refs/heads/main"), /*forceUnmerged=*/false,
                                   &result, &error));
    QCOMPARE(result.done.size(), 2);
    QVERIFY(result.failed.isEmpty());
    QVERIFY(!branchExists(QStringLiteral("merged-1")));
    QVERIFY(!branchExists(QStringLiteral("merged-2")));

    // The whole group is one operation: a single undo has to bring them all back,
    // not one of them.
    QVERIFY(session.canUndo());
    QString label;
    QVERIFY(session.undo(&label, &error));
    QVERIFY(branchExists(QStringLiteral("merged-1")));
    QVERIFY(branchExists(QStringLiteral("merged-2")));
}

void BranchGroupTest::aBatchDeleteRefusesTheCheckedOutBranchAndKeepsGoing() {
    buildMergedAndUnmerged();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    RepoSession::BranchBatchResult result;
    QVERIFY(session.deleteBranches({QStringLiteral("main"), QStringLiteral("merged-1")},
                                   QStringLiteral("refs/heads/main"), /*forceUnmerged=*/true,
                                   &result, &error));
    QCOMPARE(result.done, QStringList{QStringLiteral("merged-1")});
    QCOMPARE(result.failed.size(), 1);
    QVERIFY(result.failed.front().startsWith(QStringLiteral("main")));
    QVERIFY(branchExists(QStringLiteral("main")));
    QVERIFY(!branchExists(QStringLiteral("merged-1")));
}

void BranchGroupTest::aBranchMergedOnlyIntoTheTargetIsDeletedWithoutForce() {
    buildMergedAndUnmerged();
    // Stand somewhere merged-1 does not reach: "merged into the target" must be
    // asked of the target, not of whatever HEAD happens to be.
    const QString base = gitOut(m_path, {"rev-list", "--max-parents=0", "HEAD"});
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "side", base}));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    RepoSession::BranchBatchResult result;
    // Against HEAD the branch carries unreachable commits, so this is refused.
    QVERIFY(session.deleteBranches({QStringLiteral("merged-1")}, QString(),
                                   /*forceUnmerged=*/false, &result, &error));
    QCOMPARE(result.failed.size(), 1);
    QVERIFY(branchExists(QStringLiteral("merged-1")));

    // Against the target it is merged, so no force is needed.
    result = RepoSession::BranchBatchResult{};
    QVERIFY(session.deleteBranches({QStringLiteral("merged-1")},
                                   QStringLiteral("refs/heads/main"), /*forceUnmerged=*/false,
                                   &result, &error));
    QCOMPARE(result.done, QStringList{QStringLiteral("merged-1")});
    QVERIFY(result.failed.isEmpty());
    QVERIFY(!branchExists(QStringLiteral("merged-1")));
    QVERIFY(branchExists(QStringLiteral("main")));
}

void BranchGroupTest::setUpstreamsOnlyWhereTheRemoteTrackingBranchExists() {
    // The remote lives outside the work tree, or `add -A` would commit its objects.
    QTemporaryDir remoteDir;
    QVERIFY(remoteDir.isValid());
    const QString bare = QDir(remoteDir.path()).filePath(QStringLiteral("origin.git"));
    QVERIFY(git(m_path, {"init", "-q", "--bare", bare}));
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("a\n"), QStringLiteral("base")));
    QVERIFY(git(m_path, {"remote", "add", "origin", bare}));
    QVERIFY(git(m_path, {"push", "-q", "-u", "origin", "main"}));

    QVERIFY(git(m_path, {"checkout", "-q", "-b", "pushed"}));
    QVERIFY(commitFile(QStringLiteral("p.txt"), QStringLiteral("p\n"), QStringLiteral("p")));
    QVERIFY(git(m_path, {"push", "-q", "-u", "origin", "pushed"}));
    QVERIFY(remoteTrackingExists(QStringLiteral("origin/pushed")));

    QVERIFY(git(m_path, {"checkout", "-q", "-b", "never-pushed"}));
    QVERIFY(commitFile(QStringLiteral("n.txt"), QStringLiteral("n\n"), QStringLiteral("n")));
    QVERIFY(!remoteTrackingExists(QStringLiteral("origin/never-pushed")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    RepoSession::BranchBatchResult result;
    QVERIFY(session.setUpstreams({QStringLiteral("pushed"), QStringLiteral("never-pushed")},
                                 QStringLiteral("origin"), &result, &error));
    QCOMPARE(result.done, QStringList{QStringLiteral("pushed")});
    QCOMPARE(result.failed.size(), 1);
    QVERIFY(result.failed.front().startsWith(QStringLiteral("never-pushed")));

    // git is the oracle for the config, not our own report.
    QCOMPARE(gitOut(m_path, {"config", "--get", "branch.pushed.merge"}),
             QStringLiteral("refs/heads/pushed"));
    QVERIFY(!git(m_path, {"config", "--get", "branch.never-pushed.merge"}));
}

QTEST_MAIN(BranchGroupTest)
#include "test_branch_group.moc"
