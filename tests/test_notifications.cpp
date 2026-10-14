#include "git/cli.h"
#include "ui/main_window.h"
#include "ui/notification_log.h"
#include "ui/profiles.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

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

} // namespace

class NotificationsTest : public QObject {
    Q_OBJECT

private slots:
    void newestIsFirstAndProblemsAreCounted();
    void theHistoryIsBounded();
    void clearEmptiesIt();
    void anOperationResultLandsInTheCentre();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void NotificationsTest::newestIsFirstAndProblemsAreCounted() {
    NotificationLog log;
    log.add(NotificationLog::Info, QStringLiteral("fetched"));
    log.add(NotificationLog::Error, QStringLiteral("push failed"));
    log.add(NotificationLog::Success, QStringLiteral("merged"));

    QCOMPARE(log.entries().size(), 3);
    QCOMPARE(log.entries().front().text, QStringLiteral("merged"));
    QCOMPARE(log.entries().back().text, QStringLiteral("fetched"));
    QCOMPARE(log.problemCount(), 1);
    log.add(NotificationLog::Info, QStringLiteral("   "));
    QCOMPARE(log.entries().size(), 3);
}

void NotificationsTest::theHistoryIsBounded() {
    NotificationLog log;
    for (int i = 0; i < NotificationLog::kCapacity + 25; ++i) {
        log.add(NotificationLog::Info, QStringLiteral("entry %1").arg(i));
    }
    QCOMPARE(log.entries().size(), NotificationLog::kCapacity);
    QCOMPARE(log.entries().front().text,
             QStringLiteral("entry %1").arg(NotificationLog::kCapacity + 24));
}

void NotificationsTest::clearEmptiesIt() {
    NotificationLog log;
    log.add(NotificationLog::Warning, QStringLiteral("careful"));
    log.clear();
    QVERIFY(log.entries().isEmpty());
    QCOMPARE(log.problemCount(), 0);
}

void NotificationsTest::anOperationResultLandsInTheCentre() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));

    GitProfile profile;
    profile.name = QStringLiteral("NotifyTest");
    profile.userName = QStringLiteral("Ada");
    profile.email = QStringLiteral("ada@work");
    profiles::save(profile);

    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    window.profilesForDiagnostics(QStringLiteral("use"), QStringLiteral("NotifyTest"));

    const QStringList report = window.notificationsForDiagnostics(QStringLiteral("list"));
    const QString joined = report.join(QLatin1Char('\n'));
    QVERIFY2(joined.contains(QStringLiteral("count: 1")), qPrintable(joined));
    QVERIFY2(joined.contains(QStringLiteral("problem")), qPrintable(joined));
    QVERIFY2(joined.contains(QStringLiteral("level=1")), qPrintable(joined));

    const QStringList cleared = window.notificationsForDiagnostics(QStringLiteral("clear"));
    QVERIFY2(cleared.join(QLatin1Char('\n')).contains(QStringLiteral("count: 0")),
             qPrintable(cleared.join(QLatin1Char('\n'))));

    profiles::remove(QStringLiteral("NotifyTest"));
}

QTEST_MAIN(NotificationsTest)
#include "test_notifications.moc"
