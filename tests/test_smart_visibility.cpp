#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"
#include "ui/main_window.h"

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

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

QStringList nora() {
    return {QStringLiteral("-c"), QStringLiteral("user.name=Nora"), QStringLiteral("-c"),
            QStringLiteral("user.email=nora@example.com")};
}

const RefMeta* findMeta(const std::vector<RefMeta>& list, const QString& shortName) {
    for (const RefMeta& meta : list) {
        if (QString::fromStdString(meta.shortName) == shortName) {
            return &meta;
        }
    }
    return nullptr;
}

QString report(MainWindow& window, const QString& operation) {
    return window.visibilityForDiagnostics(operation, QString()).join(QLatin1Char('\n'));
}

} // namespace

class SmartVisibilityTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void theTargetIsTheLocalDefaultBranch();
    void aRepositoryWithoutARemoteFallsBackToMain();
    void theTargetIsEmptyWhenNothingNamesIt();
    void refMetaCarriesAuthorUpstreamAndDirection();

    void smartKeepsHeadTargetAndUpstreams();
    void mineKeepsOnlyMyBranches();
    void prefixMatchesBranchNames();
    void recentDropsOldTips();
    void withoutUpstreamKeepsUntrackedBranches();
    void aheadKeepsBranchesToPush();
    void behindKeepsBranchesToPull();
    void clearingDropsEveryMode();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_work;
    QString m_origin;
};

void SmartVisibilityTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_work = QDir(m_dir->path()).filePath(QStringLiteral("work"));
    m_origin = QDir(m_dir->path()).filePath(QStringLiteral("origin.git"));

    QVERIFY(git(m_dir->path(), {"init", "-q", "--bare", "-b", "main", m_origin}));
    QVERIFY(QDir().mkpath(m_work));
    QVERIFY(git(m_work, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_work, {"config", "user.name", "Nora"}));
    QVERIFY(git(m_work, {"config", "user.email", "nora@example.com"}));
    QVERIFY(writeFile(m_work, "base.txt", "base\n"));
    QVERIFY(git(m_work, {"add", "-A"}));
    QVERIFY(git(m_work, nora() + QStringList{"commit", "-q", "-m", "base"}));
    QVERIFY(git(m_work, {"remote", "add", "origin", m_origin}));
    QVERIFY(git(m_work, {"push", "-q", "-u", "origin", "main"}));
    QVERIFY(git(m_work, {"remote", "set-head", "origin", "-a"}));

    // A branch of mine that tracks a remote, and one of Bob's that tracks nothing.
    QVERIFY(git(m_work, {"checkout", "-q", "-b", "feature/nora"}));
    QVERIFY(writeFile(m_work, "n.txt", "n\n"));
    QVERIFY(git(m_work, {"add", "-A"}));
    QVERIFY(git(m_work, nora() + QStringList{"commit", "-q", "-m", "nora"}));
    QVERIFY(git(m_work, {"push", "-q", "-u", "origin", "feature/nora"}));

    QVERIFY(git(m_work, {"checkout", "-q", "-b", "feature/bob", "main"}));
    QVERIFY(writeFile(m_work, "b.txt", "b\n"));
    QVERIFY(git(m_work, {"add", "-A"}));
    QVERIFY(git(m_work, {"-c", "user.name=Bob", "-c", "user.email=bob@example.com", "commit",
                         "-q", "-m", "bob"}));

    // An old branch of mine, so a 30-day window has something to exclude.
    QVERIFY(git(m_work, {"checkout", "-q", "-b", "ancient", "main"}));
    QVERIFY(writeFile(m_work, "old.txt", "old\n"));
    QVERIFY(git(m_work, {"add", "-A"}));
    qputenv("GIT_AUTHOR_DATE", "2020-01-01T00:00:00");
    qputenv("GIT_COMMITTER_DATE", "2020-01-01T00:00:00");
    QVERIFY(git(m_work, nora() + QStringList{"commit", "-q", "-m", "ancient"}));
    qunsetenv("GIT_AUTHOR_DATE");
    qunsetenv("GIT_COMMITTER_DATE");

    // `behind`: pushed, then its upstream is moved forward by pushing main over it.
    QVERIFY(git(m_work, {"checkout", "-q", "-b", "behind", "main"}));
    QVERIFY(writeFile(m_work, "h.txt", "h\n"));
    QVERIFY(git(m_work, {"add", "-A"}));
    QVERIFY(git(m_work, nora() + QStringList{"commit", "-q", "-m", "h"}));
    QVERIFY(git(m_work, {"push", "-q", "-u", "origin", "behind"}));
    QVERIFY(git(m_work, {"checkout", "-q", "main"}));
    QVERIFY(git(m_work, {"merge", "-q", "--ff-only", "behind"}));
    QVERIFY(writeFile(m_work, "m2.txt", "m2\n"));
    QVERIFY(git(m_work, {"add", "-A"}));
    QVERIFY(git(m_work, nora() + QStringList{"commit", "-q", "-m", "main2"}));
    QVERIFY(git(m_work, {"push", "-q", "origin", "main"}));
    QVERIFY(git(m_work, {"push", "-q", "origin", "main:behind"}));
    QVERIFY(git(m_work, {"fetch", "-q", "origin"}));

    // `local-ahead`: pushed, then one more commit that is not.
    QVERIFY(git(m_work, {"checkout", "-q", "-b", "local-ahead", "main"}));
    QVERIFY(writeFile(m_work, "p.txt", "p\n"));
    QVERIFY(git(m_work, {"add", "-A"}));
    QVERIFY(git(m_work, nora() + QStringList{"commit", "-q", "-m", "p1"}));
    QVERIFY(git(m_work, {"push", "-q", "-u", "origin", "local-ahead"}));
    QVERIFY(writeFile(m_work, "p2.txt", "p2\n"));
    QVERIFY(git(m_work, {"add", "-A"}));
    QVERIFY(git(m_work, nora() + QStringList{"commit", "-q", "-m", "p2"}));

    QVERIFY(git(m_work, {"checkout", "-q", "feature/nora"}));
}

