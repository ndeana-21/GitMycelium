/// Repository tabs: several repositories open in one window, switched by a tab
/// strip rather than by a second window. The window keeps one set of widgets and
/// swaps the session and models behind them, so the thing worth asserting is that
/// what belongs to one repository never shows while another is on screen.

#include "git/cli.h"
#include "git/repository.h"
#include "ui/main_window.h"

#include <QDir>
#include <QFile>
#include <QTabBar>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QtTest>

#include <algorithm>

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

int lineValue(const QStringList& report, const QString& prefix) {
    for (const QString& line : report) {
        if (line.startsWith(prefix + QStringLiteral(": "))) {
            return line.mid(prefix.size() + 2).toInt();
        }
    }
    return -1;
}

QString lineText(const QStringList& report, const QString& prefix) {
    for (const QString& line : report) {
        if (line.startsWith(prefix + QStringLiteral(": "))) {
            return line.mid(prefix.size() + 2);
        }
    }
    return QString();
}

} // namespace

class TabsTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_config;
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_a;
    QString m_b;

    void makeRepository(const QString& path, const QString& branch);

private slots:
    void initTestCase();
    void init();

    void openingASecondRepositoryAddsATab();
    void reopeningAnOpenRepositoryActivatesItsTabInstead();
    void switchingTabsChangesTheRepositoryOnScreen();
    void perRepositoryStateDoesNotLeak();
    void closingATabLeavesTheRest();
    void closingATabWhileItFetchesClearsTheRunningFlag();
    void theLastTabIsEmptiedNotRemoved();
    void closingOtherTabsKeepsTheChosenOne();
    void closingAllTabsLeavesOneEmpty();
    void theTabStripIsAboveTheMainToolbar();
    void theWindowIsNamedGitMycelium();
    void theTabCloseButtonIsThemed();
    void anEmptyTabShowsTheStartPage();
    void theEmptyStartingTabIsOnTheStrip();
    void restoredTabsCarryTheirNamesBeforeTheyOpen();
    void thePlusOpensAStartPageTab();
    void thePlusDoesNotReuseARestoredTab();
    void aRememberedAliasNamesTheTabThatOpens();
    void tabsAndAliasesSurviveARestart();
};

void TabsTest::initTestCase() {
    // Keep the session and alias stores in a throwaway config dir: a test that
    // remembers tabs must not teach the next real run to open a deleted temp path.
    m_config = std::make_unique<QTemporaryDir>();
    QVERIFY(m_config->isValid());
    qputenv("XDG_CONFIG_HOME", m_config->path().toUtf8());
    Repository::initLibrary();
}

void TabsTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_a = QDir(m_dir->path()).filePath(QStringLiteral("alpha"));
    m_b = QDir(m_dir->path()).filePath(QStringLiteral("beta"));
    makeRepository(m_a, QStringLiteral("main"));
    makeRepository(m_b, QStringLiteral("feature"));
}

void TabsTest::makeRepository(const QString& path, const QString& branch) {
    QVERIFY(QDir().mkpath(path));
    QVERIFY(git(path, {"init", "-q", "-b", branch}));
    QVERIFY(git(path, {"config", "user.name", "Demo Dev"}));
    QVERIFY(git(path, {"config", "user.email", "demo@example.com"}));
    QVERIFY(git(path, {"config", "commit.gpgsign", "false"}));
    QVERIFY(writeFile(QDir(path).filePath(QStringLiteral("f.txt")), QStringLiteral("one\n")));
    QVERIFY(git(path, {"add", "-A"}));
    QVERIFY(git(path, {"commit", "-qm", "first"}));
    QVERIFY(writeFile(QDir(path).filePath(QStringLiteral("f.txt")), QStringLiteral("one\ntwo\n")));
    QVERIFY(git(path, {"commit", "-qam", "second"}));
}

void TabsTest::openingASecondRepositoryAddsATab() {
    MainWindow window;
    QVERIFY(window.openRepository(m_a));
    QStringList report = window.tabsForDiagnostics(QStringLiteral("list"), {});
    QCOMPARE(lineValue(report, QStringLiteral("count")), 1);
    QCOMPARE(lineValue(report, QStringLiteral("active")), 0);

    QVERIFY(window.openRepository(m_b));
    report = window.tabsForDiagnostics(QStringLiteral("list"), {});
    QCOMPARE(lineValue(report, QStringLiteral("count")), 2);
    QCOMPARE(lineValue(report, QStringLiteral("active")), 1);
    QCOMPARE(window.currentBranchName(), std::string("feature"));
}

