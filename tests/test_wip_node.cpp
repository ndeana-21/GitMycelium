/// The working-tree row — "// WIP" — lives at the top of the graph.
///
/// It is synthetic: there is no commit behind it, so the two things worth
/// asserting are that it appears exactly when the tree is dirty, at row 0, and
/// that nothing treats it as a commit: no reference points at it, a branch cannot
/// be dropped on it, and selecting it shows the changes rather than a commit diff.

#include "git/cli.h"
#include "model/commit_model.h"
#include "model/ref_model.h"
#include "theme/theme.h"
#include "ui/commit_details_panel.h"
#include "ui/commit_graph_view.h"
#include "ui/graph_delegate.h"
#include "ui/main_window.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QMimeData>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
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

bool writeFile(const QString& path, const QByteArray& content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    return f.write(content) == content.size();
}

} // namespace

class WipNodeTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;

    void cleanRepo() {
        git(m_path, {"init", "-q", "-b", "main"});
        git(m_path, {"config", "user.name", "Demo"});
        git(m_path, {"config", "user.email", "demo@example.com"});
        git(m_path, {"config", "commit.gpgsign", "false"});
        QVERIFY(writeFile(m_path + "/app.txt", "line 1\n"));
        QVERIFY(git(m_path, {"add", "-A"}));
        QVERIFY(git(m_path, {"commit", "-qm", "first"}));
    }

    void makeDirty() { QVERIFY(writeFile(m_path + "/dirty.txt", "not committed\n")); }

    static CommitModel* modelOf(MainWindow& window) {
        auto* view = window.findChild<CommitGraphView*>();
        return view ? qobject_cast<CommitModel*>(view->model()) : nullptr;
    }

private slots:
    void init();
    void cleanup();

    void aCleanWorkingTreeHasNoWipRow();
    void theWorkingTreeIsTheTopRowWhenDirty();
    void theWipRowIsNotACommit();
    void selectingTheWipRowShowsTheChanges();
    void aBranchCannotBeDroppedOnTheWipRow();
    void theRowFollowsTheChanges();
    void eachDirtyWorktreeGetsItsOwnNode();
    void aConflictedTreeIsAMergeConflictNode();
    void aStashTipIsAStashNode();
    void theWorktreeNodesWearTheirOwnBranchColours();
    void theWorkingTreeRingWearsTheBranchColour();
};

void WipNodeTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    cleanRepo();
}

void WipNodeTest::cleanup() {
    m_dir.reset();
}

void WipNodeTest::aCleanWorkingTreeHasNoWipRow() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    CommitModel* model = modelOf(window);
    QVERIFY(model);
    QTRY_VERIFY(model->rowCount() >= 1);
    QVERIFY(!model->hasWip());
    QCOMPARE(model->wipRow(), -1);
    QVERIFY(QString::fromStdString(model->commitAt(0)->id) != CommitModel::wipId());
}

void WipNodeTest::theWorkingTreeIsTheTopRowWhenDirty() {
    makeDirty();
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    CommitModel* model = modelOf(window);
    QVERIFY(model);
    // The row is inserted from status alone, before any page has arrived.
    QVERIFY(model->hasWip());
    QCOMPARE(model->wipRow(), 0);
    QTRY_VERIFY(model->rowCount() >= 2);
    QCOMPARE(QString::fromStdString(model->commitAt(0)->id), CommitModel::wipId());
    QVERIFY(QString::fromStdString(model->commitAt(1)->id) != CommitModel::wipId());
    QVERIFY(model->data(model->index(0, CommitModel::Message), Qt::DisplayRole)
                .toString()
                .startsWith(QStringLiteral("// WIP")));
}

void WipNodeTest::theWipRowIsNotACommit() {
    makeDirty();
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    CommitModel* model = modelOf(window);
    QVERIFY(model);
    QVERIFY(model->hasWip());
    QVERIFY(model->refsAt(0).empty());
    QVERIFY(model->data(model->index(0, 0), CommitModel::WipRole).toBool());
    QVERIFY(!model->data(model->index(1, 0), CommitModel::WipRole).toBool());
    const std::vector<Oid> line = model->headLineage();
    QVERIFY(std::find(line.begin(), line.end(), CommitModel::wipId().toStdString()) == line.end());
}

