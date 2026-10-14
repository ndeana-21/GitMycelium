/// Preferences: QSettings stays the single source of truth — the dialog must not
/// grow a second model — and the keys keep the shape an existing profile has.

#include "git/cli.h"
#include "model/commit_model.h"
#include "theme/theme.h"
#include "ui/clone_dialog.h"
#include "ui/coauthor_dialog.h"
#include "ui/commit_graph_view.h"
#include "ui/main_window.h"
#include "ui/preferences_dialog.h"
#include "ui/settings_store.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QtTest>

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

class PreferencesTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_settingsDir;
    std::unique_ptr<QTemporaryDir> m_repoDir;
    QString m_path;

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void storeRoundTripsTypedValues();
    void repositoryKeysKeepTheirShape();
    void theWindowPersistsTheGraphAnimation();
    void headlessPreferencesSetAndGet();
    void theDialogWritesTheKeysTheWindowReads();
    void theDialogWritesTheAppearance();
    void headlessThemeAndZoom();
    void theToolbarPreferencesButtonIsLive();
    void theColumnOrderIsRemembered();
    void theCloneDialogGathersTheAnswers();
    void theCoauthorDialogGathersTheAuthors();
    void theToolsPageCarriesTheChosenTools();
    void theSigningPageCarriesTheChoices();
    void theToolbarMenuButtonsReserveRoomForTheArrow();
};

void PreferencesTest::initTestCase() {
    // The window applies the same Fusion style the application uses; a test that
    // applies the appearance should not be standing in a different one.
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    // Keep the test's profile out of the real one: redirect the default settings
    // location before anything constructs a QSettings.
    m_settingsDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_settingsDir->isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("gitmycelium-test"));
    QCoreApplication::setApplicationName(QStringLiteral("gitmycelium-preferences-test"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir->path());
}

void PreferencesTest::init() {
    QSettings().clear();
    m_repoDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_repoDir->isValid());
    m_path = m_repoDir->path();
    git(m_path, {"init", "-q", "-b", "main"});
    git(m_path, {"config", "user.name", "Demo"});
    git(m_path, {"config", "user.email", "demo@example.com"});
    git(m_path, {"config", "commit.gpgsign", "false"});
    QVERIFY(writeFile(m_path + "/app.txt", "line 1\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, {"commit", "-qm", "first"}));
}

void PreferencesTest::cleanup() {
    theme::setMode(theme::ThemeMode::Dark);
    theme::setZoomPercent(100);
    m_repoDir.reset();
}

void PreferencesTest::storeRoundTripsTypedValues() {
    SettingsStore store(QStringLiteral("/repo/.git"));
    store.setBool(QStringLiteral("ui/test/bool"), true);
    store.setInt(QStringLiteral("ui/test/int"), 7);
    store.setString(QStringLiteral("ui/test/str"), QStringLiteral("hi"));
    store.setList(QStringLiteral("ui/test/list"), {QStringLiteral("a"), QStringLiteral("b")});

    QCOMPARE(store.boolValue(QStringLiteral("ui/test/bool"), false), true);
    QCOMPARE(store.intValue(QStringLiteral("ui/test/int"), 0), 7);
    QCOMPARE(store.stringValue(QStringLiteral("ui/test/str")), QStringLiteral("hi"));
    QCOMPARE(store.listValue(QStringLiteral("ui/test/list")),
             QStringList({QStringLiteral("a"), QStringLiteral("b")}));

    QVERIFY(store.contains(QStringLiteral("ui/test/bool")));
    store.remove(QStringLiteral("ui/test/bool"));
    QVERIFY(!store.contains(QStringLiteral("ui/test/bool")));
    QCOMPARE(store.intValue(QStringLiteral("ui/test/missing"), 42), 42);
}

void PreferencesTest::repositoryKeysKeepTheirShape() {
    // This is the exact shape the window wrote before the store existed; a change
    // here would silently reset every existing profile.
    QCOMPARE(SettingsStore(QStringLiteral("repo/.git")).repositoryKey(QStringLiteral("preflight")),
             QStringLiteral("ui/preflight/repo/.git"));
    QCOMPARE(SettingsStore().repositoryKey(QStringLiteral("preflight")),
             QStringLiteral("ui/preflight"));
    QCOMPARE(SettingsStore::globalKey(QStringLiteral("graph/animate")),
             QStringLiteral("ui/graph/animate"));
}

void PreferencesTest::theWindowPersistsTheGraphAnimation() {
    {
        MainWindow window;
        window.setGraphAnimationsEnabled(false);
    }
    QCOMPARE(SettingsStore().boolValue(SettingsStore::globalKey(QStringLiteral("graph/animate")),
                                       true),
             false);

    MainWindow window;
    auto* view = window.findChild<CommitGraphView*>();
    QVERIFY(view);
    QVERIFY(!view->animationsEnabled());
}

void PreferencesTest::headlessPreferencesSetAndGet() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    QStringList out = window.preferencesForDiagnostics(QStringLiteral("set"),
                                                       QStringLiteral("preflight"),
                                                       QStringLiteral("true"));
    QCOMPARE(out.value(0), QStringLiteral("preflight: 1"));
    out = window.preferencesForDiagnostics(QStringLiteral("get"), QStringLiteral("preflight"),
                                           QString());
    QCOMPARE(out.value(0), QStringLiteral("preflight: 1"));

    window.preferencesForDiagnostics(QStringLiteral("set"), QStringLiteral("autofetch.remotes"),
                                     QStringLiteral("origin,backup"));
    out = window.preferencesForDiagnostics(QStringLiteral("get"), QStringLiteral("autofetch.remotes"),
                                           QString());
    QCOMPARE(out.value(0), QStringLiteral("autofetch.remotes: origin,backup"));

    window.preferencesForDiagnostics(QStringLiteral("set"), QStringLiteral("graph.animate"),
                                     QStringLiteral("false"));
    out = window.preferencesForDiagnostics(QStringLiteral("get"), QStringLiteral("graph.animate"),
                                           QString());
    QCOMPARE(out.value(0), QStringLiteral("graph.animate: 0"));

    out = window.preferencesForDiagnostics(QStringLiteral("get"), QStringLiteral("nonsense"),
                                           QString());
    QVERIFY(out.value(0).startsWith(QStringLiteral("unknown preference")));
}

