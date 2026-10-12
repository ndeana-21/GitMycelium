/// Interactive rebase: reorder, drop, squash, fixup, reword and edit, driven by a plan
/// through the scripted `git rebase -i` the product uses. Where an ordering is involved
/// the oracle is the git binary itself: the same plan is replayed on a copy of the
/// repository with `GIT_SEQUENCE_EDITOR`, and the resulting tree and log must match.

#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <memory>

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
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

bool writeScript(const QString& path, const QString& body) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(body.toUtf8());
    file.close();
    return QFile::setPermissions(path,
                                 QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

QString todoFor(const std::vector<RewriteStep>& steps) {
    QString out;
    for (const RewriteStep& step : steps) {
        out += QStringLiteral("pick %1\n").arg(QString::fromStdString(step.commit));
    }
    return out;
}

} // namespace

class InteractiveRebaseTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void candidatesAreTheTodoOldestFirst();
    void aReorderMatchesTheGitBinary();
    void droppingAMiddleCommitRemovesItsChange();
    void squashCombinesMessagesAndFixupKeepsTheFirst();
    void rewordReplacesTheMessage();
    void anEditStepStopsTheRebase();
    void aPlanThatDoesNotMatchTheRangeIsRefused();
    void rebaseingAnotherBranchOntoATargetMatchesTheGitBinary();
    void anEditStepIsAmendedAndContinued();
    void aRepositoryPathWithShellCharactersStillRewrites();
    void rootCandidatesCoverTheWholeHistory();
    void squashingIntoTheRootMatchesTheGitBinary();
    void droppingTheRootMakesItsChildTheNewRoot();
    void rangeIsLinearFromTheRootRejectsAMerge();

private:
    bool build(int count);
    QString headSubject() const;
    QString treeOf(const QString& rev) const;
    std::vector<RewriteStep> planFrom(const std::vector<RebaseCandidate>& candidates) const;
    /// Runs the same plan with `git rebase -i` in `dir`, so the engine can be compared
    /// with the binary. `onto`/`branch` reproduce the `--onto` form. Returns false when
    /// git refused.
    bool rewriteWithBinary(const QString& dir, const QString& base,
                           const std::vector<RewriteStep>& steps, const QString& onto = {},
                           const QString& branch = {});

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void InteractiveRebaseTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
}

void InteractiveRebaseTest::cleanup() {
    m_dir.reset();
}

bool InteractiveRebaseTest::build(int count) {
    for (int i = 1; i <= count; ++i) {
        if (!writeFile(m_path, QStringLiteral("f%1.txt").arg(i),
                       QStringLiteral("%1\n").arg(i))) {
            return false;
        }
        if (!git(m_path, {"add", "-A"})) {
            return false;
        }
        if (!git(m_path, {"commit", "-q", "-m",
                          QStringLiteral("c%1").arg(i)})) {
            return false;
        }
    }
    return true;
}

QString InteractiveRebaseTest::headSubject() const {
    return gitOut(m_path, {"log", "-1", "--format=%s"}).trimmed();
}

QString InteractiveRebaseTest::treeOf(const QString& rev) const {
    return gitOut(m_path, {"rev-parse", rev + QStringLiteral("^{tree}")}).trimmed();
}

std::vector<RewriteStep> InteractiveRebaseTest::planFrom(
    const std::vector<RebaseCandidate>& candidates) const {
    std::vector<RewriteStep> steps;
    steps.reserve(candidates.size());
    for (const RebaseCandidate& candidate : candidates) {
        RewriteStep step;
        step.commit = candidate.id;
        step.action = RewriteAction::Keep;
        steps.push_back(step);
    }
    return steps;
}

bool InteractiveRebaseTest::rewriteWithBinary(const QString& dir, const QString& base,
                                              const std::vector<RewriteStep>& steps,
                                              const QString& onto, const QString& branch) {
    const QString script = QDir(dir).filePath(QStringLiteral("oracle-seq-editor.sh"));
    const QString body = QStringLiteral("#!/bin/sh\nprintf '%1' > \"$1\"\n")
                             .arg(todoFor(steps));
    if (!writeScript(script, body)) {
        return false;
    }
    QStringList args{"-c", "sequence.editor=" + script, "-c", "core.editor=true", "rebase",
                     "-i"};
    if (!onto.isEmpty()) {
        args << "--onto" << onto;
    }
    if (base == QString::fromLatin1(kRebaseRoot)) {
        args << "--root";
    } else {
        args << base;
    }
    if (!branch.isEmpty()) {
        args << branch;
    }
    return git(dir, args);
}

