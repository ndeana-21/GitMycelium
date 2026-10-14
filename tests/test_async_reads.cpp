/// Reads the window must not wait for. A refresh reads one snapshot on a worker and
/// the session answers from it until the next write; the window shows skeleton rows
/// for what has not arrived yet and never blocks behind a history page.

#include "git/cli.h"
#include "git/repository.h"
#include "repo/repo_session.h"
#include "ui/commit_graph_view.h"
#include "ui/main_window.h"

#include <QDir>
#include <QFile>
#include <QLabel>
#include <QSignalSpy>
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

bool writeFile(const QString& path, const QString& content) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content.toUtf8()) == content.toUtf8().size();
}

bool hasBranch(const std::vector<RefInfo>& refs, const std::string& name) {
    return std::any_of(refs.begin(), refs.end(), [&](const RefInfo& ref) {
        return ref.kind == RefKind::LocalBranch && ref.shortName == name;
    });
}

bool allReady(const QStringList& state) {
    return std::none_of(state.begin(), state.end(),
                        [](const QString& line) { return line.endsWith(QStringLiteral("loading")); });
}

} // namespace

class AsyncReadsTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_config;
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_repo;

private slots:
    void initTestCase();
    void init();

    void theSnapshotAnswersUntilAWrite();
    void aRetiredSnapshotIsNotServed();
    void aRequestedSnapshotArrivesOnAWorker();
    void anAsyncReadComesBackOnTheReceiversThread();
    void aNewTabShowsSkeletonsUntilItsSnapshotLands();
    void aCommitsFilesLoadBehindASkeleton();
    void aTabShownBeforeComesBackWithoutSkeletons();
    void aCheckoutRunsOnAWorker();
    void anUntrackedFileDiffsWithoutWalkingTheTree();
};

void AsyncReadsTest::initTestCase() {
    m_config = std::make_unique<QTemporaryDir>();
    QVERIFY(m_config->isValid());
    qputenv("XDG_CONFIG_HOME", m_config->path().toUtf8());
    Repository::initLibrary();
}

void AsyncReadsTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_repo = QDir(m_dir->path()).filePath(QStringLiteral("repo"));
    QVERIFY(QDir().mkpath(m_repo));
    QVERIFY(git(m_repo, {"init", "-q", "-b", "main"}));
    QVERIFY(writeFile(QDir(m_repo).filePath(QStringLiteral("f.txt")), QStringLiteral("one\n")));
    QVERIFY(git(m_repo, {"add", "-A"}));
    QVERIFY(git(m_repo, {"commit", "-qm", "first"}));
    QVERIFY(writeFile(QDir(m_repo).filePath(QStringLiteral("f.txt")), QStringLiteral("one\ntwo\n")));
    QVERIFY(git(m_repo, {"commit", "-qam", "second"}));
    QVERIFY(writeFile(QDir(m_repo).filePath(QStringLiteral("f.txt")), QStringLiteral("dirty\n")));
}

void AsyncReadsTest::theSnapshotAnswersUntilAWrite() {
    RepoSession session;
    QVERIFY(session.open(m_repo));
    QVERIFY(!session.snapshot());
    session.primeSnapshot();
    const auto snapshot = session.snapshot();
    QVERIFY(snapshot);
    QCOMPARE(snapshot->status.size(), std::size_t{1});
    QCOMPARE(session.status().size(), std::size_t{1});
    QCOMPARE(session.info().headBranch, std::string("main"));

    QString error;
    QVERIFY2(session.createBranch(QStringLiteral("topic"), QStringLiteral("HEAD"), &error),
             qPrintable(error));
    // The write retired it: the next read goes to the repository, not the snapshot.
    QVERIFY(!session.snapshot());
    QVERIFY(hasBranch(session.refs(), "topic"));
    session.primeSnapshot();
    QVERIFY(session.snapshot());
    QVERIFY(hasBranch(session.snapshot()->refs, "topic"));
}

void AsyncReadsTest::aRetiredSnapshotIsNotServed() {
    RepoSession session;
    QVERIFY(session.open(m_repo));
    session.primeSnapshot();
    QVERIFY(session.snapshot());
    // Another client commits; the session only learns of it when asked to forget.
    QVERIFY(git(m_repo, {"commit", "-qam", "third"}));
    QCOMPARE(session.status().size(), std::size_t{1});
    session.invalidateSnapshot();
    QVERIFY(!session.snapshot());
    QCOMPARE(session.status().size(), std::size_t{0});
}

void AsyncReadsTest::aRequestedSnapshotArrivesOnAWorker() {
    RepoSession session;
    QVERIFY(session.open(m_repo));
    QSignalSpy ready(&session, &RepoSession::snapshotReady);
    session.requestSnapshot();
    session.requestSnapshot();
    QVERIFY(ready.wait(5000));
    QVERIFY(session.snapshot());
    QCOMPARE(session.snapshot()->info.headBranch, std::string("main"));
    // The second request, made while the first was reading, is one more read, not
    // a queue of them.
    QTest::qWait(300);
    QVERIFY(ready.size() <= 2);
}

void AsyncReadsTest::anAsyncReadComesBackOnTheReceiversThread() {
    RepoSession session;
    QVERIFY(session.open(m_repo));
    QObject receiver;
    QThread* answeredOn = nullptr;
    std::size_t files = 0;
    session.readAsync<std::size_t>(
        &receiver, [](RepoSession& s) { return s.status().size(); },
        [&](std::size_t count) {
            answeredOn = QThread::currentThread();
            files = count;
        });
    QTRY_VERIFY(answeredOn != nullptr);
    QCOMPARE(answeredOn, receiver.thread());
    QCOMPARE(files, std::size_t{1});
}

