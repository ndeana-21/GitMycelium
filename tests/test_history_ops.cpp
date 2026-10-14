#include "git/cli.h"
#include "git/repository.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

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

QString gitOut(const QString& workDir, const QStringList& args) {
    std::vector<std::string> argv;
    for (const auto& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, workDir.toStdString()).out).trimmed();
}

bool writeFile(const QString& path, const QByteArray& content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    return f.write(content) == content.size();
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return f.readAll();
}

} // namespace

class HistoryOperationsTest : public QObject {
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

    QStringList subjects() {
        const QString out = gitOut(m_path, {"log", "--all", "--format=%s"});
        return out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }

    QString headSubject() { return gitOut(m_path, {"log", "-1", "--format=%s"}); }
    QString headId() { return gitOut(m_path, {"rev-parse", "HEAD"}); }
    QString branchTip(const QString& branch) {
        return gitOut(m_path, {"rev-parse", branch});
    }

private slots:
    void initTestCase() { Repository::initLibrary(); }

    void init() {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        m_path = m_dir->path();
        QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
        QVERIFY(git(m_path, {"config", "user.name", "Test"}));
        QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
        QVERIFY(git(m_path, {"config", "commit.gpgsign", "false"}));
    }

    void cleanup() { m_dir.reset(); }

    void renameBranchKeepsTheTip() {
        QVERIFY(commitFile("a.txt", "one\n", "first"));
        const QString before = branchTip("main");

        auto repo = open();
        repo->renameBranch("main", "trunk");
        repo.reset();

        QCOMPARE(gitOut(m_path, {"rev-parse", "trunk"}), before);
        QVERIFY(!git(m_path, {"rev-parse", "--verify", "refs/heads/main"}));
        QCOMPARE(gitOut(m_path, {"symbolic-ref", "--short", "HEAD"}), QStringLiteral("trunk"));
    }

    void renameBranchRefusesAnExistingName() {
        QVERIFY(commitFile("a.txt", "one\n", "first"));
        QVERIFY(git(m_path, {"branch", "other"}));

        auto repo = open();
        QVERIFY_THROWS_EXCEPTION(GitError, repo->renameBranch("main", "other"));
        QCOMPARE(branchTip("other"), branchTip("main"));
    }

    void cherryPickReplaysTheCommitAndKeepsTheAuthor() {
        QVERIFY(commitFile("base.txt", "base\n", "base"));
        QVERIFY(git(m_path, {"checkout", "-q", "-b", "side"}));
        QVERIFY(writeFile(m_path + "/side.txt", "from the side\n"));
        QVERIFY(git(m_path, {"add", "side.txt"}));
        // A distinct author, so "authorship is preserved" is actually testable.
        QVERIFY(git(m_path, {"commit", "-m", "side work", "--author",
                             "Side Author <side@localhost>"}));
        const QString sideId = headId();
        QVERIFY(git(m_path, {"checkout", "-q", "main"}));

        auto repo = open();
        const Oid created = repo->cherryPick({sideId.toStdString()});
        repo.reset();

        QCOMPARE(QString::fromStdString(created), headId());
        QCOMPARE(headSubject(), QStringLiteral("side work"));
        QCOMPARE(readFile(m_path + "/side.txt"), QByteArray("from the side\n"));
        // Author kept, committer is the one who ran the cherry-pick. The commit
        // *id* is not asserted: cherry-picking onto the same parent within the
        // same second legitimately produces the same oid.
        QCOMPARE(gitOut(m_path, {"log", "-1", "--format=%an"}), QStringLiteral("Side Author"));
        QCOMPARE(gitOut(m_path, {"log", "-1", "--format=%cn"}), QStringLiteral("Test"));
    }

    void resetSoftKeepsTheIndexAndMixedDoesNot() {
        QVERIFY(commitFile("a.txt", "one\n", "first"));
        QVERIFY(commitFile("a.txt", "two\n", "second"));
        const QString secondId = headId();
        const QString firstId = gitOut(m_path, {"rev-parse", "HEAD~1"});

        {
            auto repo = open();
            repo->resetTo(firstId.toStdString(), ResetMode::Soft);
        }
        QCOMPARE(headId(), firstId);
        QVERIFY(!git(m_path, {"diff", "--cached", "--quiet"}));
        QCOMPARE(readFile(m_path + "/a.txt"), QByteArray("two\n"));

        QVERIFY(git(m_path, {"reset", "--hard", secondId}));
        {
            auto repo = open();
            repo->resetTo(firstId.toStdString(), ResetMode::Mixed);
        }
        QCOMPARE(headId(), firstId);
        QVERIFY(git(m_path, {"diff", "--cached", "--quiet"}));
        QCOMPARE(readFile(m_path + "/a.txt"), QByteArray("two\n"));
    }

