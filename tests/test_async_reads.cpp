/// Reads the window must not wait for. A refresh reads one snapshot on a worker and
/// the session answers from it until the next write; the window shows skeleton rows
/// for what has not arrived yet and never blocks behind a history page.

#include "git/cli.h"
#include "git/repository.h"
#include "repo/repo_session.h"
#include "ui/commit_graph_view.h"
#include "ui/diff_view.h"
#include "ui/working_tree_panel.h"
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
    void anEditMadeOutsideReachesTheOpenDiff();
    void aStashMadeOutsideReloadsTheGraph();
    void aStashMadeInTheWindowReloadsTheGraph();
    void aTabSwitchDoesNotKeepTheOtherTabsFile();
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

namespace {

CommitModel* modelOf(MainWindow& window) {
    auto* view = window.findChild<CommitGraphView*>();
    return view ? qobject_cast<CommitModel*>(view->model()) : nullptr;
}

QString revParse(const QString& dir, const QString& revision) {
    return QString::fromStdString(
               runGit({"rev-parse", revision.toStdString()}, dir.toStdString()).out)
        .trimmed();
}

bool diffShows(const DiffView* view, const std::string& text) {
    for (const DiffHunk& hunk : view->document().diff.hunks) {
        for (const DiffLine& line : hunk.lines) {
            if (line.text == text) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

void AsyncReadsTest::anEditMadeOutsideReachesTheOpenDiff() {
    MainWindow window;
    window.setHeadless(true);
    window.setAsyncReadsEnabled(true);
    QVERIFY(window.openRepository(m_repo));
    QTRY_VERIFY_WITH_TIMEOUT(allReady(window.loadingStateForDiagnostics()), 5000);
    auto* diff = window.findChild<DiffView*>();
    QVERIFY(diff);
    QTRY_VERIFY_WITH_TIMEOUT(diffShows(diff, "dirty"), 5000);

    // Saved by another program, in place: the watcher sees the content change, the
    // refresh keeps the file chosen and reads its diff again.
    QVERIFY(writeFile(QDir(m_repo).filePath(QStringLiteral("f.txt")),
                      QStringLiteral("edited elsewhere\n")));
    QTRY_VERIFY_WITH_TIMEOUT(diffShows(diff, "edited elsewhere"), 8000);
    QCOMPARE(diff->document().path, QStringLiteral("f.txt"));
}

void AsyncReadsTest::aStashMadeOutsideReloadsTheGraph() {
    MainWindow window;
    window.setHeadless(true);
    window.setAsyncReadsEnabled(true);
    QVERIFY(window.openRepository(m_repo));
    QTRY_VERIFY_WITH_TIMEOUT(allReady(window.loadingStateForDiagnostics()), 5000);
    CommitModel* model = modelOf(window);
    QVERIFY(model);
    QTRY_VERIFY_WITH_TIMEOUT(model->rowForCommit(revParse(m_repo, QStringLiteral("HEAD"))) >= 0,
                             5000);

    QVERIFY(git(m_repo, {"stash", "push", "-q"}));
    const QString stash = revParse(m_repo, QStringLiteral("refs/stash"));
    QVERIFY(!stash.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(model->rowForCommit(stash) >= 0, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(model->wipRow(), -1, 5000);
}

void AsyncReadsTest::aStashMadeInTheWindowReloadsTheGraph() {
    MainWindow window;
    window.setHeadless(true);
    QVERIFY(window.openRepository(m_repo));
    CommitModel* model = modelOf(window);
    QVERIFY(model);
    QTRY_VERIFY_WITH_TIMEOUT(model->rowForCommit(revParse(m_repo, QStringLiteral("HEAD"))) >= 0,
                             5000);

    auto* panel = window.findChild<WorkingTreePanel*>();
    QVERIFY(panel);
    emit panel->stashAllRequested(QStringLiteral("from the window"));
    const QString stash = revParse(m_repo, QStringLiteral("refs/stash"));
    QVERIFY(!stash.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(model->rowForCommit(stash) >= 0, 5000);
}

void AsyncReadsTest::aTabSwitchDoesNotKeepTheOtherTabsFile() {
    const QString other = QDir(m_dir->path()).filePath(QStringLiteral("other"));
    QVERIFY(QDir().mkpath(other));
    QVERIFY(git(other, {"init", "-q", "-b", "main"}));
    QVERIFY(writeFile(QDir(other).filePath(QStringLiteral("g.txt")), QStringLiteral("g\n")));
    QVERIFY(git(other, {"add", "-A"}));
    QVERIFY(git(other, {"commit", "-qm", "only"}));

    MainWindow window;
    window.setHeadless(true);
    QVERIFY(window.openRepository(m_repo));
    auto* diff = window.findChild<DiffView*>();
    QVERIFY(diff);
    QTRY_COMPARE_WITH_TIMEOUT(diff->document().path, QStringLiteral("f.txt"), 5000);

    QVERIFY(window.openRepository(other));
    QTRY_VERIFY_WITH_TIMEOUT(diff->document().path != QStringLiteral("f.txt"), 5000);
    QVERIFY(diff->document().diff.hunks.empty());

    window.tabsForDiagnostics(QStringLiteral("activate"), {QStringLiteral("0")});
    QTRY_COMPARE_WITH_TIMEOUT(diff->document().path, QStringLiteral("f.txt"), 5000);
    QVERIFY(diffShows(diff, "dirty"));
}

QTEST_MAIN(AsyncReadsTest)
#include "test_async_reads.moc"
