#include "core/types.h"
#include "git/cli.h"
#include "model/commit_model.h"
#include "repo/repo_session.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace gk;

namespace {

bool git(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, dir.toStdString()).exitCode == 0;
}

} // namespace

class CommitPagingTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aPageFromBeforeAResetIsIgnored();
    void aFailedSearchDoesNotStopTheHistory();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void CommitPagingTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    for (int i = 1; i <= 3; ++i) {
        QVERIFY(git(m_path, {"commit", "-q", "--allow-empty", "-m", QStringLiteral("c%1").arg(i)}));
    }
}

void CommitPagingTest::cleanup() {
    m_dir.reset();
}

void CommitPagingTest::aPageFromBeforeAResetIsIgnored() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    CommitModel model(&session);
    model.setTimelinePeriod(TimelinePeriod::None);
    model.reset();
    QTRY_VERIFY(!model.isLoading());

    // A reset starts again from row 0, so a page still in flight from before it has
    // the `skip` the new history expects. Only its generation tells it apart.
    model.reset();
    CommitPage stale;
    stale.generation = 9999;
    stale.skip = 0;
    CommitInfo ghost;
    ghost.id = "0123456789abcdef0123456789abcdef01234567";
    ghost.summary = "from the history before the reset";
    stale.commits.push_back(ghost);
    emit session.commitsLoaded(stale);
    QVERIFY2(model.isLoading(), "a stale page cleared the flag of the request in flight");

    QTRY_VERIFY(!model.isLoading());
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.rowForCommit(QString::fromStdString(ghost.id)), -1);
}

void CommitPagingTest::aFailedSearchDoesNotStopTheHistory() {
    RepoSession session;
    QVERIFY(session.open(m_path));
    CommitModel model(&session);
    model.reset();
    QVERIFY(model.isLoading());

    // The search reports its failures on the session's shared signal; the history
    // model used to take that as its own page failing and stopped paging for good.
    emit session.requestFailed(QStringLiteral("search failed"));
    QVERIFY(model.isLoading());
    QTRY_VERIFY(!model.isLoading());
    QVERIFY(model.rowCount() >= 3);
}

QTEST_MAIN(CommitPagingTest)
#include "test_commit_paging.moc"
