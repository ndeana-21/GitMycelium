#include "core/types.h"
#include "git/cli.h"
#include "git/repository.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>
#include <memory>

using namespace gk;

namespace {

namespace fs = std::filesystem;

bool git(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    const ProcessResult r = runGit(argv, dir.toStdString());
    return r.exitCode == 0;
}

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

} // namespace

class MergeWorkflowTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void createsAndChecksOutBranches();
    void refusesToDeleteTheCurrentBranch();
    void fastForwardsWhenTargetIsAhead();
    void cleanMergeCreatesAMergeCommit();
    void conflictingMergeExposesThreeStages();
    void resolvingAConflictStagesTheResult();
    void completingAMergeCreatesATwoParentCommit();
    void abortingAMergeRestoresHeadAndWorktree();
    void checkoutOfAMissingBranchFails();

private:
    bool commitFile(const QString& name, const QString& content, const QString& message);
    std::unique_ptr<Repository> open() const;
    QString readWorktree(const QString& name) const;

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void MergeWorkflowTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
}

void MergeWorkflowTest::cleanup() {
    m_dir.reset();
}

bool MergeWorkflowTest::commitFile(const QString& name, const QString& content,
                                   const QString& message) {
    if (!writeFile(m_path, name, content)) {
        return false;
    }
    if (!git(m_path, {"add", name})) {
        return false;
    }
    return git(m_path, {"-c", "user.name=Test", "-c", "user.email=test@localhost", "commit",
                        "-q", "-m", message});
}

std::unique_ptr<Repository> MergeWorkflowTest::open() const {
    return Repository::open(m_path.toStdString());
}

