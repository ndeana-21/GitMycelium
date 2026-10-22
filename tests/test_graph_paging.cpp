/// History is paged on demand: the graph view asks for the next page when the reader
/// nears the end of what is loaded, and never otherwise.
///
/// The model used to answer `canFetchMore` with "yes" whenever more history existed.
/// QTreeView asks for another page on every layout it does, and a page that lands
/// causes a layout, so an idle window walked the entire history page after page —
/// on a large monorepo a minute and a half of a stuttering window every time it was
/// opened.

#include "model/commit_model.h"
#include "repo/repo_session.h"
#include "ui/commit_graph_view.h"
#include "ui/main_window.h"

#include <QDragMoveEvent>
#include <QFile>
#include <QMimeData>
#include <QScrollBar>

#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace gk;

namespace {

constexpr int kCommits = 1600;

/// `count` commits on main, written by fast-import in one process: a commit per
/// `git commit` would take most of a minute.
bool buildHistory(const QString& dir, int count) {
    QProcess init;
    init.start(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("-q"),
                                       QStringLiteral("-b"), QStringLiteral("main"), dir});
    if (!init.waitForFinished() || init.exitCode() != 0) {
        return false;
    }
    QByteArray stream;
    for (int i = 1; i <= count; ++i) {
        const QByteArray message = "commit " + QByteArray::number(i);
        stream += "commit refs/heads/main\n";
        stream += "committer T <t@t> " + QByteArray::number(1600000000 + i) + " +0000\n";
        stream += "data " + QByteArray::number(message.size()) + "\n" + message + "\n\n";
    }
    QProcess import;
    import.setWorkingDirectory(dir);
    import.start(QStringLiteral("git"), {QStringLiteral("fast-import"), QStringLiteral("--quiet")});
    if (!import.waitForStarted()) {
        return false;
    }
    import.write(stream);
    import.closeWriteChannel();
    return import.waitForFinished(60000) && import.exitCode() == 0;
}

} // namespace

/// Reaches the protected drag handlers, as a drag over the view would.
class DragProbeView : public CommitGraphView {
public:
    using CommitGraphView::CommitGraphView;
    void move(QDragMoveEvent* event) { dragMoveEvent(event); }
    void leave(QDragLeaveEvent* event) { dragLeaveEvent(event); }
    int stepAt(int y) const { return dragScrollStepAt(y); }
};

class GraphPagingTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void anIdleViewHoldsOnePageAndScrollingLoadsTheRest();
    void aCommitRefreshesTheGraphInPlace();
    void aDragAtTheEdgeScrollsTheGraph();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
};

void GraphPagingTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    QVERIFY(buildHistory(m_dir->path(), kCommits));
}

void GraphPagingTest::cleanup() {
    m_dir.reset();
}