void InteractiveRebaseTest::candidatesAreTheTodoOldestFirst() {
    QVERIFY(build(4));
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const QString base = gitOut(m_path, {"rev-parse", "HEAD~3"}).trimmed();
    const std::vector<RebaseCandidate> candidates =
        session.rebaseCandidates(base, QStringLiteral("HEAD"));

    // c2, c3, c4 oldest first — exactly what git's todo lists.
    QCOMPARE(candidates.size(), std::size_t(3));
    QCOMPARE(QString::fromStdString(candidates.front().subject), QStringLiteral("c2"));
    QCOMPARE(QString::fromStdString(candidates.back().subject), QStringLiteral("c4"));
    const QStringList fromGit = gitOut(m_path, {"rev-list", "--reverse", "--no-merges",
                                                base + "..HEAD"})
                                    .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(candidates.size(), static_cast<std::size_t>(fromGit.size()));
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        QCOMPARE(QString::fromStdString(candidates[i].id), fromGit.at(static_cast<int>(i)));
    }
}

void InteractiveRebaseTest::aReorderMatchesTheGitBinary() {
    QVERIFY(build(4));
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const QString base = gitOut(m_path, {"rev-parse", "HEAD~3"}).trimmed();

    const std::vector<RebaseCandidate> candidates =
        session.rebaseCandidates(base, QStringLiteral("HEAD"));
    std::vector<RewriteStep> steps = planFrom(candidates);
    std::swap(steps[1], steps[2]);

    // The oracle first, in a copy, so the engine's run cannot disturb its input.
    const QString clone = m_dir->path() + QStringLiteral("-oracle");
    QVERIFY(git(m_dir->path(), {"clone", "-q", m_path, clone}));
    QVERIFY(git(clone, {"config", "user.name", "Test"}));
    QVERIFY(git(clone, {"config", "user.email", "test@localhost"}));
    QVERIFY(rewriteWithBinary(clone, base, steps));
    const QString oracleTree = gitOut(clone, {"rev-parse", "HEAD^{tree}"}).trimmed();
    const QString oracleLog = gitOut(clone, {"log", "--format=%s", "--no-merges"});

    QVERIFY2(session.rewriteHistory(base, steps, &error), qPrintable(error));
    QVERIFY(!session.rebaseInProgress());
    QCOMPARE(treeOf(QStringLiteral("HEAD")), oracleTree);
    QCOMPARE(gitOut(m_path, {"log", "--format=%s", "--no-merges"}), oracleLog);
    QVERIFY(headSubject() == QStringLiteral("c3"));
}

void InteractiveRebaseTest::droppingAMiddleCommitRemovesItsChange() {
    QVERIFY(build(4));
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const QString base = gitOut(m_path, {"rev-parse", "HEAD~3"}).trimmed();

    const std::vector<RebaseCandidate> candidates =
        session.rebaseCandidates(base, QStringLiteral("HEAD"));
    std::vector<RewriteStep> steps = planFrom(candidates);
    steps[1].action = RewriteAction::Drop;

    QVERIFY2(session.rewriteHistory(base, steps, &error), qPrintable(error));
    QVERIFY(!session.rebaseInProgress());
    QVERIFY(gitOut(m_path, {"ls-tree", "--name-only", "HEAD"}).contains(QStringLiteral("f2.txt")));
    QVERIFY(gitOut(m_path, {"ls-tree", "--name-only", "HEAD"}).contains(QStringLiteral("f4.txt")));
    QVERIFY(!gitOut(m_path, {"ls-tree", "--name-only", "HEAD"}).contains(QStringLiteral("f3.txt")));
    QCOMPARE(gitOut(m_path, {"log", "--format=%s", "--no-merges"}).trimmed().split(
                 QLatin1Char('\n')),
             QStringList({QStringLiteral("c4"), QStringLiteral("c2"), QStringLiteral("c1")}));
}