QString MergeWorkflowTest::readWorktree(const QString& name) const {
    QFile file(QDir(m_path).filePath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

void MergeWorkflowTest::createsAndChecksOutBranches() {
    QVERIFY(commitFile("a.txt", "one\n", "first"));
    auto repo = open();
    const std::string head = repo->info().headId;

    repo->createBranch("feature", head);
    repo->checkoutBranch("feature");
    QCOMPARE(QString::fromStdString(repo->info().headBranch), QStringLiteral("feature"));

    repo->checkoutBranch("main");
    QCOMPARE(QString::fromStdString(repo->info().headBranch), QStringLiteral("main"));
}

void MergeWorkflowTest::refusesToDeleteTheCurrentBranch() {
    QVERIFY(commitFile("a.txt", "one\n", "first"));
    auto repo = open();
    const std::string head = repo->info().headId;
    repo->createBranch("doomed", head);

    repo->checkoutBranch("doomed");
    QVERIFY_THROWS_EXCEPTION(GitError, repo->deleteBranch("doomed"));

    repo->checkoutBranch("main");
    repo->deleteBranch("doomed");
    for (const RefInfo& ref : repo->refs()) {
        QVERIFY(ref.shortName != "doomed");
    }
}

void MergeWorkflowTest::fastForwardsWhenTargetIsAhead() {
    QVERIFY(commitFile("a.txt", "one\n", "first"));
    auto repo = open();
    const std::string base = repo->info().headId;

    repo->createBranch("feature", base);
    repo->checkoutBranch("feature");
    QVERIFY(commitFile("b.txt", "two\n", "second on feature"));
    const std::string featureHead = repo->info().headId;

    repo->checkoutBranch("main");
    const MergeResult result = repo->mergeBranch("feature");

    QCOMPARE(result.status, MergeStatus::FastForward);
    QCOMPARE(QString::fromStdString(repo->info().headId), QString::fromStdString(featureHead));
    bool hasMore = false;
    const auto commits = repo->commits(0, 1, &hasMore);
    QCOMPARE(commits.size(), static_cast<std::size_t>(1));
    QCOMPARE(commits.at(0).parents.size(), static_cast<std::size_t>(1));
}

void MergeWorkflowTest::cleanMergeCreatesAMergeCommit() {
    QVERIFY(commitFile("base.txt", "base\n", "base"));
    auto repo = open();
    const std::string base = repo->info().headId;

    repo->createBranch("feature", base);
    repo->checkoutBranch("feature");
    QVERIFY(commitFile("feature.txt", "from feature\n", "feature work"));

    repo->checkoutBranch("main");
    QVERIFY(commitFile("main.txt", "from main\n", "main work"));

    const MergeResult result = repo->mergeBranch("feature");
    QCOMPARE(result.status, MergeStatus::Clean);
    QCOMPARE(result.conflictCount, static_cast<std::size_t>(0));
    QVERIFY(!repo->mergeInProgress());

    bool hasMore = false;
    const auto commits = repo->commits(0, 1, &hasMore);
    QCOMPARE(commits.at(0).parents.size(), static_cast<std::size_t>(2));
    QVERIFY(commits.at(0).summary.find("Merge") != std::string::npos);

    QVERIFY(fs::exists(fs::path(m_path.toStdString()) / "feature.txt"));
    QVERIFY(fs::exists(fs::path(m_path.toStdString()) / "main.txt"));
}

void MergeWorkflowTest::conflictingMergeExposesThreeStages() {
    QVERIFY(commitFile("a.txt", "line1\nline2\nline3\n", "base"));
    auto repo = open();
    const std::string base = repo->info().headId;

    repo->createBranch("feature", base);
    repo->checkoutBranch("feature");
    QVERIFY(commitFile("a.txt", "line1\nline2-feature\nline3\n", "feature edit"));

    repo->checkoutBranch("main");
    QVERIFY(commitFile("a.txt", "line1\nline2-main\nline3\n", "main edit"));

    const MergeResult result = repo->mergeBranch("feature");
    QCOMPARE(result.status, MergeStatus::Conflicted);
    QCOMPARE(result.conflictCount, static_cast<std::size_t>(1));
    QVERIFY(repo->mergeInProgress());

    const auto conflicts = repo->conflicts();
    QCOMPARE(conflicts.size(), static_cast<std::size_t>(1));
    const ConflictEntry& entry = conflicts.at(0);
    QCOMPARE(QString::fromStdString(entry.path), QStringLiteral("a.txt"));
    QCOMPARE(entry.kind, ConflictKind::BothModified);
    QVERIFY(!entry.ancestor.empty());
    QVERIFY(!entry.ours.empty());
    QVERIFY(!entry.theirs.empty());

    // Stage 1/2/3 map to base/ours/theirs — the three panes of the editor.
    const QString ancestor = QString::fromStdString(repo->blobText(entry.ancestor));
    const QString ours = QString::fromStdString(repo->blobText(entry.ours));
    const QString theirs = QString::fromStdString(repo->blobText(entry.theirs));
    QVERIFY(ancestor.contains(QStringLiteral("line2\n")));
    QVERIFY(ours.contains(QStringLiteral("line2-main")));
    QVERIFY(theirs.contains(QStringLiteral("line2-feature")));

    QCOMPARE(repo->info().operation, RepoOperation::Merge);
}

void MergeWorkflowTest::resolvingAConflictStagesTheResult() {
    QVERIFY(commitFile("a.txt", "line1\nline2\nline3\n", "base"));
    auto repo = open();
    const std::string base = repo->info().headId;
    repo->createBranch("feature", base);
    repo->checkoutBranch("feature");
    QVERIFY(commitFile("a.txt", "line1\nline2-feature\nline3\n", "feature edit"));
    repo->checkoutBranch("main");
    QVERIFY(commitFile("a.txt", "line1\nline2-main\nline3\n", "main edit"));
    QVERIFY(repo->mergeBranch("feature").status == MergeStatus::Conflicted);

    repo->resolveConflict("a.txt", "line1\nline2-resolved\nline3\n");

    QVERIFY(repo->conflicts().empty());
    QVERIFY(repo->mergeInProgress());
    QCOMPARE(readWorktree("a.txt"), QStringLiteral("line1\nline2-resolved\nline3\n"));

    bool sawStaged = false;
    for (const FileChange& change : repo->status()) {
        if (change.path == "a.txt") {
            QVERIFY(change.staged);
            QVERIFY(change.kind != ChangeKind::Conflicted);
            sawStaged = true;
        }
    }
    QVERIFY(sawStaged);
}

void MergeWorkflowTest::completingAMergeCreatesATwoParentCommit() {
    QVERIFY(commitFile("a.txt", "line1\nline2\nline3\n", "base"));
    auto repo = open();
    const std::string base = repo->info().headId;
    repo->createBranch("feature", base);
    repo->checkoutBranch("feature");
    QVERIFY(commitFile("a.txt", "line1\nline2-feature\nline3\n", "feature edit"));
    repo->checkoutBranch("main");
    QVERIFY(commitFile("a.txt", "line1\nline2-main\nline3\n", "main edit"));
    QVERIFY(repo->mergeBranch("feature").status == MergeStatus::Conflicted);
    repo->resolveConflict("a.txt", "line1\nline2-merged\nline3\n");

    const Oid mergeCommit = repo->commitMerge("Merge branch 'feature'");
    QVERIFY(!mergeCommit.empty());
    QVERIFY(!repo->mergeInProgress());
    QVERIFY(repo->conflicts().empty());

    const auto head = repo->commits(0, 1, nullptr);
    QCOMPARE(head.size(), static_cast<std::size_t>(1));
    QCOMPARE(QString::fromStdString(head.at(0).id), QString::fromStdString(mergeCommit));
    QCOMPARE(head.at(0).parents.size(), static_cast<std::size_t>(2));
    QVERIFY(head.at(0).summary.find("Merge branch 'feature'") != std::string::npos);
}

void MergeWorkflowTest::abortingAMergeRestoresHeadAndWorktree() {
    QVERIFY(commitFile("a.txt", "line1\nline2\nline3\n", "base"));
    auto repo = open();
    const std::string base = repo->info().headId;
    repo->createBranch("feature", base);
    repo->checkoutBranch("feature");
    QVERIFY(commitFile("a.txt", "line1\nline2-feature\nline3\n", "feature edit"));
    repo->checkoutBranch("main");
    QVERIFY(commitFile("a.txt", "line1\nline2-main\nline3\n", "main edit"));
    const std::string mainHead = repo->info().headId;
    QVERIFY(repo->mergeBranch("feature").status == MergeStatus::Conflicted);

    repo->abortMerge();

    QVERIFY(!repo->mergeInProgress());
    QVERIFY(repo->conflicts().empty());
    QCOMPARE(QString::fromStdString(repo->info().headId), QString::fromStdString(mainHead));
    QCOMPARE(repo->info().operation, RepoOperation::None);
    QCOMPARE(readWorktree("a.txt"), QStringLiteral("line1\nline2-main\nline3\n"));
}

void MergeWorkflowTest::checkoutOfAMissingBranchFails() {
    QVERIFY(commitFile("a.txt", "one\n", "first"));
    auto repo = open();
    QVERIFY_THROWS_EXCEPTION(GitError, repo->checkoutBranch("does-not-exist"));
    QVERIFY_THROWS_EXCEPTION(GitError, repo->mergeBranch("does-not-exist"));
}

QTEST_MAIN(MergeWorkflowTest)
#include "test_merge_workflow.moc"