    void resetHardOverwritesTheWorkingTree() {
        QVERIFY(commitFile("a.txt", "one\n", "first"));
        QVERIFY(commitFile("a.txt", "two\n", "second"));
        const QString firstId = gitOut(m_path, {"rev-parse", "HEAD~1"});

        auto repo = open();
        repo->resetTo(firstId.toStdString(), ResetMode::Hard);
        repo.reset();

        QCOMPARE(headId(), firstId);
        QCOMPARE(readFile(m_path + "/a.txt"), QByteArray("one\n"));
    }

    void revertCreatesAnUndoingCommit() {
        QVERIFY(commitFile("a.txt", "one\n", "first"));
        QVERIFY(commitFile("a.txt", "two\n", "add two"));
        const QString addedId = headId();

        auto repo = open();
        const Oid reverted = repo->revertCommit(addedId.toStdString());
        repo.reset();

        QVERIFY(reverted != addedId.toStdString());
        // The file is back to "one" even though the bad commit is still in
        // history — that is what reverting means, as opposed to resetting.
        QCOMPARE(readFile(m_path + "/a.txt"), QByteArray("one\n"));
        QVERIFY(headSubject().startsWith(QStringLiteral("Revert")));
        QVERIFY(subjects().contains(QStringLiteral("add two")));
    }

    void createTagPointsAtTheCommit() {
        QVERIFY(commitFile("a.txt", "one\n", "first"));
        const QString head = headId();

        auto repo = open();
        repo->createTag("v1.0", head.toStdString(), "");
        repo.reset();

        QCOMPARE(gitOut(m_path, {"rev-parse", "v1.0^{commit}"}), head);
        QCOMPARE(gitOut(m_path, {"cat-file", "-t", "v1.0"}), QStringLiteral("commit"));
    }

    void annotatedTagCarriesAMessage() {
        QVERIFY(commitFile("a.txt", "one\n", "first"));

        auto repo = open();
        repo->createTag("v2.0", headId().toStdString(), "release two");
        repo.reset();

        QCOMPARE(gitOut(m_path, {"cat-file", "-t", "v2.0"}), QStringLiteral("tag"));
        // The default `git tag -l -n` column padding is not worth asserting;
        // that the message landed is.
        const QString listed = gitOut(m_path, {"tag", "-l", "-n1", "v2.0"});
        QVERIFY(listed.startsWith(QStringLiteral("v2.0")));
        QVERIFY(listed.contains(QStringLiteral("release two")));
    }

    void fastForwardMovesTheBranchOnlyWhenItIsSafe() {
        QVERIFY(commitFile("a.txt", "one\n", "first"));
        QVERIFY(git(m_path, {"checkout", "-q", "-b", "ahead"}));
        QVERIFY(commitFile("b.txt", "two\n", "second"));
        const QString aheadTip = headId();
        QVERIFY(git(m_path, {"checkout", "-q", "main"}));
        // Diverge on purpose: both branches now hold a commit the other lacks,
        // which is exactly when a "fast-forward" must be refused.
        QVERIFY(commitFile("c.txt", "three\n", "third"));
        const QString mainTip = headId();

        auto repo = open();
        QVERIFY(!repo->canFastForward("main", aheadTip.toStdString()));
        QVERIFY(!repo->canFastForward("ahead", mainTip.toStdString()));
        QVERIFY_THROWS_EXCEPTION(GitError,
                                repo->fastForwardBranch("ahead", mainTip.toStdString()));
        repo.reset();
        QCOMPARE(branchTip("ahead"), aheadTip);
        QCOMPARE(branchTip("main"), mainTip);

        const QString base = gitOut(m_path, {"rev-parse", "main~1"});
        QVERIFY(git(m_path, {"reset", "--hard", base}));
        auto repo2 = open();
        QVERIFY(repo2->canFastForward("main", aheadTip.toStdString()));
        repo2->fastForwardBranch("main", aheadTip.toStdString());
        QCOMPARE(branchTip("main"), aheadTip);
    }

    void formatPatchProducesAMailboxPatch() {
        QVERIFY(commitFile("a.txt", "one\n", "first"));
        QVERIFY(commitFile("b.txt", "two\n", "add b"));

        auto repo = open();
        const std::string patch = repo->formatPatch(headId().toStdString());
        repo.reset();

        QVERIFY2(patch.rfind("From ", 0) == 0, "patch must start with the From line");
        QVERIFY(patch.find("Subject: [PATCH] add b") != std::string::npos);
        QVERIFY(patch.find("+two") != std::string::npos);
    }
};

QTEST_MAIN(HistoryOperationsTest)
#include "test_history_ops.moc"