void TabsTest::reopeningAnOpenRepositoryActivatesItsTabInstead() {
    MainWindow window;
    QVERIFY(window.openRepository(m_a));
    QVERIFY(window.openRepository(m_b));
    // A second open of the first repository brings its tab forward; it does not add
    // a third tab and does not open the same checkout twice.
    QVERIFY(window.openRepository(m_a));
    const QStringList report = window.tabsForDiagnostics(QStringLiteral("list"), {});
    QCOMPARE(lineValue(report, QStringLiteral("count")), 2);
    QCOMPARE(lineValue(report, QStringLiteral("active")), 0);
    QCOMPARE(window.currentBranchName(), std::string("main"));
}

void TabsTest::switchingTabsChangesTheRepositoryOnScreen() {
    MainWindow window;
    QVERIFY(window.openRepository(m_a));
    QVERIFY(window.openRepository(m_b));
    window.tabsForDiagnostics(QStringLiteral("activate"), {QStringLiteral("0")});
    QCOMPARE(window.currentBranchName(), std::string("main"));
    window.tabsForDiagnostics(QStringLiteral("activate"), {QStringLiteral("1")});
    QCOMPARE(window.currentBranchName(), std::string("feature"));
}

void TabsTest::perRepositoryStateDoesNotLeak() {
    MainWindow window;
    QVERIFY(window.openRepository(m_a));
    QVERIFY(window.openRepository(m_b));
    const QStringList pinned =
        window.visibilityForDiagnostics(QStringLiteral("pin"), QStringLiteral("feature"));
    QVERIFY(lineText(pinned, QStringLiteral("pinned")).contains(QStringLiteral("feature")));

    window.tabsForDiagnostics(QStringLiteral("activate"), {QStringLiteral("0")});
    const QStringList alpha =
        window.visibilityForDiagnostics(QStringLiteral("show"), QString());
    QVERIFY(!lineText(alpha, QStringLiteral("pinned")).contains(QStringLiteral("feature")));

    window.tabsForDiagnostics(QStringLiteral("activate"), {QStringLiteral("1")});
    const QStringList betaAgain =
        window.visibilityForDiagnostics(QStringLiteral("show"), QString());
    QVERIFY(lineText(betaAgain, QStringLiteral("pinned")).contains(QStringLiteral("feature")));
}

void TabsTest::closingATabLeavesTheRest() {
    MainWindow window;
    QVERIFY(window.openRepository(m_a));
    QVERIFY(window.openRepository(m_b));
    window.tabsForDiagnostics(QStringLiteral("close"), {QStringLiteral("1")});
    const QStringList report = window.tabsForDiagnostics(QStringLiteral("list"), {});
    QCOMPARE(lineValue(report, QStringLiteral("count")), 1);
    QCOMPARE(window.currentBranchName(), std::string("main"));
}

void TabsTest::closingATabWhileItFetchesClearsTheRunningFlag() {
    // A remote that takes a second to answer, so the fetch is still running when the
    // tab goes. The session used to be deleted under its worker, and the window's
    // global "a sync is running" flag never cleared again.
    const QString remote = QDir(m_dir->path()).filePath(QStringLiteral("remote.git"));
    QVERIFY(git(m_dir->path(), {"clone", "-q", "--bare", m_a, remote}));
    QVERIFY(git(m_b, {"remote", "add", "origin", remote}));
    QVERIFY(git(m_b, {"config", "remote.origin.uploadpack", "sleep 1; git-upload-pack"}));

    MainWindow window;
    QVERIFY(window.openRepository(m_a));
    QVERIFY(window.openRepository(m_b));
    window.commandPaletteForDiagnostics(QStringLiteral("remote fetch"), true);
    QVERIFY(window.syncRunningForDiagnostics());

    window.tabsForDiagnostics(QStringLiteral("close"), {QStringLiteral("1")});
    QTRY_VERIFY_WITH_TIMEOUT(!window.syncRunningForDiagnostics(), 15000);
    QCOMPARE(window.currentBranchName(), std::string("main"));
}

void TabsTest::theLastTabIsEmptiedNotRemoved() {
    MainWindow window;
    QVERIFY(window.openRepository(m_a));
    window.tabsForDiagnostics(QStringLiteral("close"), {QStringLiteral("0")});
    const QStringList report = window.tabsForDiagnostics(QStringLiteral("list"), {});
    // The window always keeps one tab so there is somewhere to open the next
    // repository; closing the last one empties it rather than leaving nothing.
    QCOMPARE(lineValue(report, QStringLiteral("count")), 1);
    QVERIFY(window.currentBranchName().empty());
}

void TabsTest::closingOtherTabsKeepsTheChosenOne() {
    MainWindow window;
    QVERIFY(window.openRepository(m_a));
    QVERIFY(window.openRepository(m_b));
    window.tabsForDiagnostics(QStringLiteral("close-others"), {QStringLiteral("0")});
    const QStringList report = window.tabsForDiagnostics(QStringLiteral("list"), {});
    QCOMPARE(lineValue(report, QStringLiteral("count")), 1);
    QCOMPARE(window.currentBranchName(), std::string("main"));
}