void WipNodeTest::selectingTheWipRowShowsTheChanges() {
    makeDirty();
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    auto* view = window.findChild<CommitGraphView*>();
    auto* panel = window.findChild<CommitDetailsPanel*>();
    CommitModel* model = modelOf(window);
    QVERIFY(view && panel && model);
    QTRY_VERIFY(model->rowCount() >= 2);

    // Start on a commit, then move to the working-tree row: the panel must switch
    // from a commit to the changes waiting to be committed.
    view->setCurrentIndex(model->index(1, 0));
    QCoreApplication::processEvents();
    QVERIFY(!panel->showingWorkingTree());
    view->setCurrentIndex(model->index(0, 0));
    QCoreApplication::processEvents();
    QVERIFY(panel->showingWorkingTree());
}

void WipNodeTest::aBranchCannotBeDroppedOnTheWipRow() {
    makeDirty();
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    CommitModel* model = modelOf(window);
    QVERIFY(model);
    QTRY_VERIFY(model->rowCount() >= 2);

    RefInfo branch;
    branch.fullName = "refs/heads/main";
    branch.shortName = "main";
    branch.kind = RefKind::LocalBranch;
    branch.target = "0";
    const std::unique_ptr<QMimeData> payload(RefModel::mimeDataForRef(branch));
    QVERIFY(payload);

    // Row 0 is refused, a real commit row is not: the refusal is about the row.
    QVERIFY(!model->canDropMimeData(payload.get(), Qt::CopyAction, 0, 0, QModelIndex()));
    QVERIFY(model->canDropMimeData(payload.get(), Qt::CopyAction, 1, 0, QModelIndex()));
}

void WipNodeTest::theRowFollowsTheChanges() {
    makeDirty();
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    CommitModel* model = modelOf(window);
    QVERIFY(model);
    QVERIFY(model->hasWip());
    QTRY_VERIFY(model->rowCount() >= 2);
    const int withWip = model->rowCount();

    // A clean tree removes it, and it comes back with the next change. This is the
    // transition the window drives through setWorkingTreeChanges on every refresh.
    model->setWorkingTreeChanges({});
    QVERIFY(!model->hasWip());
    QCOMPARE(model->rowCount(), withWip - 1);
    QVERIFY(QString::fromStdString(model->commitAt(0)->id) != CommitModel::wipId());

    FileChange change;
    change.path = "dirty.txt";
    change.kind = ChangeKind::Untracked;
    model->setWorkingTreeChanges({change});
    QVERIFY(model->hasWip());
    QCOMPARE(model->wipRow(), 0);
    QCOMPARE(model->rowCount(), withWip);
    QCOMPARE(QString::fromStdString(model->commitAt(0)->id), CommitModel::wipId());
}

void WipNodeTest::eachDirtyWorktreeGetsItsOwnNode() {
    // A second checkout with its own commit, so the two WIP nodes hang off
    // different HEads and the test can tell them apart.
    const QString wt = QDir(m_path).filePath(QStringLiteral("wt"));
    QVERIFY(git(m_path, {"worktree", "add", "-q", wt, "-b", "wtmain"}));
    QVERIFY(writeFile(wt + "/wt.txt", "from the worktree\n"));
    QVERIFY(git(wt, {"add", "-A"}));
    QVERIFY(git(wt, {"commit", "-qm", "worktree commit"}));

    makeDirty();                                  // main: dirty
    QVERIFY(writeFile(wt + "/more.txt", "x\n"));  // worktree: dirty
    const QString mainHead = QString::fromStdString(
        runGit({"rev-parse", "HEAD"}, m_path.toStdString()).out).trimmed();
    const QString wtHead =
        QString::fromStdString(runGit({"rev-parse", "HEAD"}, wt.toStdString()).out).trimmed();
    QVERIFY(mainHead != wtHead);

    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    CommitModel* model = modelOf(window);
    QVERIFY(model);
    QTRY_VERIFY(model->rowCount() >= 3);

    // One node per dirty worktree, the current checkout first, and it has to be
    // the row existing callers mean by "my working tree".
    QVERIFY(model->hasWip());
    QCOMPARE(model->wipRow(), 0);
    QVERIFY(model->data(model->index(0, 0), CommitModel::WipRole).toBool());
    QVERIFY(model->data(model->index(1, 0), CommitModel::WipRole).toBool());
    QVERIFY(!model->data(model->index(2, 0), CommitModel::WipRole).toBool());
    QCOMPARE(model->data(model->index(1, 0), CommitModel::WipNameRole).toString(),
             QStringLiteral("wt"));
    QCOMPARE(model->data(model->index(0, 0), CommitModel::WipWorktreeRole).toString(),
             QDir::cleanPath(m_path));

    QCOMPARE(QString::fromStdString(model->commitAt(0)->parents.front()), mainHead);
    QCOMPARE(QString::fromStdString(model->commitAt(1)->parents.front()), wtHead);
}