void InteractiveRebaseTest::squashCombinesMessagesAndFixupKeepsTheFirst() {
    {
        QVERIFY(build(3));
        RepoSession session;
        QString error;
        QVERIFY(session.open(m_path, &error));
        const QString base = gitOut(m_path, {"rev-parse", "HEAD~2"}).trimmed();
        std::vector<RewriteStep> steps = planFrom(session.rebaseCandidates(base, QStringLiteral("HEAD")));
        steps[1].action = RewriteAction::Squash;

        QVERIFY2(session.rewriteHistory(base, steps, &error), qPrintable(error));
        QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("main"));
        const QString log = gitOut(m_path, {"log", "--format=%B", "--no-merges"}).trimmed();
        QVERIFY(log.contains(QStringLiteral("c2")));
        QVERIFY(log.contains(QStringLiteral("c3")));
        QCOMPARE(gitOut(m_path, {"rev-list", "--count", "HEAD"}).trimmed(),
                 QStringLiteral("2"));
        // The squash kept both files: it only joined the commits.
        QVERIFY(gitOut(m_path, {"ls-tree", "--name-only", "HEAD"}).contains(QStringLiteral("f3.txt")));
    }
    cleanup();
    init();
    {
        QVERIFY(build(3));
        RepoSession session;
        QString error;
        QVERIFY(session.open(m_path, &error));
        const QString base = gitOut(m_path, {"rev-parse", "HEAD~2"}).trimmed();
        std::vector<RewriteStep> steps = planFrom(session.rebaseCandidates(base, QStringLiteral("HEAD")));
        steps[1].action = RewriteAction::Fixup;

        QVERIFY2(session.rewriteHistory(base, steps, &error), qPrintable(error));
        const QString log = gitOut(m_path, {"log", "--format=%B", "--no-merges"}).trimmed();
        QVERIFY(log.contains(QStringLiteral("c2")));
        QVERIFY(!log.contains(QStringLiteral("c3")));
    }
}

void InteractiveRebaseTest::rewordReplacesTheMessage() {
    QVERIFY(build(3));
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const QString base = gitOut(m_path, {"rev-parse", "HEAD~2"}).trimmed();
    const QString tree = treeOf(QStringLiteral("HEAD"));

    std::vector<RewriteStep> steps = planFrom(session.rebaseCandidates(base, QStringLiteral("HEAD")));
    steps[1].action = RewriteAction::Reword;
    steps[1].message = "c3, said differently";

    QVERIFY2(session.rewriteHistory(base, steps, &error), qPrintable(error));
    QCOMPARE(headSubject(), QStringLiteral("c3, said differently"));
    // The content and the parent did not change; git am-like, the id may because the
    // committer is stamped to now.
    QCOMPARE(treeOf(QStringLiteral("HEAD")), tree);
    QCOMPARE(gitOut(m_path, {"rev-list", "--count", "HEAD"}).trimmed(), QStringLiteral("3"));
}

void InteractiveRebaseTest::anEditStepStopsTheRebase() {
    QVERIFY(build(3));
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const QString base = gitOut(m_path, {"rev-parse", "HEAD~2"}).trimmed();

    std::vector<RewriteStep> steps = planFrom(session.rebaseCandidates(base, QStringLiteral("HEAD")));
    steps[0].action = RewriteAction::Edit;

    QVERIFY2(session.rewriteHistory(base, steps, &error), qPrintable(error));
    // Stopped, not conflicted: the reader is meant to amend and continue.
    QVERIFY(session.rebaseInProgress());
    QVERIFY(session.conflicts().empty());
    QCOMPARE(headSubject(), QStringLiteral("c2"));

    session.rebaseAbort(&error);
    QVERIFY(!session.rebaseInProgress());
}

void InteractiveRebaseTest::aPlanThatDoesNotMatchTheRangeIsRefused() {
    QVERIFY(build(4));
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const QString base = gitOut(m_path, {"rev-parse", "HEAD~3"}).trimmed();
    const std::vector<RebaseCandidate> candidates =
        session.rebaseCandidates(base, QStringLiteral("HEAD"));

    // One commit short: git would silently drop it, so the engine refuses.
    std::vector<RewriteStep> shortPlan = planFrom(candidates);
    shortPlan.pop_back();
    QVERIFY(!session.rewriteHistory(base, shortPlan, &error));
    QVERIFY(error.contains(QStringLiteral("holds")));

    std::vector<RewriteStep> foreign = planFrom(candidates);
    foreign[0].commit = gitOut(m_path, {"rev-parse", "HEAD"}).trimmed().toStdString();
    QVERIFY(!session.rewriteHistory(base, foreign, &error));
    QVERIFY(error.contains(QStringLiteral("outside the range")));

    QVERIFY(!session.rebaseInProgress());
    QCOMPARE(gitOut(m_path, {"rev-list", "--count", "HEAD"}).trimmed(), QStringLiteral("4"));
}