void TabsTest::closingAllTabsLeavesOneEmpty() {
    MainWindow window;
    QVERIFY(window.openRepository(m_a));
    QVERIFY(window.openRepository(m_b));
    window.tabsForDiagnostics(QStringLiteral("close-all"), {});
    const QStringList report = window.tabsForDiagnostics(QStringLiteral("list"), {});
    QCOMPARE(lineValue(report, QStringLiteral("count")), 1);
    QVERIFY(window.currentBranchName().empty());
}

void TabsTest::theTabStripIsAboveTheMainToolbar() {
    MainWindow window;
    window.resize(900, 600);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* tabs = window.findChild<QToolBar*>(QStringLiteral("tabsToolBar"));
    auto* main = window.findChild<QToolBar*>(QStringLiteral("mainToolBar"));
    QVERIFY(tabs);
    QVERIFY(main);
    QVERIFY(window.findChild<QToolButton*>(QStringLiteral("newTabButton")));
    QVERIFY(tabs->mapTo(&window, QPoint(0, 0)).y() < main->mapTo(&window, QPoint(0, 0)).y());
}

void TabsTest::theWindowIsNamedGitMycelium() {
    // The product name is "GitMycelium" wherever a person reads it; the settings
    // identity behind it stays `gitmycelium`, because changing that resets every
    // preference the reader has.
    MainWindow window;
    QCOMPARE(window.windowTitle(), QStringLiteral("GitMycelium"));
    QVERIFY(window.openRepository(m_a));
    QCOMPARE(window.windowTitle(), QStringLiteral("GitMycelium — alpha"));
}

void TabsTest::theTabCloseButtonIsThemed() {
    MainWindow window;
    QVERIFY(window.openRepository(m_a));
    QVERIFY(window.openRepository(m_b));
    auto* bar = window.findChild<QTabBar*>(QStringLiteral("repositoryTabBar"));
    QVERIFY(bar);
    QCOMPARE(bar->count(), 2);

    // Our themed button, not the style's: the built-in one is a red glyph taken from
    // the desktop's icon theme, which belongs to no palette in this window.
    auto* close = qobject_cast<QToolButton*>(bar->tabButton(0, QTabBar::RightSide));
    QVERIFY2(close, "the tab has to carry the themed close button");
    QCOMPARE(close->objectName(), QStringLiteral("tabClose"));
    QVERIFY(!close->icon().isNull());

    // The strip is movable, so the button finds the tab it belongs to rather than an
    // index captured when it was built.
    close->click();
    const QStringList report = window.tabsForDiagnostics(QStringLiteral("list"), {});
    QCOMPARE(lineValue(report, QStringLiteral("count")), 1);
    QCOMPARE(window.currentBranchName(), std::string("feature"));
}

void TabsTest::anEmptyTabShowsTheStartPage() {
    MainWindow window;
    QString report = window.startPageForDiagnostics().join(QLatin1Char('\n'));
    QVERIFY(report.contains(QStringLiteral("visible: yes")));

    QVERIFY(window.openRepository(m_a));
    report = window.startPageForDiagnostics().join(QLatin1Char('\n'));
    QVERIFY(report.contains(QStringLiteral("visible: no")));

    window.tabsForDiagnostics(QStringLiteral("close-all"), {});
    report = window.startPageForDiagnostics().join(QLatin1Char('\n'));
    QVERIFY(report.contains(QStringLiteral("visible: yes")));
}

void TabsTest::theEmptyStartingTabIsOnTheStrip() {
    MainWindow window;
    const QStringList report = window.tabsForDiagnostics(QStringLiteral("list"), {});
    QCOMPARE(lineValue(report, QStringLiteral("count")), 1);
    // A window that has opened nothing yet still has its one tab, and it is called a
    // new tab rather than named after a repository it does not hold.
    QCOMPARE(lineText(report, QStringLiteral("bar 0")), QStringLiteral("New Tab"));
    QCOMPARE(lineText(report, QStringLiteral("tab 0")),
             QStringLiteral("New Tab | path=(none) | opened=no"));
}

void TabsTest::thePlusOpensAStartPageTab() {
    MainWindow window;
    QVERIFY(window.openRepository(m_a));
    QCOMPARE(lineValue(window.tabsForDiagnostics(QStringLiteral("list"), {}),
                       QStringLiteral("count")),
             1);

    // "+" opens a start-page tab, not the file chooser: the reader picks a way in
    // from the page.
    window.tabsForDiagnostics(QStringLiteral("new"), {});
    QStringList report = window.tabsForDiagnostics(QStringLiteral("list"), {});
    QCOMPARE(lineValue(report, QStringLiteral("count")), 2);
    QCOMPARE(window.currentBranchName().empty(), true);
    QVERIFY(window.startPageForDiagnostics().join(QLatin1Char('\n'))
                .contains(QStringLiteral("visible: yes")));

    window.tabsForDiagnostics(QStringLiteral("new"), {});
    report = window.tabsForDiagnostics(QStringLiteral("list"), {});
    QCOMPARE(lineValue(report, QStringLiteral("count")), 2);
}