void WipNodeTest::aConflictedTreeIsAMergeConflictNode() {
    // Two branches edit the same line; the merge stops with a conflict in the
    // index, which is the state the row must wear as its node type.
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "other"}));
    QVERIFY(writeFile(m_path + "/app.txt", "other side\n"));
    QVERIFY(git(m_path, {"commit", "-qam", "other"}));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
    QVERIFY(writeFile(m_path + "/app.txt", "main side\n"));
    QVERIFY(git(m_path, {"commit", "-qam", "main"}));
    git(m_path, {"merge", "other"});

    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    CommitModel* model = modelOf(window);
    QVERIFY(model);
    QTRY_VERIFY(model->hasWip());

    const GraphNode* node = model->graphNodeAt(model->wipRow());
    QVERIFY(node);
    QCOMPARE(node->type, GraphNodeType::MergeConflict);
}

void WipNodeTest::aStashTipIsAStashNode() {
    QVERIFY(writeFile(m_path + "/app.txt", "stashed change\n"));
    QVERIFY(git(m_path, {"stash", "push", "-q"}));
    const QString stash = QString::fromStdString(
        runGit({"rev-parse", "refs/stash"}, m_path.toStdString()).out).trimmed();
    QVERIFY(!stash.isEmpty());

    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    CommitModel* model = modelOf(window);
    QVERIFY(model);
    QTRY_VERIFY(model->rowForCommit(stash) >= 0);

    const GraphNode* node = model->graphNodeAt(model->rowForCommit(stash));
    QVERIFY(node);
    QCOMPARE(node->type, GraphNodeType::Stash);
    // The stash commit is a merge of HEAD and the index, but the index commit is how
    // git stores a stash, not a commit of anyone's: it hangs off HEAD alone.
    QVERIFY(!node->isMerge);
    const QString index = QString::fromStdString(
        runGit({"rev-parse", "refs/stash^2"}, m_path.toStdString()).out).trimmed();
    QCOMPARE(model->rowForCommit(index), -1);
}

