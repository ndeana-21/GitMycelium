#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

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

QString gitOut(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, dir.toStdString()).out).trimmed();
}

} // namespace

class HistoryPagingTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void pagesAreContiguousAndComplete();
    void hasMoreIsExactAtAPageBoundary();
    void aDroppedWalkSeesAMovedRef();

private:
    bool commit(const QString& name);

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void HistoryPagingTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
}

void HistoryPagingTest::cleanup() {
    m_dir.reset();
}

bool HistoryPagingTest::commit(const QString& name) {
    QFile file(QDir(m_path).filePath(name + QStringLiteral(".txt")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(name.toUtf8());
    file.close();
    return git(m_path, {"add", "-A"}) &&
           git(m_path, {"commit", "-q", "-m", name});
}

void HistoryPagingTest::pagesAreContiguousAndComplete() {
    for (int i = 1; i <= 25; ++i) {
        QVERIFY(commit(QStringLiteral("c%1").arg(i)));
    }
    const QStringList expected = gitOut(m_path, {"rev-list", "HEAD"}).split(QLatin1Char('\n'));

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    QStringList paged;
    const quint32 pageSize = 10;
    for (quint64 skip = 0;; skip += pageSize) {
        bool hasMore = false;
        const std::vector<CommitInfo> page = session.commitsBlocking(skip, pageSize, &hasMore);
        for (const CommitInfo& ci : page) {
            paged << QString::fromStdString(ci.id);
        }
        if (!hasMore) {
            break;
        }
    }
    QCOMPARE(paged, expected);
}

void HistoryPagingTest::hasMoreIsExactAtAPageBoundary() {
    for (int i = 1; i <= 20; ++i) {
        QVERIFY(commit(QStringLiteral("c%1").arg(i)));
    }

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    bool hasMore = false;
    QCOMPARE(session.commitsBlocking(0, 10, &hasMore).size(), std::size_t(10));
    QVERIFY2(hasMore, "ten more commits are left");

    QCOMPARE(session.commitsBlocking(10, 10, &hasMore).size(), std::size_t(10));
    QVERIFY2(!hasMore, "the second page ends the history exactly");
}

void HistoryPagingTest::aDroppedWalkSeesAMovedRef() {
    for (int i = 1; i <= 5; ++i) {
        QVERIFY(commit(QStringLiteral("c%1").arg(i)));
    }

    RepoSession session;
    QString error;
    QVERIFY2(session.open(m_path, &error), qPrintable(error));

    bool hasMore = false;
    QCOMPARE(session.commitsBlocking(0, 2, &hasMore).size(), std::size_t(2));

    QVERIFY(commit(QStringLiteral("c6")));

    // Without the drop the cached walk hands out the pre-commit rows.
    session.invalidateCommitWalk();
    const std::vector<CommitInfo> page = session.commitsBlocking(2, 2, &hasMore);
    const QStringList expected = gitOut(m_path, {"rev-list", "HEAD"}).split(QLatin1Char('\n'));
    QCOMPARE(page.size(), std::size_t(2));
    QCOMPARE(QString::fromStdString(page[0].id), expected.at(2));
    QCOMPARE(QString::fromStdString(page[1].id), expected.at(3));
}

QTEST_MAIN(HistoryPagingTest)
#include "test_history_paging.moc"