void InteractiveRebaseTest::rebaseingAnotherBranchOntoATargetMatchesTheGitBinary() {
    // main: c1. feature from c1: f1, f2. main then advances, so the two share only c1.
    QVERIFY(build(1));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(writeFile(m_path, QStringLiteral("feature1.txt"), QStringLiteral("x\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-q", "-m", "f1"}));
    QVERIFY(writeFile(m_path, QStringLiteral("feature2.txt"), QStringLiteral("y\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-q", "-m", "f2"}));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(m_path, QStringLiteral("main2.txt"), QStringLiteral("z\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-q", "-m", "m2"}));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const QString base = session.mergeBase(QStringLiteral("feature"), QStringLiteral("main"));
    QCOMPARE(base, gitOut(m_path, {"merge-base", "feature", "main"}).trimmed());

    const std::vector<RebaseCandidate> candidates =
        session.rebaseCandidates(base, QStringLiteral("feature"));
    QCOMPARE(candidates.size(), std::size_t(2));
    std::vector<RewriteStep> steps = planFrom(candidates);
    std::swap(steps[0], steps[1]); // f2, then f1

    const QString clone = m_dir->path() + QStringLiteral("-oracle");
    QVERIFY(git(m_dir->path(), {"clone", "-q", m_path, clone}));
    QVERIFY(git(clone, {"config", "user.name", "Test"}));
    QVERIFY(git(clone, {"config", "user.email", "test@localhost"}));
    // A clone only makes the default branch local; the rebase needs `feature` to exist.
    QVERIFY(git(clone, {"branch", "-q", "feature", "origin/feature"}));
    QVERIFY(rewriteWithBinary(clone, base, steps, QStringLiteral("main"),
                              QStringLiteral("feature")));
    const QString oracleTree = gitOut(clone, {"rev-parse", "feature^{tree}"}).trimmed();
    const QString oracleLog = gitOut(clone, {"log", "--format=%s", "feature"});
    const QString oracleCount = gitOut(clone, {"rev-list", "--count", "feature"}).trimmed();

    QVERIFY2(session.rewriteHistory(base, steps, &error, QStringLiteral("main"),
                                    QStringLiteral("feature")),
             qPrintable(error));
    QVERIFY(!session.rebaseInProgress());
    // The branch was checked out by the rebase and now sits on main.
    QCOMPARE(QString::fromStdString(session.info().headBranch), QStringLiteral("feature"));
    QCOMPARE(gitOut(m_path, {"rev-parse", "feature^{tree}"}).trimmed(), oracleTree);
    QCOMPARE(gitOut(m_path, {"log", "--format=%s", "feature"}), oracleLog);
    QCOMPARE(gitOut(m_path, {"rev-list", "--count", "feature"}).trimmed(), oracleCount);
}

void InteractiveRebaseTest::anEditStepIsAmendedAndContinued() {
    QVERIFY(build(3));
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const QString base = gitOut(m_path, {"rev-parse", "HEAD~2"}).trimmed();

    // Amending needs an open rebase; there is none yet.
    QVERIFY(!session.amendStoppedCommit(QString(), &error));

    std::vector<RewriteStep> steps = planFrom(session.rebaseCandidates(base, QStringLiteral("HEAD")));
    steps[0].action = RewriteAction::Edit;
    QVERIFY2(session.rewriteHistory(base, steps, &error), qPrintable(error));
    QVERIFY(session.rebaseInProgress());
    QVERIFY(session.conflicts().empty());
    QCOMPARE(headSubject(), QStringLiteral("c2"));

    // Stage a change and amend the stopped commit, keeping its message.
    QVERIFY(writeFile(m_path, QStringLiteral("amended.txt"), QStringLiteral("new\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY2(session.amendStoppedCommit(QString(), &error), qPrintable(error));
    QCOMPARE(headSubject(), QStringLiteral("c2"));
    QVERIFY(gitOut(m_path, {"ls-tree", "--name-only", "HEAD"}).contains(QStringLiteral("amended.txt")));

    // Continue: the rest replays on top of the amended commit.
    RebaseResult result;
    QVERIFY2(session.rebaseContinue(&result, &error), qPrintable(error));
    QVERIFY(!session.rebaseInProgress());
    QCOMPARE(headSubject(), QStringLiteral("c3"));
    QCOMPARE(gitOut(m_path, {"rev-list", "--count", "HEAD"}).trimmed(), QStringLiteral("3"));
    QVERIFY(gitOut(m_path, {"ls-tree", "--name-only", "HEAD~1"}).contains(QStringLiteral("amended.txt")));
}

void InteractiveRebaseTest::aRepositoryPathWithShellCharactersStillRewrites() {
    // git runs the editors and the todo's `exec` lines through the shell; an unquoted
    // path split at the space and the rewrite failed before it started.
    m_path = m_dir->path() + QStringLiteral("/my repo $HOME 'q\"");
    QVERIFY(QDir().mkpath(m_path));
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    QVERIFY(build(3));

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));
    const QString base = gitOut(m_path, {"rev-parse", "HEAD~2"}).trimmed();
    std::vector<RewriteStep> steps =
        planFrom(session.rebaseCandidates(base, QStringLiteral("HEAD")));
    std::swap(steps[0], steps[1]);
    steps[1].action = RewriteAction::Reword;
    steps[1].message = "c2, said differently";

    QVERIFY2(session.rewriteHistory(base, steps, &error), qPrintable(error));
    QCOMPARE(gitOut(m_path, {"log", "--format=%s", "-3"}).trimmed(),
             QStringLiteral("c2, said differently\nc3\nc1"));
}

void InteractiveRebaseTest::rootCandidatesCoverTheWholeHistory() {
    QVERIFY(build(3));
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const QString root = QString::fromLatin1(kRebaseRoot);
    const std::vector<RebaseCandidate> candidates =
        session.rebaseCandidates(root, QStringLiteral("HEAD"));
    QCOMPARE(candidates.size(), std::size_t(3));
    QCOMPARE(QString::fromStdString(candidates.front().subject), QStringLiteral("c1"));
    QCOMPARE(QString::fromStdString(candidates.back().subject), QStringLiteral("c3"));

    const QStringList fromGit =
        gitOut(m_path, {"rev-list", "--reverse", "--no-merges", "HEAD"})
            .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(candidates.size(), static_cast<std::size_t>(fromGit.size()));
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        QCOMPARE(QString::fromStdString(candidates[i].id), fromGit.at(static_cast<int>(i)));
    }
}