void SmartVisibilityTest::cleanup() {
    m_dir.reset();
}

void SmartVisibilityTest::theTargetIsTheLocalDefaultBranch() {
    RepoSession session;
    QVERIFY(session.open(m_work));
    // origin/HEAD points at origin/main, and a local main exists: the local one is
    // the branch the reader types into the box.
    QCOMPARE(session.targetBranch(), QStringLiteral("refs/heads/main"));
}

void SmartVisibilityTest::aRepositoryWithoutARemoteFallsBackToMain() {
    const QString plain = QDir(m_dir->path()).filePath(QStringLiteral("plain"));
    QVERIFY(QDir().mkpath(plain));
    QVERIFY(git(plain, {"init", "-q", "-b", "main"}));
    QVERIFY(writeFile(plain, "x.txt", "x\n"));
    QVERIFY(git(plain, {"add", "-A"}));
    QVERIFY(git(plain, nora() + QStringList{"commit", "-q", "-m", "x"}));

    RepoSession session;
    QVERIFY(session.open(plain));
    QCOMPARE(session.targetBranch(), QStringLiteral("refs/heads/main"));
}

void SmartVisibilityTest::theTargetIsEmptyWhenNothingNamesIt() {
    const QString odd = QDir(m_dir->path()).filePath(QStringLiteral("odd"));
    QVERIFY(QDir().mkpath(odd));
    QVERIFY(git(odd, {"init", "-q", "-b", "develop"}));
    QVERIFY(writeFile(odd, "x.txt", "x\n"));
    QVERIFY(git(odd, {"add", "-A"}));
    QVERIFY(git(odd, nora() + QStringList{"commit", "-q", "-m", "x"}));

    RepoSession session;
    QVERIFY(session.open(odd));
    QVERIFY(session.targetBranch().isEmpty());
}

void SmartVisibilityTest::refMetaCarriesAuthorUpstreamAndDirection() {
    RepoSession session;
    QVERIFY(session.open(m_work));
    const std::vector<RefMeta> meta = session.refMeta();
    QVERIFY(!meta.empty());

    const RefMeta* noraBranch = findMeta(meta, QStringLiteral("feature/nora"));
    QVERIFY(noraBranch);
    QCOMPARE(QString::fromStdString(noraBranch->authorEmail), QStringLiteral("nora@example.com"));
    QCOMPARE(QString::fromStdString(noraBranch->upstream), QStringLiteral("origin/feature/nora"));
    QVERIFY(noraBranch->local);
    QVERIFY(!noraBranch->ahead);
    QVERIFY(!noraBranch->behind);

    const RefMeta* bob = findMeta(meta, QStringLiteral("feature/bob"));
    QVERIFY(bob);
    QVERIFY(bob->upstream.empty());
    QVERIFY(bob->local);
    QCOMPARE(QString::fromStdString(bob->authorEmail), QStringLiteral("bob@example.com"));

    const RefMeta* ahead = findMeta(meta, QStringLiteral("local-ahead"));
    QVERIFY(ahead);
    QVERIFY(ahead->ahead);
    QVERIFY(!ahead->behind);

    const RefMeta* behind = findMeta(meta, QStringLiteral("behind"));
    QVERIFY(behind);
    QVERIFY(behind->behind);
    QVERIFY(!behind->ahead);
}

