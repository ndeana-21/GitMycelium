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

class GraphPagingTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void anIdleViewHoldsOnePageAndScrollingLoadsTheRest();

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

    // Reaching the end of what is loaded asks for more, until the history runs out.
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
