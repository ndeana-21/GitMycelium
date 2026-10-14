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

class ConflictPreventionTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void theBaseIsTheRemoteHeadOfAClone();
    void aDivergedBaseIsReportedAsAConflict();
    void anUpToDateBranchHasNoConflicts();
    void aDetachedHeadHasNoBase();

private:
    void buildDiverged();
    void commitOn(const QString& branch, const QString& content, const QString& message);

    std::unique_ptr<QTemporaryDir> m_root;
    QString m_bare;
    QString m_work;
};

void ConflictPreventionTest::init() {
    m_root = std::make_unique<QTemporaryDir>();
    QVERIFY(m_root->isValid());
    m_bare = QDir(m_root->path()).filePath(QStringLiteral("origin.git"));
    m_work = QDir(m_root->path()).filePath(QStringLiteral("work"));
    QVERIFY(git(m_root->path(), {"init", "-q", "--bare", "-b", "main", m_bare}));
    QVERIFY(git(m_root->path(), {"clone", "-q", m_bare, m_work}));
    QVERIFY(git(m_work, {"config", "user.name", "Test"}));
    QVERIFY(git(m_work, {"config", "user.email", "test@localhost"}));
}

void ConflictPreventionTest::cleanup() {
    m_root.reset();
}

void ConflictPreventionTest::commitOn(const QString& branch, const QString& content,
                                      const QString& message) {
    if (!branch.isEmpty()) {
        QVERIFY(git(m_work, {"checkout", "-q", branch}));
    }
    QVERIFY(writeFile(m_work, QStringLiteral("f.txt"), content));
    QVERIFY(git(m_work, {"add", "f.txt"}));
    QVERIFY(git(m_work, kIdentity + QStringList{"commit", "-q", "-m", message}));
}

/// origin/main advances on the same line the feature branch changed, so merging the
/// base into the feature would conflict.
void ConflictPreventionTest::buildDiverged() {
    commitOn(QString(), QStringLiteral("a\nb\nc\n"), QStringLiteral("base"));
    QVERIFY(git(m_work, {"push", "-q", "-u", "origin", "main"}));

    QVERIFY(git(m_work, {"checkout", "-q", "-b", "feature"}));
    commitOn(QString(), QStringLiteral("a\nFEATURE\nc\n"), QStringLiteral("feature"));

    commitOn(QStringLiteral("main"), QStringLiteral("a\nMAINLINE\nc\n"), QStringLiteral("mainline"));
    QVERIFY(git(m_work, {"push", "-q", "origin", "main"}));

    QVERIFY(git(m_work, {"checkout", "-q", "feature"}));
    QVERIFY(git(m_work, {"fetch", "-q", "origin"}));
}

void ConflictPreventionTest::theBaseIsTheRemoteHeadOfAClone() {
    buildDiverged();

    RepoSession session;
    QVERIFY(session.open(m_work));
    QCOMPARE(session.baseBranchForCurrentBranch(), QStringLiteral("origin/main"));
}

void ConflictPreventionTest::aDivergedBaseIsReportedAsAConflict() {
    buildDiverged();

    RepoSession session;
    QVERIFY(session.open(m_work));

    QString base;
    QString error;
    const MergePreview preview = session.previewAgainstBase(&base, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(base, QStringLiteral("origin/main"));
    QVERIFY(preview.available);
    QVERIFY(!preview.clean);
    QCOMPARE(preview.conflictedPaths.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(preview.conflictedPaths.front()), QStringLiteral("f.txt"));

    QCOMPARE(session.conflicts().size(), std::size_t(0));
}

void ConflictPreventionTest::anUpToDateBranchHasNoConflicts() {
    buildDiverged();

    RepoSession session;
    QVERIFY(session.open(m_work));
    QVERIFY(git(m_work, {"checkout", "-q", "main"}));

    QString base;
    QString error;
    const MergePreview preview = session.previewAgainstBase(&base, &error);
    QCOMPARE(base, QStringLiteral("origin/main"));
    QVERIFY(preview.clean);
    QVERIFY(preview.conflictedPaths.empty());
}

void ConflictPreventionTest::aDetachedHeadHasNoBase() {
    buildDiverged();

    RepoSession session;
    QVERIFY(session.open(m_work));
    QVERIFY(git(m_work, {"checkout", "-q", "--detach", "HEAD"}));

    // Nothing to compare against: inventing a base would warn about the wrong branch.
    QVERIFY(session.baseBranchForCurrentBranch().isEmpty());
    QString base;
    QString error;
    const MergePreview preview = session.previewAgainstBase(&base, &error);
    QVERIFY(base.isEmpty());
    QVERIFY(!preview.conflictedPaths.size());
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(ConflictPreventionTest)
#include "test_conflict_prevention.moc"