void InteractiveRebaseTest::squashingIntoTheRootMatchesTheGitBinary() {
    QVERIFY(build(3));
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const QString treeBefore = treeOf(QStringLiteral("HEAD"));

    const QString root = QString::fromLatin1(kRebaseRoot);
    std::vector<RewriteStep> steps =
        planFrom(session.rebaseCandidates(root, QStringLiteral("HEAD")));
    steps[1].action = RewriteAction::Squash;

    QVERIFY2(session.rewriteHistory(root, steps, &error), qPrintable(error));
    QVERIFY(!session.rebaseInProgress());
    QCOMPARE(gitOut(m_path, {"rev-list", "--count", "HEAD"}).trimmed(), QStringLiteral("2"));
    QCOMPARE(gitOut(m_path, {"log", "--format=%s", "--no-merges"}).trimmed(),
             QStringLiteral("c3\nc1"));
    const QString body = gitOut(m_path, {"log", "-1", "--format=%B", "HEAD~1"});
    QVERIFY(body.contains(QStringLiteral("c1")));
    QVERIFY(body.contains(QStringLiteral("c2")));
    QCOMPARE(treeOf(QStringLiteral("HEAD")), treeBefore);
}

void InteractiveRebaseTest::droppingTheRootMakesItsChildTheNewRoot() {
    QVERIFY(build(3));
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const QString root = gitOut(m_path, {"rev-list", "--max-parents=0", "HEAD"}).trimmed();

    QVERIFY2(session.dropCommit(root, &error), qPrintable(error));
    QVERIFY(!session.rebaseInProgress());
    QCOMPARE(gitOut(m_path, {"log", "--format=%s", "--no-merges"}).trimmed().split(
                 QLatin1Char('\n')),
             QStringList({QStringLiteral("c3"), QStringLiteral("c2")}));
    QVERIFY(!gitOut(m_path, {"ls-tree", "--name-only", "HEAD"}).contains(QStringLiteral("f1.txt")));
    QCOMPARE(gitOut(m_path, {"rev-list", "--max-parents=0", "HEAD"}).trimmed(),
             gitOut(m_path, {"rev-parse", "HEAD~1"}).trimmed());
}

void InteractiveRebaseTest::rangeIsLinearFromTheRootRejectsAMerge() {
    QVERIFY(build(3));
    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    const QString root = QString::fromLatin1(kRebaseRoot);
    QVERIFY(session.rangeIsLinear(root, QString::fromStdString(session.info().headId), &error));

    QVERIFY(git(m_path, {"checkout", "-q", "-b", "side", "HEAD~1"}));
    QVERIFY(writeFile(m_path, QStringLiteral("side.txt"), QStringLiteral("s\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-q", "-m", "side"}));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(m_path, QStringLiteral("main.txt"), QStringLiteral("m\n")));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-q", "-m", "m2"}));
    QVERIFY(git(m_path, {"merge", "-q", "--no-ff", "-m", "merge", "side"}));

    const QString merged = gitOut(m_path, {"rev-parse", "HEAD"}).trimmed();
    QVERIFY(!session.rangeIsLinear(root, merged, &error));
}

QTEST_MAIN(InteractiveRebaseTest)
#include "test_interactive_rebase.moc"