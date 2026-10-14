/// Stashes in the graph are stashes, not commits, and what is hidden "only for me"
/// stays out of everyone else's way.

#include "git/cli.h"
#include "git/repository.h"
#include "model/commit_model.h"
#include "model/ref_model.h"
#include "repo/repo_session.h"
#include "ui/commit_graph_view.h"
#include "ui/main_window.h"
#include "ui/ref_pill_layout.h"

#include <QAction>
#include <QDir>
#include <QDragMoveEvent>
#include <QFile>
#include <QMimeData>
#include <QTemporaryDir>
#include <QToolBar>
#include <QtTest>

using namespace gk;

namespace {

bool git(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
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

bool writeFile(const QString& path, const QString& content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(content.toUtf8()) == content.toUtf8().size();
}

CommitModel* modelOf(MainWindow& window) {
    auto* view = window.findChild<CommitGraphView*>();
    return view ? qobject_cast<CommitModel*>(view->model()) : nullptr;
}

/// dragMoveEvent is protected; a synthetic event sent to the viewport is not routed in.
class DropProbeView : public CommitGraphView {
public:
    using CommitGraphView::CommitGraphView;
    void move(QDragMoveEvent* event) { dragMoveEvent(event); }
};

} // namespace

class StashGraphTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_config;
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_repo;

private slots:
    void initTestCase() {
        m_config = std::make_unique<QTemporaryDir>();
        qputenv("XDG_CONFIG_HOME", m_config->path().toUtf8());
        Repository::initLibrary();
    }

