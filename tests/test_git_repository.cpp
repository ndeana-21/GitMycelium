#include "git/cli.h"
#include "git/repository.h"
#include "repo/repo_session.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>
#include <memory>

using namespace gk;

namespace {

bool git(const QString& workDir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const auto& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, workDir.toStdString()).exitCode == 0;
}

bool writeFile(const QString& path, const QByteArray& content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    return f.write(content) == content.size();
}

} // namespace

bool hasBranch(const Repository& repo, const std::string& name) {
    for (const RefInfo& ref : repo.refs()) {
        if (ref.kind == RefKind::LocalBranch && ref.shortName == name) {
            return true;
        }
    }
    return false;
}

class RepositoryTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;

    bool commitFile(const QString& name, const QByteArray& content, const QString& message) {
        if (!writeFile(m_path + "/" + name, content)) {
            return false;
        }
        return git(m_path, {"add", name}) && git(m_path, {"commit", "-m", message});
    }

    std::unique_ptr<Repository> open() { return Repository::open(m_path.toStdString()); }

private slots:
    void initTestCase() { Repository::initLibrary(); }

    void init() {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        m_path = m_dir->path();
        QVERIFY(git(m_path, {"init", "-b", "main"}));
        QVERIFY(git(m_path, {"config", "user.name", "Test"}));
        QVERIFY(git(m_path, {"config", "user.email", "test@example.com"}));
        QVERIFY(git(m_path, {"config", "commit.gpgsign", "false"}));
    }

    void cleanup() { m_dir.reset(); }

    void reportsRepositoryInfo() {
        QVERIFY(commitFile("a.txt", "hello\n", "first commit"));
        QVERIFY(commitFile("b.txt", "world\n", "second commit"));

        const auto repo = open();
        const RepoInfo info = repo->info();
        QCOMPARE(QString::fromStdString(info.headBranch), QStringLiteral("main"));
        QVERIFY(!info.bare);
        QVERIFY(!info.detached);
        QVERIFY(!info.partialClone);
        QCOMPARE(info.operation, RepoOperation::None);
        QCOMPARE(info.headId.size(), static_cast<std::size_t>(40));
        QVERIFY(!info.gitDir.empty());
        QVERIFY(!info.workDir.empty());
    }

    void listsReferences() {
        QVERIFY(commitFile("a.txt", "hello\n", "first commit"));
        QVERIFY(git(m_path, {"tag", "v1.0"}));
        QVERIFY(git(m_path, {"branch", "feature/x"}));

        const auto repo = open();
        bool sawMain = false;
        bool sawTag = false;
        bool sawFeature = false;
        for (const auto& r : repo->refs()) {
            if (r.fullName == "refs/heads/main") {
                sawMain = r.kind == RefKind::LocalBranch && r.shortName == "main";
            }
            if (r.fullName == "refs/heads/feature/x") {
                sawFeature = r.kind == RefKind::LocalBranch && r.shortName == "feature/x";
            }
            if (r.fullName == "refs/tags/v1.0") {
                sawTag = r.kind == RefKind::Tag && !r.target.empty();
            }
        }
        QVERIFY(sawMain);
        QVERIFY(sawFeature);
        QVERIFY(sawTag);
    }

    void pagesHistory() {
        QVERIFY(commitFile("a.txt", "1\n", "commit one"));
        QVERIFY(commitFile("a.txt", "2\n", "commit two"));
        QVERIFY(commitFile("a.txt", "3\n", "commit three"));

        const auto repo = open();

        bool hasMore = true;
        const auto first = repo->commits(0, 2, &hasMore);
        QCOMPARE(first.size(), static_cast<std::size_t>(2));
        QCOMPARE(QString::fromStdString(first.at(0).summary), QStringLiteral("commit three"));
        QCOMPARE(QString::fromStdString(first.at(1).summary), QStringLiteral("commit two"));
        QVERIFY(hasMore);
        QCOMPARE(first.at(0).parents.size(), static_cast<std::size_t>(1));
        QVERIFY(!first.at(0).author.email.empty());

        const auto second = repo->commits(2, 2, &hasMore);
        QCOMPARE(second.size(), static_cast<std::size_t>(1));
        QCOMPARE(QString::fromStdString(second.at(0).summary), QStringLiteral("commit one"));
        QVERIFY(!hasMore);
    }

    void reportsWorkingTreeChanges() {
        QVERIFY(commitFile("a.txt", "hello\n", "first commit"));
        QVERIFY(writeFile(m_path + "/untracked.txt", "new\n"));
        QVERIFY(writeFile(m_path + "/a.txt", "changed\n"));

        const auto repo = open();
        bool sawUntracked = false;
        bool sawModified = false;
        for (const auto& c : repo->status()) {
            if (c.path == "untracked.txt" && c.kind == ChangeKind::Untracked && !c.staged) {
                sawUntracked = true;
            }
            if (c.path == "a.txt" && c.kind == ChangeKind::Modified && !c.staged) {
                sawModified = true;
            }
        }
        QVERIFY(sawUntracked);
        QVERIFY(sawModified);
    }

    void stagesFileAndReportsStagedState() {
        QVERIFY(commitFile("a.txt", "hello\n", "first commit"));
        QVERIFY(writeFile(m_path + "/a.txt", "changed\n"));
        QVERIFY(git(m_path, {"add", "a.txt"}));

        const auto repo = open();
        bool sawStaged = false;
        for (const auto& c : repo->status()) {
            if (c.path == "a.txt" && c.staged) {
                sawStaged = true;
            }
        }
        QVERIFY(sawStaged);
    }

    void detectsInProgressMerge() {
        QVERIFY(commitFile("a.txt", "base\n", "base"));
        QVERIFY(git(m_path, {"checkout", "-b", "side"}));
        QVERIFY(commitFile("a.txt", "side\n", "side change"));
        QVERIFY(git(m_path, {"checkout", "main"}));
        QVERIFY(commitFile("a.txt", "main\n", "main change"));

        git(m_path, {"merge", "side"});

        const auto repo = open();
        const RepoInfo info = repo->info();
        QCOMPARE(info.operation, RepoOperation::Merge);

        bool conflicted = false;
        for (const auto& c : repo->status()) {
            if (c.kind == ChangeKind::Conflicted) {
                conflicted = true;
            }
        }
        QVERIFY(conflicted);
    }

    void sessionOpensRepository() {
        QVERIFY(commitFile("a.txt", "hello\n", "first commit"));

        RepoSession session;
        QString error;
        QVERIFY2(session.open(m_path, &error), qPrintable(error));
        QVERIFY(session.isOpen());
        QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("main"));

        bool hasMore = false;
        const auto page = session.commitsBlocking(0, 10, &hasMore);
        QCOMPARE(page.size(), static_cast<std::size_t>(1));
        QVERIFY(!hasMore);

        QString bad;
        QVERIFY(!session.open(m_path + "/definitely-not-a-repo", &bad));
        QVERIFY(!bad.isEmpty());
        // A failed open must not clobber the still-open repository.
        QVERIFY(session.isOpen());
    }

    void reportsUnbornHeadWithoutCrashing() {
        const auto repo = open();
        const RepoInfo info = repo->info();
        QVERIFY(info.headId.empty());
        QVERIFY(info.headBranch.empty());
        bool hasMore = true;
        QVERIFY(repo->commits(0, 10, &hasMore).empty());
        QVERIFY(repo->refs().empty());
    }

    void cliRunsGitBinary() {
        QVERIFY(commitFile("a.txt", "hello\n", "first commit"));
        const ProcessResult r = runGit({"rev-parse", "--abbrev-ref", "HEAD"}, m_path.toStdString());
        QCOMPARE(r.exitCode, 0);
        QVERIFY(r.out.find("main") != std::string::npos);
        const auto repo = open();
        QVERIFY(!repo->info().partialClone);
    }

    void reportsFilesChangedByCommit() {
        QVERIFY(commitFile("a.txt", "one\n", "first"));
        QVERIFY(commitFile("b.txt", "two\n", "second"));
        QVERIFY(commitFile("a.txt", "one changed\n", "third"));

        const auto repo = open();
        bool hasMore = false;
        const auto commits = repo->commits(0, 1, &hasMore);
        QCOMPARE(commits.size(), static_cast<std::size_t>(1));

        const auto files = repo->changedFiles(commits.at(0).id);
        QCOMPARE(files.size(), static_cast<std::size_t>(1));
        QCOMPARE(QString::fromStdString(files.at(0).path), QStringLiteral("a.txt"));
        QCOMPARE(files.at(0).kind, ChangeKind::Modified);
    }

    void amendsHeadMessageKeepingTheTree() {
        QVERIFY(commitFile("a.txt", "hello\n", "first commit"));
        QVERIFY(commitFile("b.txt", "world\n", "second commit"));

        const auto repo = open();
        const RepoInfo before = repo->info();
        const auto filesBefore = repo->changedFiles(before.headId);
        QCOMPARE(filesBefore.size(), static_cast<std::size_t>(1));

        repo->amendHeadMessage("second commit, reworded");

        const RepoInfo after = repo->info();
        QVERIFY2(after.headId != before.headId, "an amend must produce a new commit object");
        QCOMPARE(after.headBranch, before.headBranch);

        const auto filesAfter = repo->changedFiles(after.headId);
        QCOMPARE(filesAfter.size(), filesBefore.size());
        QCOMPARE(filesAfter.front().path, filesBefore.front().path);

        const auto commits = repo->commits(0, 1, nullptr, HistoryFilter{HistoryScope::AllRefs, {}});
        QVERIFY(!commits.empty());
        QCOMPARE(QString::fromStdString(commits.front().summary),
                 QStringLiteral("second commit, reworded"));
    }

    void amendingWithoutACommitFails() {
        const auto repo = open();
        bool threw = false;
        try {
            repo->amendHeadMessage("nothing to amend");
        } catch (const std::exception&) {
            threw = true;
        }
        QVERIFY(threw);
    }

    /// Checking out a remote-tracking branch means making a local branch that
    /// stands for it — and one that follows it, or the first push goes somewhere
    /// nobody intended.
    void createsATrackingBranchForARemote() {
        QVERIFY(commitFile("a.txt", "hello\n", "first commit"));
        // A remote-tracking branch only exists because a remote is configured; the
        // upstream cannot be set without one.
        QVERIFY(git(m_path, {"remote", "add", "origin", "https://example.invalid/repo.git"}));
        QVERIFY(git(m_path, {"update-ref", "refs/remotes/origin/feature", "HEAD"}));

        const auto repo = open();
        try {
            repo->createTrackingBranch("feature", "origin/feature");
        } catch (const std::exception& error) {
            QFAIL(qPrintable(QStringLiteral("createTrackingBranch threw: %1")
                                 .arg(QString::fromUtf8(error.what()))));
        }
        repo->checkoutBranch("feature");

        const RepoInfo info = repo->info();
        QCOMPARE(QString::fromStdString(info.headBranch), QStringLiteral("feature"));
        QVERIFY(!info.detached);

        const UpstreamInfo upstream = repo->upstreamInfo();
        QVERIFY(upstream.configured);
        QCOMPARE(QString::fromStdString(upstream.name), QStringLiteral("origin/feature"));
    }

    void trackingABranchThatDoesNotExistFails() {
        QVERIFY(commitFile("a.txt", "hello\n", "first commit"));

        const auto repo = open();
        bool threw = false;
        try {
            repo->createTrackingBranch("feature", "origin/nowhere");
        } catch (const std::exception&) {
            threw = true;
        }
        QVERIFY(threw);
        QVERIFY2(!hasBranch(*repo, "feature"), "a failed attempt must not leave a branch behind");
    }

    void aBranchWithoutAWorkingUpstreamIsNotLeftBehind() {
        QVERIFY(commitFile("a.txt", "hello\n", "first commit"));
        QVERIFY(git(m_path, {"update-ref", "refs/remotes/origin/feature", "HEAD"}));

        const auto repo = open();
        bool threw = false;
        try {
            repo->createTrackingBranch("feature", "origin/feature");
        } catch (const std::exception&) {
            threw = true;
        }
        QVERIFY(threw);
        QVERIFY2(!hasBranch(*repo, "feature"),
                 "a branch that does not follow anything must not be left behind");
    }

    void diffsSingleFileAgainstParent() {
        QVERIFY(commitFile("a.txt", "alpha\nbeta\ngamma\n", "first"));
        QVERIFY(commitFile("a.txt", "alpha\nBETA\ngamma\n", "second"));

        const auto repo = open();
        bool hasMore = false;
        const auto commits = repo->commits(0, 1, &hasMore);
        const FileDiff diff = repo->commitFileDiff(commits.at(0).id, "a.txt");

        QCOMPARE(QString::fromStdString(diff.newPath), QStringLiteral("a.txt"));
        QVERIFY(!diff.binary);
        QCOMPARE(diff.added, 1);
        QCOMPARE(diff.removed, 1);
        QCOMPARE(diff.hunks.size(), static_cast<std::size_t>(1));

        bool sawRemoved = false;
        bool sawAdded = false;
        for (const auto& line : diff.hunks.at(0).lines) {
            if (line.kind == DiffLineKind::Removed) {
                sawRemoved = true;
                QCOMPARE(QString::fromStdString(line.text), QStringLiteral("beta"));
            }
            if (line.kind == DiffLineKind::Added) {
                sawAdded = true;
                QCOMPARE(QString::fromStdString(line.text), QStringLiteral("BETA"));
                QVERIFY(line.newLine > 0);
            }
        }
        QVERIFY(sawRemoved);
        QVERIFY(sawAdded);
    }

    void diffDetectsWholeFileAddition() {
        QVERIFY(commitFile("first.txt", "content\n", "first"));
        QVERIFY(commitFile("second.txt", "brand new\n", "second"));

        const auto repo = open();
        bool hasMore = false;
        const auto commits = repo->commits(0, 1, &hasMore);
        const FileDiff diff = repo->commitFileDiff(commits.at(0).id, "second.txt");
        QCOMPARE(diff.added, 1);
        QCOMPARE(diff.removed, 0);
    }

    void rootCommitDiffsAgainstEmptyTree() {
        QVERIFY(commitFile("a.txt", "only\n", "root"));
        const auto repo = open();
        bool hasMore = false;
        const auto commits = repo->commits(0, 1, &hasMore);
        const auto files = repo->changedFiles(commits.at(0).id);
        QCOMPARE(files.size(), static_cast<std::size_t>(1));
        QCOMPARE(files.at(0).kind, ChangeKind::Added);
    }
};

QTEST_MAIN(RepositoryTest)
#include "test_git_repository.moc"