void SmartVisibilityTest::smartKeepsHeadTargetAndUpstreams() {
    MainWindow window;
    QVERIFY(window.openRepository(m_work));
    const QString text = report(window, QStringLiteral("smart"));

    QVERIFY(text.contains(QStringLiteral("smart: yes")));
    QVERIFY(text.contains(QStringLiteral("target: refs/heads/main")));
    QVERIFY(text.contains(QStringLiteral("label: feature/nora")));
    QVERIFY(text.contains(QStringLiteral("label: main")));
    QVERIFY(text.contains(QStringLiteral("label: origin/main")));
    QVERIFY(text.contains(QStringLiteral("label: origin/feature/nora")));
    QVERIFY(!text.contains(QStringLiteral("label: feature/bob")));
    QVERIFY(!text.contains(QStringLiteral("label: ancient")));

    window.visibilityForDiagnostics(QStringLiteral("show"), QString());
}

void SmartVisibilityTest::mineKeepsOnlyMyBranches() {
    MainWindow window;
    QVERIFY(window.openRepository(m_work));
    const QString text = report(window, QStringLiteral("preset:mine"));

    QVERIFY(text.contains(QStringLiteral("preset: my branches")));
    QVERIFY(text.contains(QStringLiteral("label: feature/nora")));
    QVERIFY(text.contains(QStringLiteral("label: local-ahead")));
    QVERIFY(text.contains(QStringLiteral("label: main")));
    QVERIFY(!text.contains(QStringLiteral("label: feature/bob")));

    window.visibilityForDiagnostics(QStringLiteral("show"), QString());
}

void SmartVisibilityTest::prefixMatchesBranchNames() {
    MainWindow window;
    QVERIFY(window.openRepository(m_work));
    const QString text = report(window, QStringLiteral("preset-prefix:feature/"));

    QVERIFY(text.contains(QStringLiteral("label: feature/nora")));
    QVERIFY(text.contains(QStringLiteral("label: feature/bob")));
    QVERIFY(!text.contains(QStringLiteral("label: local-ahead")));
    QVERIFY(!text.contains(QStringLiteral("label: main")));

    window.visibilityForDiagnostics(QStringLiteral("show"), QString());
}

void SmartVisibilityTest::recentDropsOldTips() {
    MainWindow window;
    QVERIFY(window.openRepository(m_work));
    const QString text = report(window, QStringLiteral("preset:recent"));

    QVERIFY(text.contains(QStringLiteral("label: feature/nora")));
    QVERIFY(!text.contains(QStringLiteral("label: ancient")));

    window.visibilityForDiagnostics(QStringLiteral("show"), QString());
}

void SmartVisibilityTest::withoutUpstreamKeepsUntrackedBranches() {
    MainWindow window;
    QVERIFY(window.openRepository(m_work));
    const QString text = report(window, QStringLiteral("preset:no-upstream"));

    QVERIFY(text.contains(QStringLiteral("label: feature/bob")));
    QVERIFY(text.contains(QStringLiteral("label: ancient")));
    QVERIFY(!text.contains(QStringLiteral("label: local-ahead")));
    QVERIFY(!text.contains(QStringLiteral("label: behind")));

    window.visibilityForDiagnostics(QStringLiteral("show"), QString());
}

void SmartVisibilityTest::aheadKeepsBranchesToPush() {
    MainWindow window;
    QVERIFY(window.openRepository(m_work));
    const QString text = report(window, QStringLiteral("preset:ahead"));

    QVERIFY(text.contains(QStringLiteral("label: local-ahead")));
    QVERIFY(!text.contains(QStringLiteral("label: main")));

    window.visibilityForDiagnostics(QStringLiteral("show"), QString());
}

void SmartVisibilityTest::behindKeepsBranchesToPull() {
    MainWindow window;
    QVERIFY(window.openRepository(m_work));
    const QString text = report(window, QStringLiteral("preset:behind"));

    QVERIFY(text.contains(QStringLiteral("label: behind")));
    QVERIFY(!text.contains(QStringLiteral("label: local-ahead")));

    window.visibilityForDiagnostics(QStringLiteral("show"), QString());
}

void SmartVisibilityTest::clearingDropsEveryMode() {
    MainWindow window;
    QVERIFY(window.openRepository(m_work));

    QString text = report(window, QStringLiteral("preset:mine"));
    QVERIFY(text.contains(QStringLiteral("preset: my branches")));
    text = report(window, QStringLiteral("smart"));
    QVERIFY(text.contains(QStringLiteral("smart: yes")));
    QVERIFY(text.contains(QStringLiteral("preset: (none)")));

    text = report(window, QStringLiteral("show"));
    QVERIFY(text.contains(QStringLiteral("smart: no")));
    QVERIFY(text.contains(QStringLiteral("preset: (none)")));
    QVERIFY(text.contains(QStringLiteral("label: feature/bob")));
}

QTEST_MAIN(SmartVisibilityTest)
#include "test_smart_visibility.moc"
