/// Commits dragged onto a branch's label are picked onto that branch. A branch that is
/// not checked out is picked onto in memory: the working tree and the current branch
/// are not touched, and a conflict leaves the branch where it was.

#include "git/cli.h"
#include "git/repository.h"
#include "model/commit_model.h"
#include "model/ref_model.h"
#include "repo/repo_session.h"
#include "ui/commit_graph_view.h"
#include "ui/main_window.h"
#include "ui/ref_pill_layout.h"
#include "ui/ref_tree_view.h"

#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QMimeData>
#include <QSignalSpy>
#include <QTemporaryDir>
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

/// The drag handlers are protected; a synthetic event sent to the viewport is not
/// routed in.
class DropProbeView : public CommitGraphView {
public:
    using CommitGraphView::CommitGraphView;
    void move(QDragMoveEvent* event) { dragMoveEvent(event); }
    void drop(QDropEvent* event) { dropEvent(event); }
};

class RailProbe : public RefTreeView {
public:
    using RefTreeView::RefTreeView;
    void move(QDragMoveEvent* event) { dragMoveEvent(event); }
    void drop(QDropEvent* event) { dropEvent(event); }
};

} // namespace

class PickOntoTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aBranchNotCheckedOutGetsTheCommit();
    void aConflictMovesNothing();
    void aChangeTheBranchHasIsLeftOut();
    void aMergeIsRefused();
    void dragOntoALabelPicks();
    void dragOntoTheRailPicks();
    void theWindowChecksOutToResolveAConflict();

private:
    bool commit(const QString& file, const QString& content, const QString& message,
                const QString& author = QStringLiteral("Dev"));

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

bool PickOntoTest::commit(const QString& file, const QString& content, const QString& message,
                          const QString& author) {
    return writeFile(m_path + QLatin1Char('/') + file, content) && git(m_path, {"add", "-A"}) &&
           git(m_path, {"-c", "user.name=" + author, "-c", "user.email=" + author + "@x",
                        "commit", "-qm", message});
}

void PickOntoTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Picker"}));
    QVERIFY(git(m_path, {"config", "user.email", "picker@x"}));
    QVERIFY(commit(QStringLiteral("shared.txt"), QStringLiteral("one\n"), QStringLiteral("base")));
    QVERIFY(git(m_path, {"branch", "feature"}));
    QVERIFY(commit(QStringLiteral("main.txt"), QStringLiteral("m\n"), QStringLiteral("main work"),
                   QStringLiteral("Alice")));
    QVERIFY(commit(QStringLiteral("shared.txt"), QStringLiteral("main\n"),
                   QStringLiteral("main edits shared")));
    QVERIFY(git(m_path, {"checkout", "-q", "feature"}));
    QVERIFY(commit(QStringLiteral("shared.txt"), QStringLiteral("feature\n"),
                   QStringLiteral("feature edits shared")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
}

void PickOntoTest::cleanup() {
    m_dir.reset();
}

void PickOntoTest::aBranchNotCheckedOutGetsTheCommit() {
    const QString pick = gitOut(m_path, {"rev-parse", "main~1"});
    const QString before = gitOut(m_path, {"rev-parse", "feature"});
    const auto repo = Repository::open(m_path.toStdString());
    const PickOntoResult result = repo->cherryPickOnto("feature", {pick.toStdString()});
    QVERIFY(!result.needsCheckout);
    QCOMPARE(result.picked, 1);
    QCOMPARE(gitOut(m_path, {"rev-parse", "feature^"}), before);
    QCOMPARE(gitOut(m_path, {"rev-parse", "feature"}), QString::fromStdString(result.tip));
    QCOMPARE(gitOut(m_path, {"log", "-1", "--format=%an|%cn|%s", "feature"}),
             QStringLiteral("Alice|Picker|main work"));
    QCOMPARE(gitOut(m_path, {"show", "feature:main.txt"}), QStringLiteral("m"));
    // Nothing moved here: the checkout is main, clean.
    QCOMPARE(gitOut(m_path, {"rev-parse", "--abbrev-ref", "HEAD"}), QStringLiteral("main"));
    QVERIFY(gitOut(m_path, {"status", "--porcelain"}).isEmpty());
}

void PickOntoTest::aConflictMovesNothing() {
    const QString before = gitOut(m_path, {"rev-parse", "feature"});
    const auto repo = Repository::open(m_path.toStdString());
    const PickOntoResult result = repo->cherryPickOnto(
        "feature", {gitOut(m_path, {"rev-parse", "main~1"}).toStdString(),
                    gitOut(m_path, {"rev-parse", "main"}).toStdString()});
    QVERIFY(result.needsCheckout);
    QCOMPARE(result.conflicts, std::vector<std::string>{"shared.txt"});
    QCOMPARE(gitOut(m_path, {"rev-parse", "feature"}), before);
    QVERIFY(gitOut(m_path, {"status", "--porcelain"}).isEmpty());
}

void PickOntoTest::aChangeTheBranchHasIsLeftOut() {
    QVERIFY(git(m_path, {"branch", "copy", "main"}));
    const QString before = gitOut(m_path, {"rev-parse", "copy"});
    const auto repo = Repository::open(m_path.toStdString());
    const PickOntoResult result =
        repo->cherryPickOnto("copy", {gitOut(m_path, {"rev-parse", "main~1"}).toStdString()});
    QCOMPARE(result.picked, 0);
    QCOMPARE(result.empty, 1);
    QCOMPARE(gitOut(m_path, {"rev-parse", "copy"}), before);
}

void PickOntoTest::aMergeIsRefused() {
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "side", "main~2"}));
    QVERIFY(commit(QStringLiteral("side.txt"), QStringLiteral("s\n"), QStringLiteral("side")));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(git(m_path, {"merge", "-q", "--no-ff", "-m", "merge side", "side"}));
    RepoSession session;
    QVERIFY(session.open(m_path));
    PickOntoResult result;
    QString error;
    QVERIFY(!session.cherryPickOnto(QStringLiteral("feature"),
                                    {gitOut(m_path, {"rev-parse", "main"})}, &result, &error));
    QVERIFY2(error.contains(QStringLiteral("merge")), qPrintable(error));
}