void TabsTest::restoredTabsCarryTheirNamesBeforeTheyOpen() {
    // Save a two-tab session, then restore it: only the active tab is opened at
    // startup, and the other is a path waiting for a switch.
    {
        MainWindow window;
        window.setTabsPersistenceEnabled(true);
        window.resize(900, 600);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.openRepository(m_a));
        QVERIFY(window.openRepository(m_b));
    }

    MainWindow restored;
    restored.setTabsPersistenceEnabled(true);
    restored.restoreTabs();
    const QStringList report = restored.tabsForDiagnostics(QStringLiteral("list"), {});
    // A tab that has not been opened yet still knows what it holds, so the strip
    // names it now rather than after the reader has visited it and come back.
    QCOMPARE(lineText(report, QStringLiteral("bar 0")), QStringLiteral("alpha"));
    QCOMPARE(lineText(report, QStringLiteral("bar 1")), QStringLiteral("beta"));
    QVERIFY(report.join(QLatin1Char('\n')).contains(QStringLiteral("opened=no")));
}

void TabsTest::thePlusDoesNotReuseARestoredTab() {
    {
        MainWindow window;
        window.setTabsPersistenceEnabled(true);
        window.resize(900, 600);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.openRepository(m_a));
        QVERIFY(window.openRepository(m_b));
    }

    MainWindow restored;
    restored.setTabsPersistenceEnabled(true);
    restored.restoreTabs();
    QCOMPARE(lineValue(restored.tabsForDiagnostics(QStringLiteral("list"), {}),
                       QStringLiteral("count")),
             2);

    // Neither restored tab has been opened, and neither is an empty tab: "+" has to
    // add a third rather than jump to a repository the reader did not ask for.
    restored.tabsForDiagnostics(QStringLiteral("new"), {});
    const QStringList report = restored.tabsForDiagnostics(QStringLiteral("list"), {});
    QCOMPARE(lineValue(report, QStringLiteral("count")), 3);
    QCOMPARE(lineValue(report, QStringLiteral("active")), 2);
    QCOMPARE(lineText(report, QStringLiteral("bar 2")), QStringLiteral("New Tab"));
}

void TabsTest::aRememberedAliasNamesTheTabThatOpens() {
    {
        MainWindow window;
        window.setTabsPersistenceEnabled(true);
        window.resize(900, 600);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.openRepository(m_a));
        window.tabsForDiagnostics(QStringLiteral("rename"),
                                  {QStringLiteral("0"), QStringLiteral("AlphaTab")});
    }

    // The alias lives in the per-repository store, and a tab reads it only once its
    // session is open — after the bar was rebuilt for the switch. A strip that is not
    // relabelled there shows the directory name while the context already says the
    // alias, which is the two-claims-disagreeing shape this whole area is about.
    MainWindow reopened;
    QVERIFY(reopened.openRepository(m_a));
    const QStringList report = reopened.tabsForDiagnostics(QStringLiteral("list"), {});
    QCOMPARE(lineText(report, QStringLiteral("bar 0")), QStringLiteral("AlphaTab"));
    QVERIFY(lineText(report, QStringLiteral("tab 0")).startsWith(QStringLiteral("AlphaTab")));
}

void TabsTest::tabsAndAliasesSurviveARestart() {
    {
        MainWindow window;
        window.setTabsPersistenceEnabled(true);
        window.resize(900, 600);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.openRepository(m_a));
        QVERIFY(window.openRepository(m_b));
        window.tabsForDiagnostics(QStringLiteral("rename"),
                                  {QStringLiteral("1"), QStringLiteral("BetaTab")});
    }

    MainWindow restored;
    restored.setTabsPersistenceEnabled(true);
    restored.restoreTabs();
    const QStringList report = restored.tabsForDiagnostics(QStringLiteral("list"), {});
    QCOMPARE(lineValue(report, QStringLiteral("count")), 2);
    QCOMPARE(lineValue(report, QStringLiteral("active")), 1);
    QVERIFY(report.join(QLatin1Char('\n')).contains(QStringLiteral("BetaTab")));

    QCOMPARE(restored.currentBranchName(), std::string("feature"));
    QVERIFY(report.join(QLatin1Char('\n')).contains(QStringLiteral("opened=no")));

    restored.tabsForDiagnostics(QStringLiteral("activate"), {QStringLiteral("0")});
    QCOMPARE(restored.currentBranchName(), std::string("main"));
}

QTEST_MAIN(TabsTest)
#include "test_tabs.moc"
