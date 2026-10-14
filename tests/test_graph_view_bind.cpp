/// The graph view is handed its model again whenever the window rebinds its views
/// (`MainWindow::bindViews`), and it is the same model every time within a tab.
///
/// Handing it the same model must be a no-op, not a one-way door. The old code
/// disconnected every connection from the model to the view and only then called
/// `QTreeView::setModel`, which returns early when the model has not changed — so
/// nothing was reconnected, the view stopped following the model, and with a dirty
/// working tree the graph showed only the working-tree row until the repository was
/// closed and opened again.

#include "core/types.h"
#include "git/cli.h"
#include "model/commit_model.h"
#include "repo/repo_session.h"
#include "ui/commit_graph_view.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <vector>

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

} // namespace

class GraphViewBindTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void rebindingTheSameModelKeepsTheViewFollowingIt();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void GraphViewBindTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    QVERIFY(writeFile(m_path + "/a.txt", QStringLiteral("base\n")));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, {"commit", "-qm", "base"}));
}

void GraphViewBindTest::cleanup() {
    m_dir.reset();
}

void GraphViewBindTest::rebindingTheSameModelKeepsTheViewFollowingIt() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    CommitModel model(&session);
    model.reset();
    QTRY_VERIFY(model.rowCount() >= 1);

    CommitGraphView view;
    view.resize(1000, 500);
    view.setModel(&model);
    // Exactly what the window does on every activation: the same model, again.
    view.setModel(&model);
    view.setModel(&model);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QTRY_VERIFY(view.visualRect(model.index(0, 0)).isValid());

    // A dirty working tree inserts its row at the front. A view that was disconnected
    // by the rebind never learns about it and keeps the stale one-row layout.
    FileChange change;
    change.path = "a.txt";
    change.kind = ChangeKind::Modified;
    change.staged = false;
    model.setWorkingTreeChanges({change});

    QTRY_COMPARE(model.rowCount(), 2);
    QTRY_VERIFY2(view.visualRect(model.index(1, 0)).isValid(),
                 "the view did not follow the model after the same-model rebind");
}

QTEST_MAIN(GraphViewBindTest)
#include "test_graph_view_bind.moc"