void PickOntoTest::dragOntoALabelPicks() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    CommitModel model(&session);
    model.reset();
    QTRY_VERIFY(model.realRowCount() >= 4);
    DropProbeView view;
    view.setModel(&model);
    view.resize(900, 400);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    const int row = model.rowForCommit(gitOut(m_path, {"rev-parse", "feature"}));
    QRect pill;
    for (const RefPill& candidate : view.rowPillsForDiagnostics(row)) {
        if (candidate.shortName == QStringLiteral("feature")) {
            pill = candidate.rect;
        }
    }
    QVERIFY(!pill.isNull());

    const QString pick = gitOut(m_path, {"rev-parse", "main~1"});
    QMimeData mime;
    mime.setData(QString::fromLatin1(kCommitsMimeType), pick.toUtf8());
    QDragMoveEvent over(pill.center(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    view.move(&over);
    QVERIFY(over.isAccepted());
    QCOMPARE(view.dropTargetForDiagnostics(), QStringLiteral("feature"));

    // Not a branch: the commit row beside the labels takes nothing.
    QDragMoveEvent beside(QPoint(view.viewport()->width() - 5, pill.center().y()), Qt::CopyAction,
                          &mime, Qt::LeftButton, Qt::NoModifier);
    view.move(&beside);
    QVERIFY(!beside.isAccepted());

    QSignalSpy dropped(&view, &CommitGraphView::commitsDroppedOnBranch);
    QDropEvent drop(QPointF(pill.center()), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    view.drop(&drop);
    QCOMPARE(dropped.count(), 1);
    QCOMPARE(dropped.front().at(0).toStringList(), QStringList{pick});
    QCOMPARE(dropped.front().at(1).toString(), QStringLiteral("feature"));
}

void PickOntoTest::dragOntoTheRailPicks() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    RefModel model;
    model.setHeadBranch(QStringLiteral("main"));
    model.rebuild(session.refs());
    RailProbe rail;
    rail.setModel(&model);
    rail.resize(400, 400);
    rail.show();
    QVERIFY(QTest::qWaitForWindowExposed(&rail));
    rail.expandAll();
    const QModelIndexList found =
        model.match(model.index(0, 0), RefModel::ShortNameRole, QStringLiteral("feature"), 1,
                    Qt::MatchExactly | Qt::MatchRecursive);
    QVERIFY(!found.isEmpty());
    const QPoint at = rail.visualRect(found.front()).center();

    const QString pick = gitOut(m_path, {"rev-parse", "main~1"});
    QMimeData mime;
    mime.setData(QString::fromLatin1(kCommitsMimeType), pick.toUtf8());
    QDragMoveEvent over(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    rail.move(&over);
    QVERIFY(over.isAccepted());
    QSignalSpy dropped(&rail, &RefTreeView::commitsDroppedOnBranch);
    QDropEvent drop(QPointF(at), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    rail.drop(&drop);
    QCOMPARE(dropped.count(), 1);
    QCOMPARE(dropped.front().at(1).toString(), QStringLiteral("feature"));
}

void PickOntoTest::theWindowChecksOutToResolveAConflict() {
    MainWindow window;
    window.setHeadless(true);
    QVERIFY(window.openRepository(m_path));

    window.pickCommitsOntoBranch({gitOut(m_path, {"rev-parse", "main~1"})},
                                 QStringLiteral("feature"));
    QCOMPARE(gitOut(m_path, {"log", "-1", "--format=%s", "feature"}), QStringLiteral("main work"));
    QCOMPARE(gitOut(m_path, {"rev-parse", "--abbrev-ref", "HEAD"}), QStringLiteral("main"));

    // The conflicting one: the window asks (headless answers yes), checks feature out
    // and leaves the conflict for the merge editor.
    window.pickCommitsOntoBranch({gitOut(m_path, {"rev-parse", "main"})}, QStringLiteral("feature"));
    QCOMPARE(gitOut(m_path, {"rev-parse", "--abbrev-ref", "HEAD"}), QStringLiteral("feature"));
    QVERIFY(gitOut(m_path, {"diff", "--name-only", "--diff-filter=U"})
                .contains(QStringLiteral("shared.txt")));
    QVERIFY(git(m_path, {"cherry-pick", "--abort"}));
}

QTEST_MAIN(PickOntoTest)
#include "test_pick_onto.moc"
