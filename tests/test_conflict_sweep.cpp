#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>

using namespace gk;

namespace {

namespace fs = std::filesystem;

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

/// The automatic pass over a stopped rebase, against real repositories: the point
/// is not that the text comparison works in a unit test, it is that a rebase which
/// git reports as conflicted finishes on its own when nothing is actually contested.
class ConflictSweepTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void adjacentEditsDoNotStopARebase();
    void theSameLineChangedTwiceStillNeedsAPerson();
    void anAlreadyResolvedRebaseIsNotCommittedForTheReader();
    void theAutoPassSaysWhyAConflictNeededNobody();
    void theAutoPassIsSilentWhenAPersonIsNeeded();

private:
    bool commitFile(const QString& name, const QString& content, const QString& message);
    void buildAdjacentConflict();

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void ConflictSweepTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
}

void ConflictSweepTest::cleanup() {
    m_dir.reset();
}

bool ConflictSweepTest::commitFile(const QString& name, const QString& content,
                                   const QString& message) {
    if (!writeFile(m_path, name, content)) {
        return false;
    }
    if (!git(m_path, {"add", name})) {
        return false;
    }
    QStringList args = kIdentity;
    args << "commit" << "-q" << "-m" << message;
    return git(m_path, args);
}

/// A rebase where the two sides changed neighbouring lines: git calls it a
/// conflict, and it is resolved without a person every single time.
void ConflictSweepTest::buildAdjacentConflict() {
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("a\nb\nc\n"),
                       QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("a\nB\nc\n"),
                       QStringLiteral("feature changes the second line")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("a\nb\nC\n"),
                       QStringLiteral("main changes the third line")));
    QVERIFY(git(m_path, {"checkout", "-q", "feature"}));
}

void ConflictSweepTest::adjacentEditsDoNotStopARebase() {
    buildAdjacentConflict();

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    RebaseResult result;
    QVERIFY2(session.rebaseOnto(QStringLiteral("feature"), QStringLiteral("main"), &result, &error),
             qPrintable(error));
    QCOMPARE(result.status, RebaseStatus::Conflicted);

    const RepoSession::SweepOutcome sweep = session.sweepOperationConflicts();
    QVERIFY2(sweep.finished, qPrintable(sweep.error));
    QVERIFY(sweep.hunksResolved >= 1);
    QVERIFY(sweep.autoStaged.contains(QStringLiteral("f.txt")));
    QVERIFY(!session.rebaseInProgress());

    QCOMPARE(readFile(m_path, QStringLiteral("f.txt")), QStringLiteral("a\nB\nC\n"));
}

void ConflictSweepTest::theSameLineChangedTwiceStillNeedsAPerson() {
    // The same line, written two different ways: this is the one thing that is
    // genuinely a decision, and the pass must not make it.
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nhere\n"),
                       QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nfeature\n"),
                       QStringLiteral("feature rewrites the line")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nmain\n"),
                       QStringLiteral("main rewrites the line")));
    QVERIFY(git(m_path, {"checkout", "-q", "feature"}));

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    RebaseResult result;
    QVERIFY2(session.rebaseOnto(QStringLiteral("feature"), QStringLiteral("main"), &result, &error),
             qPrintable(error));
    QCOMPARE(result.status, RebaseStatus::Conflicted);

    const RepoSession::SweepOutcome sweep = session.sweepOperationConflicts();
    QVERIFY(!sweep.finished);
    QVERIFY(sweep.needsAPerson);
    QVERIFY(sweep.needsAPersonPaths.contains(QStringLiteral("f.txt")));
    QVERIFY(session.rebaseInProgress());

    const QString content = readFile(m_path, QStringLiteral("f.txt"));
    QVERIFY(content.contains(QStringLiteral("<<<<<<<")));
    QVERIFY(content.contains(QStringLiteral("feature")));
    QVERIFY(content.contains(QStringLiteral("main")));
}

void ConflictSweepTest::anAlreadyResolvedRebaseIsNotCommittedForTheReader() {
    buildAdjacentConflict();

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    RebaseResult result;
    QVERIFY2(session.rebaseOnto(QStringLiteral("feature"), QStringLiteral("main"), &result, &error),
             qPrintable(error));
    QCOMPARE(result.status, RebaseStatus::Conflicted);

    QVERIFY(session.resolveConflict(QStringLiteral("f.txt"), QStringLiteral("a\nB\nC\n"), &error));

    const RepoSession::SweepOutcome sweep = session.sweepOperationConflicts();
    QVERIFY(!sweep.finished);
    QVERIFY(!sweep.needsAPerson);
    QVERIFY2(session.rebaseInProgress(),
             "finishing an operation a person already resolved is not the sweep's call");
}

void ConflictSweepTest::theAutoPassSaysWhyAConflictNeededNobody() {
    buildAdjacentConflict();

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    RebaseResult result;
    QVERIFY2(session.rebaseOnto(QStringLiteral("feature"), QStringLiteral("main"), &result, &error),
             qPrintable(error));
    QCOMPARE(result.status, RebaseStatus::Conflicted);

    const RepoSession::AutoResolveOutcome pass = session.autoResolveTrivialConflicts();
    QVERIFY(pass.stagedFiles.contains(QStringLiteral("f.txt")));
    QVERIFY(pass.needsAPerson.isEmpty());
    QCOMPARE(pass.resolvedDetail.size(), 1);
    QVERIFY2(pass.resolvedDetail.first().startsWith(QStringLiteral("f.txt:")),
             qPrintable(pass.resolvedDetail.first()));
    QVERIFY2(!pass.resolvedDetail.first().mid(QStringLiteral("f.txt:").size()).trimmed().isEmpty(),
             qPrintable(pass.resolvedDetail.first()));
}

void ConflictSweepTest::theAutoPassIsSilentWhenAPersonIsNeeded() {
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nhere\n"),
                       QStringLiteral("base")));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nfeature\n"),
                       QStringLiteral("feature rewrites the line")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(commitFile(QStringLiteral("f.txt"), QStringLiteral("keep\nmain\n"),
                       QStringLiteral("main rewrites the line")));
    QVERIFY(git(m_path, {"checkout", "-q", "feature"}));

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    RebaseResult result;
    QVERIFY2(session.rebaseOnto(QStringLiteral("feature"), QStringLiteral("main"), &result, &error),
             qPrintable(error));

    const RepoSession::AutoResolveOutcome pass = session.autoResolveTrivialConflicts();
    QVERIFY(pass.needsAPerson.contains(QStringLiteral("f.txt")));
    QVERIFY(pass.resolvedDetail.isEmpty());
}

QTEST_MAIN(ConflictSweepTest)
#include "test_conflict_sweep.moc"