void WipNodeTest::theWorktreeNodesWearTheirOwnBranchColours() {
    // Two checkouts, each dirty, each on its own branch — so their working-tree nodes
    // hang off different tips and must take different colours. The second node is the
    // one that matters: it is not the top row, so it does not land on the first lane.
    const QString wt = QDir(m_path).filePath(QStringLiteral("wt"));
    QVERIFY(git(m_path, {"worktree", "add", "-q", wt, "-b", "wtmain"}));
    QVERIFY(writeFile(wt + "/wt.txt", "from the worktree\n"));
    QVERIFY(git(wt, {"add", "-A"}));
    QVERIFY(git(wt, {"commit", "-qm", "worktree commit"}));

    // A third branch somewhere else, so `wtmain`'s palette index is *not* the next
    // free lane colour. Without it the two coincide and the assertion below would
    // hold even for a node that had been handed a colour of its own.
    QVERIFY(writeFile(m_path + "/far.txt", "far\n"));
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "far", "main"}));
    QVERIFY(git(m_path, {"add", "far.txt"}));
    QVERIFY(git(m_path, {"commit", "-qm", "far"}));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));

    makeDirty();                                 // main: dirty
    QVERIFY(writeFile(wt + "/more.txt", "x\n")); // worktree: dirty

    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    CommitModel* model = modelOf(window);
    QVERIFY(model);
    QTRY_VERIFY(model->rowCount() >= 3);

    QVERIFY(model->data(model->index(0, 0), CommitModel::WipRole).toBool());
    QVERIFY(model->data(model->index(1, 0), CommitModel::WipRole).toBool());

    const GraphNode* first = model->graphNodeAt(0);
    const GraphNode* second = model->graphNodeAt(1);
    QVERIFY(first);
    QVERIFY(second);

    // Each node is the tip of the branch its own worktree is on, and wears that
    // branch's colour: a fresh colour is what made uncommitted changes read as a
    // separate branch hanging beside the one they belong to.
    QCOMPARE(first->colorIndex, model->refColorIndex(QStringLiteral("main")));
    QCOMPARE(second->colorIndex, model->refColorIndex(QStringLiteral("wtmain")));
    QVERIFY(first->colorIndex != second->colorIndex);
}

void WipNodeTest::theWorkingTreeRingWearsTheBranchColour() {
    makeDirty();
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    CommitModel* model = modelOf(window);
    QVERIFY(model);
    QVERIFY(model->hasWip());
    QTRY_VERIFY(model->rowCount() >= 2);

    const int wipRow = model->wipRow();
    QVERIFY(wipRow >= 0);
    const GraphNode* node = model->graphNodeAt(wipRow);
    QVERIFY(node);
    QCOMPARE(node->type, GraphNodeType::WorkDir);

    // Paint the row the way the view does and read the pixel back: the complaint was
    // about what the ring looks like, and an accent-coloured ring on the branch's own
    // line reads as a second branch. A colour-index assertion cannot catch this — the
    // delegate used to ignore the index and draw the accent colour regardless.
    const QRect cell(0, 0, 220, 30);
    QImage image(cell.size(), QImage::Format_ARGB32);
    image.fill(theme::background());
    {
        QPainter painter(&image);
        GraphDelegate delegate;
        QStyleOptionViewItem option;
        option.rect = cell;
        option.font = QApplication::font();
        delegate.paint(&painter, option, model->index(wipRow, CommitModel::Graph));
    }

    // The ring's right edge is a clean sample point: the lane's own line runs through
    // the node's centre, so a pixel off to the side is the ring and not the line.
    const int centerX = cell.left() + GraphDelegate::lanePadding() +
                        GraphDelegate::laneWidth() * node->lane;
    const int centerY = cell.center().y();
    const int ringX = centerX + GraphDelegate::nodeRadius();

    const auto distance = [](const QColor& a, const QColor& b) {
        const int dr = a.red() - b.red();
        const int dg = a.green() - b.green();
        const int db = a.blue() - b.blue();
        return dr * dr + dg * dg + db * db;
    };

    // The stroke is a couple of pixels wide and anti-aliased, so take the neighbour
    // furthest from the row's background: that one is the ring itself.
    QColor ring = image.pixelColor(ringX, centerY);
    int best = distance(ring, theme::background());
    for (int dx = -2; dx <= 2; ++dx) {
        for (int dy = -2; dy <= 2; ++dy) {
            const QColor candidate = image.pixelColor(ringX + dx, centerY + dy);
            const int d = distance(candidate, theme::background());
            if (d > best) {
                best = d;
                ring = candidate;
            }
        }
    }

    const QColor branch = theme::branchColor(node->colorIndex);
    QVERIFY2(distance(ring, branch) < distance(ring, theme::accent()),
             qPrintable(QStringLiteral("ring %1: branch %2, accent %3")
                            .arg(ring.name(), branch.name(), theme::accent().name())));
}

QTEST_MAIN(WipNodeTest)
#include "test_wip_node.moc"
