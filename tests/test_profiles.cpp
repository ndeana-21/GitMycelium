#include "git/cli.h"
#include "ui/main_window.h"
#include "ui/profiles.h"

#include <QDir>
#include <QFile>
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

QString configValue(const QString& dir, const QString& key) {
    return QString::fromStdString(runGit({"config", "--local", "--get", key.toStdString()}, dir.toStdString()).out)
        .trimmed();
}

} // namespace

class ProfilesTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void definitionsRoundTrip();
    void applyingAProfileWritesTheLocalConfig();
    void switchingProfilesRewritesTheConfig();
    void theActiveProfileIsRememberedPerRepository();

private:
    GitProfile make(const QString& name, const QString& user, const QString& email) {
        GitProfile profile;
        profile.name = name;
        profile.userName = user;
        profile.email = email;
        return profile;
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void ProfilesTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
}

void ProfilesTest::cleanup() {
    // The definitions are global settings, so the test owns the names it made.
    profiles::remove(QStringLiteral("Work"));
    profiles::remove(QStringLiteral("Personal"));
    m_dir.reset();
}

void ProfilesTest::definitionsRoundTrip() {
    QVERIFY(!profiles::names().contains(QStringLiteral("Work")));
    profiles::save(make(QStringLiteral("Work"), QStringLiteral("Ada"), QStringLiteral("ada@work")));
    QVERIFY(profiles::names().contains(QStringLiteral("Work")));
    const GitProfile loaded = profiles::load(QStringLiteral("Work"));
    QCOMPARE(loaded.userName, QStringLiteral("Ada"));
    QCOMPARE(loaded.email, QStringLiteral("ada@work"));

    profiles::save(make(QStringLiteral("Work"), QStringLiteral("Ada"), QStringLiteral("new@work")));
    QCOMPARE(profiles::names().count(QStringLiteral("Work")), 1);
    QCOMPARE(profiles::load(QStringLiteral("Work")).email, QStringLiteral("new@work"));

    profiles::remove(QStringLiteral("Work"));
    QVERIFY(!profiles::names().contains(QStringLiteral("Work")));
}

void ProfilesTest::applyingAProfileWritesTheLocalConfig() {
    profiles::save(make(QStringLiteral("Work"), QStringLiteral("Ada"), QStringLiteral("ada@work")));

    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    window.profilesForDiagnostics(QStringLiteral("use"), QStringLiteral("Work"));

    QCOMPARE(configValue(m_path, QStringLiteral("user.name")), QStringLiteral("Ada"));
    QCOMPARE(configValue(m_path, QStringLiteral("user.email")), QStringLiteral("ada@work"));
    QCOMPARE(configValue(m_path, QStringLiteral("commit.gpgsign")), QStringLiteral("false"));
}

void ProfilesTest::switchingProfilesRewritesTheConfig() {
    profiles::save(make(QStringLiteral("Work"), QStringLiteral("Ada"), QStringLiteral("ada@work")));
    profiles::save(
        make(QStringLiteral("Personal"), QStringLiteral("Grace"), QStringLiteral("grace@home")));

    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    window.profilesForDiagnostics(QStringLiteral("use"), QStringLiteral("Work"));
    QCOMPARE(configValue(m_path, QStringLiteral("user.email")), QStringLiteral("ada@work"));

    window.profilesForDiagnostics(QStringLiteral("use"), QStringLiteral("Personal"));
    QCOMPARE(configValue(m_path, QStringLiteral("user.name")), QStringLiteral("Grace"));
    QCOMPARE(configValue(m_path, QStringLiteral("user.email")), QStringLiteral("grace@home"));
}

void ProfilesTest::theActiveProfileIsRememberedPerRepository() {
    profiles::save(make(QStringLiteral("Work"), QStringLiteral("Ada"), QStringLiteral("ada@work")));
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    window.profilesForDiagnostics(QStringLiteral("use"), QStringLiteral("Work"));

    // The active choice is keyed by the common git dir, the repository's identity,
    // not by the checkout path.
    const QString commonDir = QString::fromStdString(
                                  runGit({"rev-parse", "--absolute-git-dir"}, m_path.toStdString()).out)
                                  .trimmed();
    QCOMPARE(profiles::activeFor(commonDir), QStringLiteral("Work"));

    const QStringList report = window.profilesForDiagnostics(QStringLiteral("list"), QString());
    QVERIFY2(report.join(QLatin1Char('\n')).contains(QStringLiteral("active: Work")),
             qPrintable(report.join(QLatin1Char('\n'))));
}

QTEST_MAIN(ProfilesTest)
#include "test_profiles.moc"
