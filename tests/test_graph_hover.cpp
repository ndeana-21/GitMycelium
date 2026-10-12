#include "core/selection_plan.h"
#include "core/types.h"
#include "git/cli.h"
#include "model/commit_model.h"
#include "repo/repo_session.h"
#include "ui/commit_graph_view.h"
#include "ui/main_window.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace gk;

namespace {

// Each commit is a second later than the last. Which of two tips the graph
// draws first follows their dates, and commits made within one second leave that
// to a tie-break — the fixtures describe an order, so they spell it out.
void stampNextCommit(const QStringList& args) {
    static qint64 date = 1700000000;
    if (args.contains(QStringLiteral("commit"))) {
        const QByteArray stamp = QByteArray::number(++date) + " +0000";
        qputenv("GIT_AUTHOR_DATE", stamp);
        qputenv("GIT_COMMITTER_DATE", stamp);
    }
}

bool git(const QString& dir, const QStringList& args) {
    stampNextCommit(args);
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
    return QString::fromStdString(runGit(argv, dir.toStdString()).out).trimmed();
}

QStringList identity() {
    return {QStringLiteral("-c"), QStringLiteral("user.name=Test"), QStringLiteral("-c"),
            QStringLiteral("user.email=test@localhost")};
}

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

} // namespace

class GraphHoverTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void theGhostNamesTheNearestBranch();
    void aSharedCommitIsNamedByItsOwnLane();
    void aLabelledRowHasNoGhost();
    void theHighlightLightsTheAncestry();
    void clearingTheHighlightUndimsEverything();
    void theAncestorWalkIsBounded();

    void theWindowReportsAndActsOnTheGhost();
    void collapsedBranchesAreRevealedForDiagnostics();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void GraphHoverTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(writeFile(m_path, "m.txt", "m1\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "m1"}));

    // feature grows two commits; only its tip wears a label, so its first commit has
    // no label of its own and is what the ghost badge speaks for.
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature"}));
    QVERIFY(writeFile(m_path, "f.txt", "f1\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "f1"}));
    QVERIFY(writeFile(m_path, "f.txt", "f2\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "f2"}));

    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(m_path, "m.txt", "m2\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "m2"}));
}

void GraphHoverTest::cleanup() {
    m_dir.reset();
}

void GraphHoverTest::theGhostNamesTheNearestBranch() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    CommitModel model(&session);
    model.reset();
    QTRY_COMPARE(model.rowCount(), 4);

    CommitGraphView view;
    view.setModel(&model);

    const int f1Row = model.rowForCommit(gitOut(m_path, {"rev-parse", "feature~1"}));
    QVERIFY(f1Row >= 0);
    QCOMPARE(view.ghostBranchAt(f1Row), QStringLiteral("feature"));

    // The shared base is on HEAD's own line, so the ghost answers with HEAD's branch.
    const int baseRow = model.rowForCommit(gitOut(m_path, {"rev-parse", "main~1"}));
    QVERIFY(baseRow >= 0);
    QCOMPARE(view.ghostBranchAt(baseRow), QStringLiteral("main"));
}

void GraphHoverTest::aSharedCommitIsNamedByItsOwnLane() {
    // main moves on after feature forks from an older main commit, and HEAD is
    // feature. The fork commit is an ancestor of feature — but it is drawn on
    // main's line, and that is the branch it belongs to.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path();
    QVERIFY(git(path, {"init", "-q", "-b", "main"}));
    const auto commit = [&](const QString& file, const QString& text, const QString& message) {
        QVERIFY(writeFile(path, file, text));
        QVERIFY(git(path, {"add", "-A"}));
        QVERIFY(git(path, identity() + QStringList{"commit", "-q", "-m", message}));
    };
    commit(QStringLiteral("a.txt"), QStringLiteral("1\n"), QStringLiteral("m1"));
    commit(QStringLiteral("a.txt"), QStringLiteral("2\n"), QStringLiteral("m2"));
    QVERIFY(git(path, {"checkout", "-q", "-b", "feature"}));
    commit(QStringLiteral("f.txt"), QStringLiteral("1\n"), QStringLiteral("f1"));
    QVERIFY(git(path, {"checkout", "-q", "main"}));
    commit(QStringLiteral("a.txt"), QStringLiteral("3\n"), QStringLiteral("m3"));
    QVERIFY(git(path, {"checkout", "-q", "feature"}));

    RepoSession session;
    QVERIFY(session.open(path));
    CommitModel model(&session);
    model.reset();
    QTRY_VERIFY(model.rowCount() >= 4);

    CommitGraphView view;
    view.setModel(&model);

    const int forkRow = model.rowForCommit(gitOut(path, {"rev-parse", "feature~1"}));
    QVERIFY(forkRow >= 0);
    QCOMPARE(view.ghostBranchAt(forkRow), QStringLiteral("main"));
}

void GraphHoverTest::aLabelledRowHasNoGhost() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    CommitModel model(&session);
    model.reset();
    QTRY_COMPARE(model.rowCount(), 4);

    CommitGraphView view;
    view.setModel(&model);

    const int f2Row = model.rowForCommit(gitOut(m_path, {"rev-parse", "feature"}));
    QVERIFY(f2Row >= 0);
    // A commit that already wears a label needs no invented one.
    QVERIFY(view.ghostBranchAt(f2Row).isEmpty());
}

void GraphHoverTest::theHighlightLightsTheAncestry() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    CommitModel model(&session);
    model.reset();
    QTRY_COMPARE(model.rowCount(), 4);

    model.setHighlightedBranch(QStringLiteral("feature"));
    QVERIFY(model.highlightActive());

    const int f2Row = model.rowForCommit(gitOut(m_path, {"rev-parse", "feature"}));
    const int f1Row = model.rowForCommit(gitOut(m_path, {"rev-parse", "feature~1"}));
    const int baseRow = model.rowForCommit(gitOut(m_path, {"rev-parse", "main~1"}));
    const int m2Row = model.rowForCommit(gitOut(m_path, {"rev-parse", "main"}));
    QVERIFY(f2Row >= 0 && f1Row >= 0 && baseRow >= 0 && m2Row >= 0);

    QVERIFY(!model.isDimmedRow(f2Row));
    QVERIFY(!model.isDimmedRow(f1Row));
    // The shared base is part of feature's history and stays lit.
    QVERIFY(!model.isDimmedRow(baseRow));
    // main's own tip is not behind feature and fades.
    QVERIFY(model.isDimmedRow(m2Row));
}

void GraphHoverTest::clearingTheHighlightUndimsEverything() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    CommitModel model(&session);
    model.reset();
    QTRY_COMPARE(model.rowCount(), 4);

    model.setHighlightedBranch(QStringLiteral("feature"));
    const int m2Row = model.rowForCommit(gitOut(m_path, {"rev-parse", "main"}));
    QVERIFY(model.isDimmedRow(m2Row));

    model.clearHighlight();
    QVERIFY(!model.highlightActive());
    QVERIFY(!model.isDimmedRow(m2Row));
}

void GraphHoverTest::theAncestorWalkIsBounded() {
    const std::vector<LineageRow> rows = {
        {QStringLiteral("c").toStdString(), {QStringLiteral("b").toStdString()}},
        {QStringLiteral("b").toStdString(), {QStringLiteral("a").toStdString()}},
        {QStringLiteral("a").toStdString(), {}},
    };
    QCOMPARE(ancestorSet(rows, QStringLiteral("c").toStdString(), 100).size(), std::size_t(3));
    // Over the cap there is no useful highlight, so the walk says so instead of
    // returning an arbitrary slice.
    QVERIFY(ancestorSet(rows, QStringLiteral("c").toStdString(), 2).empty());
    QVERIFY(ancestorSet(rows, QStringLiteral("nope").toStdString(), 100).empty());
}

void GraphHoverTest::theWindowReportsAndActsOnTheGhost() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    // The first page loads on a worker; a headless check must wait for it rather
    // than race it and read an empty graph.
    QTRY_VERIFY_WITH_TIMEOUT(
        !window.describeHoverRow(0).join(QLatin1Char('\n')).contains(QStringLiteral("out of range")),
        5000);

    int ghostRow = -1;
    for (int row = 0; row < 6; ++row) {
        const QString text = window.describeHoverRow(row).join(QLatin1Char('\n'));
        if (text.contains(QStringLiteral("ghost: feature"))) {
            ghostRow = row;
            break;
        }
    }
    QVERIFY(ghostRow >= 0);

    const QString text = window.describeHoverRow(ghostRow).join(QLatin1Char('\n'));
    QVERIFY(text.contains(QStringLiteral("highlight: feature")));

    // A row that already wears a label has no ghost to act on.
    bool foundLabelled = false;
    for (int row = 0; row < 6; ++row) {
        const QString t = window.describeHoverRow(row).join(QLatin1Char('\n'));
        if (t.contains(QStringLiteral("labels "))) {
            const QString acted =
                window.activateHoverGhostForDiagnostics(row).join(QLatin1Char('\n'));
            QVERIFY(acted.contains(QStringLiteral("ghost: (none)")));
            QVERIFY(!acted.contains(QStringLiteral("branch:")));
            foundLabelled = true;
            break;
        }
    }
    QVERIFY(foundLabelled);

    const QStringList report = window.activateHoverGhostForDiagnostics(ghostRow);
    QVERIFY(report.join(QLatin1Char('\n')).contains(QStringLiteral("branch: feature")));
    QCOMPARE(gitOut(m_path, {"rev-parse", "--abbrev-ref", "HEAD"}), QStringLiteral("feature"));
}

void GraphHoverTest::collapsedBranchesAreRevealedForDiagnostics() {
    // Two more branches land on main's tip, so its pill collapses them into "+2".
    QVERIFY(git(m_path, {"branch", "extra", "main"}));
    QVERIFY(git(m_path, {"branch", "third", "main"}));

    RepoSession session;
    QVERIFY(session.open(m_path));
    CommitModel model(&session);
    model.reset();
    QTRY_VERIFY(model.rowCount() >= 4);

    CommitGraphView view;
    view.setModel(&model);

    const int tipRow = model.rowForCommit(gitOut(m_path, {"rev-parse", "main"}));
    QVERIFY(tipRow >= 0);

    const QStringList collapsed = view.hoverBranchesAt(tipRow);
    QCOMPARE(collapsed.size(), 2);
    QVERIFY(collapsed.contains(QStringLiteral("extra")));
    QVERIFY(collapsed.contains(QStringLiteral("third")));

    // The headless hover builds the same overlay a mouse would, revealing exactly
    // those branches under the pill.
    view.setHoverExpansionForDiagnostics(tipRow);
    QStringList revealed = view.expandedBranchesForDiagnostics();
    revealed.sort();
    QStringList expected = collapsed;
    expected.sort();
    QCOMPARE(revealed, expected);

    // Find a row that would otherwise show a ghost badge, while the overlay is not
    // yet in the way.
    int ghostRow = -1;
    for (int r = 0; r < model.rowCount(); ++r) {
        if (!view.ghostBranchAt(r).isEmpty()) {
            ghostRow = r;
            break;
        }
    }
    QVERIFY(ghostRow >= 0);
    view.setHoverRowForDiagnostics(ghostRow);
    QVERIFY(view.ghostBadgeRowForDiagnostics() >= 0);

    // The pointer has to cross the gap between the pill and the stack to reach the
    // branches; leaving the pill must not close the overlay on the way.
    const QRect source = view.hoverExpansionSourceRect();
    QVERIFY(source.isValid());
    view.hoverMoveForDiagnostics(QPoint(source.center().x(), source.bottom() + 1));
    QVERIFY(view.hoverExpansionOpen());

    // While the pointer is on a revealed branch, the commit's ghost badge is
    // suppressed: the reader is looking at the branches under the commit, not asking
    // which branch contains it.
    const QRect pill = view.hoverExpansionPillRect(collapsed.at(0));
    QVERIFY(pill.isValid());
    view.hoverMoveForDiagnostics(pill.center());
    QVERIFY(view.hoverExpansionOpen());
    QCOMPARE(view.ghostBadgeRowForDiagnostics(), -1);

    // A press on a revealed branch captures it as a drag source, so it can be
    // dragged for merge/rebase rather than only checked out on a double-click.
    view.pressRefForDiagnostics(pill.center());
    QCOMPARE(view.pressedRefForDiagnostics(), collapsed.at(0));

    view.hoverMoveForDiagnostics(QPoint(source.left() + 2, source.top() - 400));
    QVERIFY(!view.hoverExpansionOpen());

    const int f2Row = model.rowForCommit(gitOut(m_path, {"rev-parse", "feature"}));
    QVERIFY(f2Row >= 0);
    QVERIFY(view.hoverBranchesAt(f2Row).isEmpty());
    view.setHoverExpansionForDiagnostics(f2Row);
    QVERIFY(view.expandedBranchesForDiagnostics().isEmpty());
}

QTEST_MAIN(GraphHoverTest)
#include "test_graph_hover.moc"
