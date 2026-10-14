/// Timestamps are passed in, so the order is a property of the code and not of
/// how fast the test ran.

#include "ui/recent_repositories.h"

#include <QDateTime>
#include <QTemporaryDir>
#include <QtTest>

using namespace gk;

namespace {

QDateTime at(int hour) {
    return QDateTime(QDate(2020, 1, 1), QTime(hour, 0, 0));
}

QStringList pathsOf(const QVector<RecentRepository>& entries) {
    QStringList out;
    for (const RecentRepository& entry : entries) {
        out << entry.path;
    }
    return out;
}

} // namespace

class RecentRepositoriesTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_config;

private slots:
    void initTestCase();
    void init();

    void mostRecentComesFirst();
    void theSamePathMovesToTheFront();
    void theListIsCappedAtTen();
    void clearRemovesEverything();
    void entriesSurviveAReload();
};

void RecentRepositoriesTest::initTestCase() {
    // Its own config dir: the store is a profile write and must not touch the
    // reader's real one.
    m_config = std::make_unique<QTemporaryDir>();
    QVERIFY(m_config->isValid());
    qputenv("XDG_CONFIG_HOME", m_config->path().toUtf8());
}

void RecentRepositoriesTest::init() {
    RecentRepositories().clear();
}

void RecentRepositoriesTest::mostRecentComesFirst() {
    RecentRepositories recent;
    recent.touch(QStringLiteral("/one"), QStringLiteral("one"), at(10));
    recent.touch(QStringLiteral("/two"), QStringLiteral("two"), at(11));
    recent.touch(QStringLiteral("/three"), QStringLiteral("three"), at(12));
    QCOMPARE(pathsOf(recent.entries()),
             QStringList({QStringLiteral("/three"), QStringLiteral("/two"),
                          QStringLiteral("/one")}));
    QCOMPARE(recent.entries().front().lastUsed, at(12));
}

void RecentRepositoriesTest::theSamePathMovesToTheFront() {
    RecentRepositories recent;
    recent.touch(QStringLiteral("/one"), QStringLiteral("one"), at(10));
    recent.touch(QStringLiteral("/two"), QStringLiteral("two"), at(11));
    recent.touch(QStringLiteral("/one"), QStringLiteral("one-again"), at(12));
    QCOMPARE(recent.entries().size(), 2);
    QCOMPARE(recent.entries().front().path, QStringLiteral("/one"));
    QCOMPARE(recent.entries().front().name, QStringLiteral("one-again"));
}

void RecentRepositoriesTest::theListIsCappedAtTen() {
    RecentRepositories recent;
    for (int i = 0; i < 12; ++i) {
        recent.touch(QStringLiteral("/r%1").arg(i), QStringLiteral("r%1").arg(i), at(i));
    }
    QCOMPARE(recent.entries().size(), RecentRepositories::kMaxEntries);
    QCOMPARE(recent.entries().front().path, QStringLiteral("/r11"));
    QCOMPARE(recent.entries().back().path, QStringLiteral("/r2"));
}

void RecentRepositoriesTest::clearRemovesEverything() {
    RecentRepositories recent;
    recent.touch(QStringLiteral("/one"), QStringLiteral("one"), at(10));
    recent.clear();
    QVERIFY(recent.entries().isEmpty());
    RecentRepositories reloaded;
    reloaded.load();
    QVERIFY(reloaded.entries().isEmpty());
}

void RecentRepositoriesTest::entriesSurviveAReload() {
    {
        RecentRepositories recent;
        recent.touch(QStringLiteral("/alpha"), QStringLiteral("alpha"), at(9));
        recent.touch(QStringLiteral("/beta"), QStringLiteral("beta"), at(13));
    }
    RecentRepositories reloaded;
    reloaded.load();
    QCOMPARE(reloaded.entries().size(), 2);
    QCOMPARE(reloaded.entries().front().path, QStringLiteral("/beta"));
    QCOMPARE(reloaded.entries().front().name, QStringLiteral("beta"));
    QCOMPARE(reloaded.entries().front().lastUsed, at(13));
}

QTEST_MAIN(RecentRepositoriesTest)
#include "test_recent_repositories.moc"
