#include "git/cli.h"
#include "model/commit_model.h"
#include "ui/commit_graph_view.h"
#include "ui/main_window.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

#include <cstdio>

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

class GraphSelectionTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;

    bool commitFile(const QString& name, const QByteArray& content, const QString& message) {
        if (!writeFile(m_path + "/" + name, content)) {
            return false;
        }
        return git(m_path, {"add", "-A"}) && git(m_path, {"commit", "-qm", message});
    }

    QString id(const QString& revision) {
        return QString::fromStdString(
               runGit(std::vector<std::string>{"rev-parse", revision.toStdString()},
                      m_path.toStdString())
                   .out)
        .trimmed();
    }

    /// The row a commit is drawn at, or -1. The graph interleaves branches, so a row number
    /// is not something a test may assume — it is asked for.
    int rowOf(CommitGraphView* view, const QString& commitId) {
        auto* model = qobject_cast<CommitModel*>(view->model());
        return model ? model->rowForCommit(commitId) : -1;
    }

    void clickRow(CommitGraphView* view, int row, Qt::KeyboardModifiers modifiers,
                  Qt::MouseButton button = Qt::LeftButton) {
        auto* model = qobject_cast<CommitModel*>(view->model());
        QVERIFY(model && row >= 0);
        // Selecting a commit hands the centre of the window to its diff, which hides the
        // graph and collapses its viewport to nothing — the next click would be aimed at a
        // zero-height widget and select nothing, which reads exactly like a bug in the
        // selection code. Escape is the documented way back to the graph.
        if (view->viewport()->height() < 200) {
            QTest::keyClick(view->window(), Qt::Key_Escape);
        }
        QTRY_VERIFY_WITH_TIMEOUT(view->viewport()->height() > 200, 5000);
        // Bring it into view first. A row scrolled out of sight has a rectangle outside the
        // viewport, a click there selects nothing, and the failure then looks like a bug in
        // the selection code rather than in the test's aim.
        view->scrollTo(model->index(row, CommitModel::Message));
        QCoreApplication::processEvents();
        // Not the Refs column: a click there lands on a branch label and is a different
        // gesture entirely. Not Graph either, which has its own handling. The point is
        // clamped into the viewport, because a column can be only partly visible.
        const QRect rect = view->visualRect(model->index(row, CommitModel::Message));
        QVERIFY(rect.isValid());
        QPoint point = rect.center();
        const QRect visible = view->viewport()->rect();
        point.setX(std::clamp(point.x(), visible.left() + 1, visible.right() - 1));
        point.setY(std::clamp(point.y(), visible.top() + 1, visible.bottom() - 1));
        QTest::mouseClick(view->viewport(), button, modifiers, point);
    }

private slots:
    void init();
    void cleanup();

    void shiftTakesEverythingBetweenTheTwoClicks();
    void squashingTheRootAndItsChildIsAllowed();
};

void GraphSelectionTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    git(m_path, {"init", "-q", "-b", "main"});
    git(m_path, {"config", "user.name", "Demo"});
    git(m_path, {"config", "user.email", "demo@example.com"});
    git(m_path, {"config", "commit.gpgsign", "false"});
    for (int step = 1; step <= 5; ++step) {
        QVERIFY(commitFile("app.txt",
                           QStringLiteral("line %1\n").arg(step).toUtf8(),
                           QStringLiteral("c%1: step %1").arg(step)));
    }
    QVERIFY(git(m_path, {"checkout", "-q", "-b", "side", "main~3"}));
    QVERIFY(commitFile("s.txt", "s1\n", "side: first"));
    QVERIFY(commitFile("s.txt", "s2\n", "side: second (tip)"));
    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
}

void GraphSelectionTest::cleanup() {
    m_dir.reset();
}

void GraphSelectionTest::shiftTakesEverythingBetweenTheTwoClicks() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    // A window that was never shown has a viewport a few pixels wide, and a click aimed at
    // a row then lands outside it — the selection stays empty and the test blames the code.
    window.resize(1400, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    CommitGraphView* view = window.findChild<CommitGraphView*>();
    QVERIFY(view);
    QTRY_VERIFY(view->model() && view->model()->rowCount() >= 7);
    QTRY_VERIFY(view->viewport()->height() > 200);


    const int newest = rowOf(view, id("main"));
    const int third = rowOf(view, id("main~2"));
    QVERIFY(newest >= 0 && third >= 0);
    QVERIFY(newest != third);

    clickRow(view, third, Qt::NoModifier);
    QCOMPARE(view->selectedRows().size(), 1);
    clickRow(view, newest, Qt::ShiftModifier);

    QVector<int> expected;
    for (int row = std::min(newest, third); row <= std::max(newest, third); ++row) {
        expected.push_back(row);
    }
    QCOMPARE(view->selectedRows(), expected);

    // And every commit between the two clicks is in there, not merely the right number of
    // rows: the graph interleaves branches, so a count can be right while the commits are
    // wrong.
    auto* model = qobject_cast<CommitModel*>(view->model());
    QVERIFY(model);
    QStringList selectedIds;
    for (const int row : view->selectedRows()) {
        selectedIds << QString::fromStdString(model->commitAt(row)->id).left(7);
    }
    QStringList expectedIds;
    for (const int row : expected) {
        expectedIds << QString::fromStdString(model->commitAt(row)->id).left(7);
    }
    QCOMPARE(selectedIds, expectedIds);
    // The commits clicked, and not only the rows between them: an engine that selected a
    // neighbouring range would satisfy the row check and fail this one.
    QVERIFY(selectedIds.contains(id("main").left(7)));
    QVERIFY(selectedIds.contains(id("main~2").left(7)));
}




void GraphSelectionTest::squashingTheRootAndItsChildIsAllowed() {
    QTemporaryDir repo;
    QVERIFY(repo.isValid());
    const QString path = repo.path();
    QVERIFY(git(path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(path, {"config", "user.name", "Demo"}));
    QVERIFY(git(path, {"config", "user.email", "demo@example.com"}));
    QVERIFY(writeFile(path + QStringLiteral("/a.txt"), QByteArray("one\n")));
    QVERIFY(git(path, {"add", "-A"}) && git(path, {"commit", "-qm", "c1"}));
    QVERIFY(writeFile(path + QStringLiteral("/a.txt"), QByteArray("two\n")));
    QVERIFY(git(path, {"add", "-A"}) && git(path, {"commit", "-qm", "c2"}));

    MainWindow window;
    // The squash asks for confirmation; headless, the window answers it instead of
    // waiting on a dialog nobody can click.
    window.setHeadless(true);
    QVERIFY(window.openRepository(path));
    QTRY_VERIFY_WITH_TIMEOUT(window.commitsAreLoaded(), 5000);

    const QStringList plan = window.describeSelection(QVector<int>{0, 1});
    const QString planText = plan.join(QLatin1Char('\n'));
    QVERIFY2(planText.contains(QStringLiteral("squash: yes")), qPrintable(planText));

    const QStringList run =
        window.runSelectionForDiagnostics(QVector<int>{0, 1}, QStringLiteral("squash"));
    const QString runText = run.join(QLatin1Char('\n'));
    QVERIFY2(runText.contains(QStringLiteral("in progress: no")), qPrintable(runText));

    const QString count =
        QString::fromStdString(runGit({"rev-list", "--count", "HEAD"}, path.toStdString()).out)
            .trimmed();
    QCOMPARE(count, QStringLiteral("1"));
}

QTEST_MAIN(GraphSelectionTest)
#include "test_graph_selection.moc"