void PreferencesTest::theDialogWritesTheKeysTheWindowReads() {
    RererePreferences rerere;
    rerere.available = true;
    PreferencesDialog dialog(m_path, {QStringLiteral("origin")}, rerere);

    auto* preflight = dialog.findChild<QCheckBox*>(QStringLiteral("preflight"));
    QVERIFY(preflight);
    QVERIFY(!preflight->isChecked());
    preflight->setChecked(true);

    auto* animate = dialog.findChild<QCheckBox*>(QStringLiteral("animateGraph"));
    QVERIFY(animate);
    QVERIFY(animate->isChecked());
    animate->setChecked(false);

    dialog.accept();

    const SettingsStore store(m_path);
    QVERIFY(store.boolValue(store.repositoryKey(QStringLiteral("preflight")), false));
    QVERIFY(!SettingsStore().boolValue(SettingsStore::globalKey(QStringLiteral("graph/animate")),
                                       true));
}

void PreferencesTest::theDialogWritesTheAppearance() {
    RererePreferences rerere;
    rerere.available = true;
    PreferencesDialog dialog(m_path, {QStringLiteral("origin")}, rerere);

    auto* themeBox = dialog.findChild<QComboBox*>(QStringLiteral("theme"));
    QVERIFY(themeBox);
    themeBox->setCurrentIndex(themeBox->findData(QStringLiteral("light")));
    auto* zoomBox = dialog.findChild<QSpinBox*>(QStringLiteral("zoom"));
    QVERIFY(zoomBox);
    zoomBox->setValue(150);

    dialog.accept();

    const SettingsStore store;
    QCOMPARE(store.stringValue(SettingsStore::globalKey(QStringLiteral("theme"))),
             QStringLiteral("light"));
    QCOMPARE(store.intValue(SettingsStore::globalKey(QStringLiteral("zoom")), 0), 150);
}