void GraphPagingTest::aCommitRefreshesTheGraphInPlace() {
    // A local operation must not throw the graph away: no empty graph while the walk
    // runs, the reader's place and selection kept, the new commit on top.
    MainWindow window;
    window.setHeadless(true);
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openRepository(m_dir->path()));
    auto* view = window.findChild<CommitGraphView*>();
    QVERIFY(view);
    auto* model = qobject_cast<CommitModel*>(view->model());
    QVERIFY(model);
    QTRY_VERIFY(model->realRowCount() >= 500 && !model->isLoading());
    model->fetchMore(QModelIndex());
    QTRY_VERIFY(model->realRowCount() >= 1000 && !model->isLoading());

    const int target = model->rowCount() - 300;
    const QModelIndex chosen = model->index(target, CommitModel::Message);
    view->setCurrentIndex(chosen);
    view->scrollTo(chosen, QAbstractItemView::PositionAtCenter);
    QCoreApplication::processEvents();
    const QString selected = QString::fromStdString(model->commitAt(target)->id);
    const int scroll = view->verticalScrollBar()->value();
    QVERIFY(scroll > 0);

    QFile file(m_dir->path() + QStringLiteral("/local.txt"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("local\n");
    file.close();
    window.commitPanelForDiagnostics({}, QStringLiteral("local commit"));
    QVERIFY(model->realRowCount() >= 1000);
    QTRY_VERIFY(!model->isLoading());
    QCOMPARE(QString::fromStdString(model->commitAt(0)->summary), QStringLiteral("local commit"));
    QVERIFY(model->realRowCount() > target);
    QTRY_COMPARE(view->verticalScrollBar()->value(), scroll);
    // The commit sends the panel back to the working tree, so nothing stays selected,
    // but the row the reader looked at is where it was.
    QCOMPARE(QString::fromStdString(model->commitAt(target + 1)->id), selected);
}

void GraphPagingTest::aDragAtTheEdgeScrollsTheGraph() {
    RepoSession session;
    QVERIFY(session.open(m_dir->path()));
    CommitModel model(&session);
    DragProbeView view;
    view.resize(900, 500);
    view.setModel(&model);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    model.reset();
    QTRY_VERIFY(model.rowCount() > 0 && !model.isLoading());

    const int height = view.viewport()->height();
    QCOMPARE(view.stepAt(height / 2), 0);
    QVERIFY(view.stepAt(2) < 0);
    QVERIFY(view.stepAt(height - 2) > 0);
    // Deeper into the band is faster.
    QVERIFY(view.stepAt(height - 2) > view.stepAt(height - 20));

    QMimeData mime;
    mime.setData(QStringLiteral("application/x-gitmycelium-commits"), "0000000");
    QDragMoveEvent nearBottom(QPoint(view.viewport()->width() / 2, height - 3), Qt::CopyAction,
                              &mime, Qt::LeftButton, Qt::NoModifier);
    view.move(&nearBottom);
    QScrollBar* bar = view.verticalScrollBar();
    QTRY_VERIFY(bar->value() > 0);

    // Back in the middle the graph stays where it got to.
    QDragMoveEvent middle(QPoint(view.viewport()->width() / 2, height / 2), Qt::CopyAction,
                          &mime, Qt::LeftButton, Qt::NoModifier);
    view.move(&middle);
    const int stopped = bar->value();
    QTest::qWait(100);
    QCOMPARE(bar->value(), stopped);

    // Up near the top it goes back; leaving with no drag alive stops it.
    QDragMoveEvent nearTop(QPoint(view.viewport()->width() / 2, 2), Qt::CopyAction, &mime,
                           Qt::LeftButton, Qt::NoModifier);
    view.move(&nearTop);
    QTRY_VERIFY(bar->value() < stopped);
    QDragLeaveEvent leave;
    view.leave(&leave);
    const int left = bar->value();
    QTest::qWait(100);
    QCOMPARE(bar->value(), left);
}

void GraphPagingTest::anIdleViewHoldsOnePageAndScrollingLoadsTheRest() {
    RepoSession session;
    QVERIFY(session.open(m_dir->path()));
    CommitModel model(&session);

    CommitGraphView view;
    view.resize(900, 500);
    view.setModel(&model);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    model.reset();

    const int page = static_cast<int>(RepoSession::kPageSize);
    QTRY_COMPARE(model.rowCount(), page);
    // Left alone, the view must not go on paging: layouts happen, nothing scrolls.
    QTest::qWait(1000);
    QCOMPARE(model.rowCount(), page);
    QVERIFY(model.hasMore());

    for (int attempt = 0; attempt < 50 && model.hasMore(); ++attempt) {
        view.scrollToBottom();
        const int before = model.rowCount();
        QTRY_VERIFY(model.rowCount() > before || !model.hasMore());
    }
    QCOMPARE(model.rowCount(), kCommits);
    QVERIFY(!model.hasMore());
}

QTEST_MAIN(GraphPagingTest)
#include "test_graph_paging.moc"