void AsyncReadsTest::aNewTabShowsSkeletonsUntilItsSnapshotLands() {
    MainWindow window;
    window.setHeadless(true);
    window.setAsyncReadsEnabled(true);
    QVERIFY(window.openRepository(m_repo));
    // Nothing has been read on the GUI thread yet: every panel still waits.
    const QStringList before = window.loadingStateForDiagnostics();
    QVERIFY2(before.contains(QStringLiteral("refs: loading")), qPrintable(before.join(", ")));
    QVERIFY2(before.contains(QStringLiteral("branch: loading")), qPrintable(before.join(", ")));
    QVERIFY2(before.contains(QStringLiteral("changes: loading")), qPrintable(before.join(", ")));

    QTRY_VERIFY2_WITH_TIMEOUT(allReady(window.loadingStateForDiagnostics()),
                              qPrintable(window.loadingStateForDiagnostics().join(", ")), 5000);
    auto* branch = window.findChild<QLabel*>(QStringLiteral("statusBranch"));
    QVERIFY(branch);
    QVERIFY2(branch->text().contains(QStringLiteral("main")), qPrintable(branch->text()));
}

void AsyncReadsTest::aCommitsFilesLoadBehindASkeleton() {
    MainWindow window;
    window.setHeadless(true);
    window.setAsyncReadsEnabled(true);
    QVERIFY(window.openRepository(m_repo));
    QTRY_VERIFY_WITH_TIMEOUT(allReady(window.loadingStateForDiagnostics()), 5000);

    auto* graph = window.findChild<CommitGraphView*>();
    QVERIFY(graph && graph->model());
    // Row 0 is the working tree; row 1 the newest commit.
    graph->setCurrentIndex(graph->model()->index(1, 0));
    QVERIFY2(window.loadingStateForDiagnostics().contains(QStringLiteral("files: loading")),
             qPrintable(window.loadingStateForDiagnostics().join(", ")));
    QTRY_VERIFY_WITH_TIMEOUT(allReady(window.loadingStateForDiagnostics()), 5000);
}

void AsyncReadsTest::aTabShownBeforeComesBackWithoutSkeletons() {
    const QString other = QDir(m_dir->path()).filePath(QStringLiteral("other"));
    QVERIFY(git(m_dir->path(), {"clone", "-q", m_repo, other}));

    MainWindow window;
    window.setHeadless(true);
    window.setAsyncReadsEnabled(true);
    QVERIFY(window.openRepository(m_repo));
    QTRY_VERIFY_WITH_TIMEOUT(allReady(window.loadingStateForDiagnostics()), 5000);
    QVERIFY(window.openRepository(other));
    QTRY_VERIFY_WITH_TIMEOUT(allReady(window.loadingStateForDiagnostics()), 5000);

    window.tabsForDiagnostics(QStringLiteral("activate"), {QStringLiteral("0")});
    const QStringList state = window.loadingStateForDiagnostics();
    QVERIFY2(state.contains(QStringLiteral("refs: ready")), qPrintable(state.join(", ")));
    QVERIFY2(state.contains(QStringLiteral("branch: ready")), qPrintable(state.join(", ")));
    QCOMPARE(window.currentBranchName(), std::string("main"));
}

void AsyncReadsTest::aCheckoutRunsOnAWorker() {
    QVERIFY(git(m_repo, {"checkout", "-q", "--", "f.txt"}));
    QVERIFY(git(m_repo, {"branch", "topic", "HEAD~1"}));
    MainWindow window;
    window.setHeadless(true);
    window.setAsyncReadsEnabled(true);
    QVERIFY(window.openRepository(m_repo));
    QTRY_VERIFY_WITH_TIMEOUT(allReady(window.loadingStateForDiagnostics()), 5000);

    window.commandPaletteForDiagnostics(QStringLiteral("Checkout topic"), true);
    QTRY_COMPARE_WITH_TIMEOUT(window.currentBranchName(), std::string("topic"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(allReady(window.loadingStateForDiagnostics()), 5000);
    auto* branch = window.findChild<QLabel*>(QStringLiteral("statusBranch"));
    QTRY_VERIFY_WITH_TIMEOUT(branch->text().contains(QStringLiteral("topic")), 5000);
}

void AsyncReadsTest::anUntrackedFileDiffsWithoutWalkingTheTree() {
    QVERIFY(writeFile(QDir(m_repo).filePath(QStringLiteral("new.txt")),
                      QStringLiteral("alpha\nbeta")));
    RepoSession session;
    QVERIFY(session.open(m_repo));
    const FileDiff diff = session.workdirFileDiff(QStringLiteral("new.txt"));
    QCOMPARE(diff.kind, ChangeKind::Untracked);
    QCOMPARE(diff.added, 2);
    QCOMPARE(diff.hunks.size(), std::size_t{1});
    QCOMPARE(diff.hunks.front().lines.back().text, std::string("beta"));
    QVERIFY(diff.hunks.front().lines.back().noNewline);
    QCOMPARE(diff.newMode, 0100644);
}

QTEST_MAIN(AsyncReadsTest)
#include "test_async_reads.moc"