void PreferencesTest::headlessThemeAndZoom() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    QStringList out =
        window.preferencesForDiagnostics(QStringLiteral("set"), QStringLiteral("theme"),
                                         QStringLiteral("light"));
    QCOMPARE(out.value(0), QStringLiteral("theme: light"));
    QCOMPARE(theme::mode(), theme::ThemeMode::Light);
    out = window.preferencesForDiagnostics(QStringLiteral("get"), QStringLiteral("theme"),
                                           QString());
    QCOMPARE(out.value(0), QStringLiteral("theme: light"));

    out = window.preferencesForDiagnostics(QStringLiteral("set"), QStringLiteral("zoom"),
                                           QStringLiteral("150"));
    QCOMPARE(out.value(0), QStringLiteral("zoom: 150"));
    QCOMPARE(theme::zoomPercent(), 150);
    out = window.preferencesForDiagnostics(QStringLiteral("get"), QStringLiteral("zoom"),
                                           QString());
    QCOMPARE(out.value(0), QStringLiteral("zoom: 150"));
}

void PreferencesTest::theToolbarPreferencesButtonIsLive() {
    MainWindow window;
    auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("mainToolBar"));
    QVERIFY(toolbar);
    QAction* preferences = nullptr;
    for (QAction* action : toolbar->actions()) {
        if (action->text() == QStringLiteral("Preferences")) {
            preferences = action;
            break;
        }
    }
    QVERIFY(preferences);
    QVERIFY(preferences->isEnabled());
    QVERIFY(!preferences->icon().isNull());
}

void PreferencesTest::theColumnOrderIsRemembered() {
    {
        MainWindow window;
        QVERIFY(window.openRepository(m_path));
        auto* view = window.findChild<CommitGraphView*>();
        QVERIFY(view);
        QHeaderView* header = view->header();
        const int visual = header->visualIndex(CommitModel::Message);
        header->moveSection(visual, 0);
        // The write is debounced so a divider drag is one save; wait it out.
        QTest::qWait(500);
    }

    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    auto* view = window.findChild<CommitGraphView*>();
    QVERIFY(view);
    QCOMPARE(view->header()->visualIndex(CommitModel::Message), 0);
}

void PreferencesTest::theCloneDialogGathersTheAnswers() {
    CloneDialog dialog;
    auto* url = dialog.findChild<QLineEdit*>(QStringLiteral("cloneUrl"));
    auto* dest = dialog.findChild<QLineEdit*>(QStringLiteral("cloneDestination"));
    auto* partial = dialog.findChild<QCheckBox*>(QStringLiteral("clonePartial"));
    auto* branch = dialog.findChild<QLineEdit*>(QStringLiteral("cloneBranch"));
    auto* depth = dialog.findChild<QSpinBox*>(QStringLiteral("cloneDepth"));
    QVERIFY(url && dest && partial && branch && depth);

    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    QVERIFY(buttons);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    QVERIFY(ok);
    QVERIFY(!ok->isEnabled());
    url->setText(QStringLiteral("/tmp/repo.git"));
    dest->setText(QStringLiteral("/tmp/dest"));
    QVERIFY(ok->isEnabled());

    partial->setChecked(true);
    branch->setText(QStringLiteral("main"));
    depth->setValue(1);
    QCOMPARE(dialog.url(), QStringLiteral("/tmp/repo.git"));
    QCOMPARE(dialog.destination(), QStringLiteral("/tmp/dest"));
    QVERIFY(dialog.partial());
    QCOMPARE(dialog.branch(), QStringLiteral("main"));
    QCOMPARE(dialog.depth(), 1);
}

void PreferencesTest::theCoauthorDialogGathersTheAuthors() {
    CoAuthorDialog dialog;
    dialog.setSuggestions({{"Ada Lovelace", "ada@example.com"}, {"Grace Hopper", "grace@example.com"}});

    auto* recent = dialog.findChild<QComboBox*>(QStringLiteral("coauthorRecent"));
    auto* name = dialog.findChild<QLineEdit*>(QStringLiteral("coauthorName"));
    auto* email = dialog.findChild<QLineEdit*>(QStringLiteral("coauthorEmail"));
    auto* add = dialog.findChild<QPushButton*>(QStringLiteral("coauthorAdd"));
    auto* list = dialog.findChild<QListWidget*>(QStringLiteral("coauthorList"));
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    QVERIFY(recent && name && email && add && list && buttons);
    QCOMPARE(recent->count(), 2);

    auto* ok = buttons->button(QDialogButtonBox::Ok);
    QVERIFY(ok);
    QVERIFY(!ok->isEnabled());

    name->setText(QStringLiteral("Ada Lovelace"));
    email->setText(QStringLiteral("ada@example.com"));
    add->click();
    QCOMPARE(list->count(), 1);
    // The fields are cleared after Add, so there is no half-typed row for OK to
    // accept; the one author is what makes it available.
    QVERIFY(ok->isEnabled());

    name->setText(QStringLiteral("Ada Lovelace"));
    email->setText(QStringLiteral("ada@example.com"));
    add->click();
    QCOMPARE(list->count(), 1);

    const std::vector<CoAuthor> authors = dialog.authors();
    QCOMPARE(authors.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(authors.front().name), QStringLiteral("Ada Lovelace"));
    QCOMPARE(QString::fromStdString(authors.front().email), QStringLiteral("ada@example.com"));
}