    void init() {
        m_dir = std::make_unique<QTemporaryDir>();
        m_repo = QDir(m_dir->path()).filePath(QStringLiteral("repo"));
        QVERIFY(QDir().mkpath(m_repo));
        QVERIFY(git(m_repo, {"init", "-q", "-b", "main"}));
        QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QStringLiteral("a\n")));
        QVERIFY(git(m_repo, {"add", "-A"}));
        QVERIFY(git(m_repo, {"commit", "-qm", "first"}));
    }

    void everyStashIsDrawn() {
        QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QStringLiteral("one\n")));
        QVERIFY(git(m_repo, {"stash", "push", "-q"}));
        QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QStringLiteral("two\n")));
        QVERIFY(git(m_repo, {"stash", "push", "-q"}));

        MainWindow window;
        window.setHeadless(true);
        QVERIFY(window.openRepository(m_repo));
        CommitModel* model = modelOf(window);
        for (const QString& stash : {QStringLiteral("stash@{0}"), QStringLiteral("stash@{1}")}) {
            const QString id = gitOut(m_repo, {"rev-parse", stash});
            QTRY_VERIFY2(model->rowForCommit(id) >= 0, qPrintable(stash));
            const GraphNode* node = model->graphNodeAt(model->rowForCommit(id));
            QCOMPARE(node->type, GraphNodeType::Stash);
        }
        // Neither stash's index commit is drawn.
        QCOMPARE(model->rowForCommit(gitOut(m_repo, {"rev-parse", "stash@{1}^2"})), -1);
    }

    void aStashOfNewFilesListsThem() {
        QVERIFY(writeFile(m_repo + QStringLiteral("/fresh.txt"), QStringLiteral("new\n")));
        QVERIFY(git(m_repo, {"stash", "push", "-u", "-q"}));
        RepoSession session;
        QVERIFY(session.open(m_repo));
        const QString stash = gitOut(m_repo, {"rev-parse", "refs/stash"});
        const std::vector<ChangedFile> files = session.changedFiles(stash);
        QCOMPARE(files.size(), std::size_t{1});
        QCOMPARE(files.front().path, std::string("fresh.txt"));
        QCOMPARE(files.front().kind, ChangeKind::Added);
        const FileDiff diff = session.commitFileDiff(stash, QStringLiteral("fresh.txt"));
        QCOMPARE(diff.added, 1);
    }

    void aStashOffersOnlyStashOperations() {
        QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QStringLiteral("one\n")));
        QVERIFY(git(m_repo, {"stash", "push", "-q"}));
        MainWindow window;
        window.setHeadless(true);
        QVERIFY(window.openRepository(m_repo));
        const QString stash = gitOut(m_repo, {"rev-parse", "refs/stash"});
        QTRY_VERIFY(modelOf(window)->rowForCommit(stash) >= 0);
        const QString entries = window.commitMenuEntries(stash).join(QLatin1Char('\n'));
        QVERIFY2(entries.contains(QStringLiteral("stash-pop")), qPrintable(entries));
        QVERIFY(entries.contains(QStringLiteral("stash-drop")));
        QVERIFY(!entries.contains(QStringLiteral("checkout")));
        QVERIFY(!entries.contains(QStringLiteral("reset")));
        QVERIFY(!entries.contains(QStringLiteral("cherry")));
    }

    void stashAndPopAreOffOnlyWhenThereIsNothingToDo() {
        MainWindow window;
        window.setHeadless(true);
        QVERIFY(window.openRepository(m_repo));
        QAction* stash = nullptr;
        QAction* pop = nullptr;
        for (QAction* action : window.findChild<QToolBar*>(QStringLiteral("mainToolBar"))->actions()) {
            if (action->iconText() == QStringLiteral("Stash")) {
                stash = action;
            } else if (action->iconText() == QStringLiteral("Pop")) {
                pop = action;
            }
        }
        QVERIFY(stash && pop);
        QVERIFY(!stash->isEnabled());
        QVERIFY(!pop->isEnabled());

        QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QStringLiteral("changed\n")));
        QVERIFY(window.openRepository(m_repo)); // already open: refreshes it
        QVERIFY(stash->isEnabled());
        QVERIFY(git(m_repo, {"stash", "push", "-q"}));
        QVERIFY(window.openRepository(m_repo)); // already open: refreshes it
        QVERIFY(!stash->isEnabled());
        QVERIFY(pop->isEnabled());
    }

    void aDraggedBranchShowsWhereItWouldLand() {
        QVERIFY(git(m_repo, {"branch", "topic"}));
        QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QStringLiteral("b\n")));
        QVERIFY(git(m_repo, {"commit", "-qam", "second"}));
        RepoSession session;
        QVERIFY(session.open(m_repo));
        CommitModel model(&session);
        model.reset();
        QTRY_VERIFY(model.realRowCount() >= 2);
        DropProbeView view;
        view.setModel(&model);
        view.resize(900, 400);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        const int row = model.rowForCommit(gitOut(m_repo, {"rev-parse", "topic"}));
        QVERIFY(row >= 0);
        QRect pill;
        for (const RefPill& candidate : view.rowPillsForDiagnostics(row)) {
            if (candidate.shortName == QStringLiteral("topic")) {
                pill = candidate.rect;
            }
        }
        QVERIFY(!pill.isNull());

        RefInfo main;
        main.fullName = "refs/heads/main";
        main.shortName = "main";
        main.kind = RefKind::LocalBranch;
        std::unique_ptr<QMimeData> mime(RefModel::mimeDataForRef(main));
        QVERIFY(mime);
        QDragMoveEvent over(pill.center(), Qt::MoveAction, mime.get(), Qt::LeftButton,
                            Qt::NoModifier);
        view.move(&over);
        QCOMPARE(view.dropTargetForDiagnostics(), QStringLiteral("topic"));

        // Off the label, onto empty space: the promise is withdrawn.
        QDragMoveEvent away(QPoint(view.viewport()->width() - 5, view.viewport()->height() - 5),
                            Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        view.move(&away);
        QVERIFY(view.dropTargetForDiagnostics().isEmpty());
    }

    void aRemoteBranchParkedOnTheTrunkHasNoBadge() {
        const QString first = gitOut(m_repo, {"rev-parse", "HEAD"});
        QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QStringLiteral("b\n")));
        QVERIFY(git(m_repo, {"commit", "-qam", "second"}));
        QVERIFY(git(m_repo, {"checkout", "-q", "-b", "side", first}));
        QVERIFY(writeFile(m_repo + QStringLiteral("/s.txt"), QStringLiteral("s\n")));
        QVERIFY(git(m_repo, {"add", "-A"}));
        QVERIFY(git(m_repo, {"commit", "-qm", "side work"}));
        QVERIFY(git(m_repo, {"checkout", "-q", "main"}));
        QVERIFY(git(m_repo, {"update-ref", "refs/remotes/origin/main", "main"}));
        // A release branch cut at a trunk commit, and one with a line of its own.
        QVERIFY(git(m_repo, {"update-ref", "refs/remotes/origin/release-1", first}));
        QVERIFY(git(m_repo, {"update-ref", "refs/remotes/origin/feature", "side"}));
        QVERIFY(git(m_repo, {"branch", "-D", "side"}));

        MainWindow window;
        window.setHeadless(true);
        QVERIFY(window.openRepository(m_repo));
        CommitModel* model = modelOf(window);
        QTRY_VERIFY(model->rowForCommit(first) >= 0);
        const int firstRow = model->rowForCommit(first);
        const auto names = [&](int row) {
            QStringList out;
            for (const RefInfo& ref : model->refsAt(row)) {
                out << QString::fromStdString(ref.shortName);
            }
            return out;
        };
        QVERIFY2(!names(firstRow).contains(QStringLiteral("origin/release-1")),
                 qPrintable(names(firstRow).join(',')));
        const int sideRow = model->rowForCommit(gitOut(m_repo, {"rev-parse", "origin/feature"}));
        QVERIFY(names(sideRow).contains(QStringLiteral("origin/feature")));
        const int mainRow = model->rowForCommit(gitOut(m_repo, {"rev-parse", "main"}));
        QVERIFY(names(mainRow).contains(QStringLiteral("origin/main")));

        model->setShowMergedRemotes(true);
        QVERIFY(names(firstRow).contains(QStringLiteral("origin/release-1")));
    }

    void hiddenOnlyForMeLeavesTheSharedFilesAlone() {
        QVERIFY(writeFile(m_repo + QStringLiteral("/local.log"), QStringLiteral("noise\n")));
        QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QStringLiteral("my tweak\n")));
        MainWindow window;
        window.setHeadless(true);
        QVERIFY(window.openRepository(m_repo));

        for (const QString& path : {QStringLiteral("local.log"), QStringLiteral("a.txt")}) {
            window.ignoreForDiagnostics(path, QStringLiteral("exclude"), QStringLiteral("file"));
        }
        // Nothing to commit for anyone: no .gitignore, nothing staged, and both gone
        // from the status.
        QVERIFY(!QFile::exists(m_repo + QStringLiteral("/.gitignore")));
        QCOMPARE(gitOut(m_repo, {"status", "--porcelain"}), QString());
        QCOMPARE(gitOut(m_repo, {"diff", "--cached", "--name-only"}), QString());
        RepoSession session;
        QVERIFY(session.open(m_repo));
        QCOMPARE(session.localExcludePatterns(), QStringList{QStringLiteral("/local.log")});
        QCOMPARE(session.skipWorktreePaths(), QStringList{QStringLiteral("a.txt")});

        // And both come back from the dialog's edits.
        QVERIFY(window.applyLocalIgnore({QStringLiteral("*.tmp")}, {QStringLiteral("/local.log")},
                                        {QStringLiteral("a.txt")}));
        QCOMPARE(session.localExcludePatterns(), QStringList{QStringLiteral("*.tmp")});
        QVERIFY(session.skipWorktreePaths().isEmpty());
        const QString status = gitOut(m_repo, {"status", "--porcelain"});
        QVERIFY2(status.contains(QStringLiteral("local.log")) &&
                     status.contains(QStringLiteral("a.txt")),
                 qPrintable(status));
    }
};

QTEST_MAIN(StashGraphTest)
#include "test_stash_graph.moc"