void PreferencesTest::theToolsPageCarriesTheChosenTools() {
    ExternalToolsPreferences tools;
    tools.available = true;
    tools.diffTool = QStringLiteral("meld");
    tools.mergeTool = QStringLiteral("vimdiff");
    tools.diffTools = {QStringLiteral("meld"), QStringLiteral("vimdiff"), QStringLiteral("araxis")};
    tools.mergeTools = {QStringLiteral("vimdiff")};

    PreferencesDialog dialog(QStringLiteral("/repo"), {}, {}, {}, {}, tools);
    auto* diffBox = dialog.findChild<QComboBox*>(QStringLiteral("diffTool"));
    auto* mergeBox = dialog.findChild<QComboBox*>(QStringLiteral("mergeTool"));
    QVERIFY(diffBox && mergeBox);
    QCOMPARE(dialog.diffTool(), QStringLiteral("meld"));
    QCOMPARE(dialog.mergeTool(), QStringLiteral("vimdiff"));

    // "git default" is how the page spells "unset", which is what the window writes.
    diffBox->setCurrentIndex(0);
    QCOMPARE(dialog.diffTool(), QString());
}

void PreferencesTest::theSigningPageCarriesTheChoices() {
    SigningPreferences signing;
    signing.available = true;
    signing.sign = true;
    signing.signingKey = QStringLiteral("ABC");
    signing.format = QStringLiteral("ssh");
    signing.gpgProgram = QStringLiteral("/usr/bin/gpg");

    PreferencesDialog dialog(QStringLiteral("/repo"), {}, {}, {}, {}, {}, signing);
    QVERIFY(dialog.signCommits());
    QCOMPARE(dialog.signingKey(), QStringLiteral("ABC"));
    QCOMPARE(dialog.signingFormat(), QStringLiteral("ssh"));
    QCOMPARE(dialog.gpgProgram(), QStringLiteral("/usr/bin/gpg"));

    auto* keyEdit = dialog.findChild<QLineEdit*>(QStringLiteral("signingKey"));
    auto* formatBox = dialog.findChild<QComboBox*>(QStringLiteral("signingFormat"));
    QVERIFY(keyEdit && formatBox);
    keyEdit->setText(QStringLiteral("XYZ"));
    formatBox->setCurrentIndex(0); // "git default" is unset
    QCOMPARE(dialog.signingKey(), QStringLiteral("XYZ"));
    QCOMPARE(dialog.signingFormat(), QString());
}

void PreferencesTest::theToolbarMenuButtonsReserveRoomForTheArrow() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("mainToolBar"));
    QVERIFY(toolbar);
    auto* pull = toolbar->findChild<QToolButton*>(QStringLiteral("pullButton"));
    auto* push = toolbar->findChild<QToolButton*>(QStringLiteral("pushButton"));
    QVERIFY(pull && push);
    QCOMPARE(pull->property("variant").toString(), QStringLiteral("menu"));
    QCOMPARE(push->property("variant").toString(), QStringLiteral("menu"));

    // The arrow lives inside the button; the variant reserves it its own room rather
    // than letting it eat the icon, so the button is wider than a plain icon button.
    int plain = -1;
    for (QToolButton* button : toolbar->findChildren<QToolButton*>()) {
        if (button != pull && button != push &&
            button->property("variant").toString() != QStringLiteral("menu")) {
            plain = button->sizeHint().width();
            break;
        }
    }
    QVERIFY2(plain > 0, "a plain toolbar button to compare against");
    QVERIFY2(pull->sizeHint().width() > plain, qPrintable(QStringLiteral("pull %1 vs plain %2")
                                                              .arg(pull->sizeHint().width())
                                                              .arg(plain)));
    QVERIFY(push->sizeHint().width() > plain);
}

QTEST_MAIN(PreferencesTest)
#include "test_preferences.moc"
