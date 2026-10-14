
#include "git/cli.h"
#include "ui/keymap.h"
#include "ui/main_window.h"
#include "ui/preferences_dialog.h"

#include <QKeySequenceEdit>
#include <QSettings>
#include <QShortcut>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace gk;

class KeymapTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_settingsDir;
    std::unique_ptr<QTemporaryDir> m_repoDir;
    QString m_path;

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void overridesRoundTrip();
    void anUnsetCommandKeepsItsDefault();
    void theWindowRebindsAndReports();
    void aDuplicateIsRefused();
    void aMenuItemActionIsReboundNotDuplicated();
    void theDialogRefusesAConflict();
};

void KeymapTest::initTestCase() {
    m_settingsDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_settingsDir->isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("gitmycelium-test"));
    QCoreApplication::setApplicationName(QStringLiteral("gitmycelium-keymap-test"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir->path());
}

void KeymapTest::init() {
    QSettings().clear();
    m_repoDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_repoDir->isValid());
    m_path = m_repoDir->path();
}

void KeymapTest::cleanup() {
    m_repoDir.reset();
}

void KeymapTest::overridesRoundTrip() {
    Keymap keymap;
    const QKeySequence fallback(QStringLiteral("Ctrl+B"));
    QCOMPARE(keymap.effective(QStringLiteral("view.branches"), fallback), fallback);

    keymap.setOverride(QStringLiteral("view.branches"), QKeySequence(QStringLiteral("Ctrl+Alt+B")));
    QCOMPARE(keymap.effective(QStringLiteral("view.branches"), fallback),
             QKeySequence(QStringLiteral("Ctrl+Alt+B")));
    QCOMPARE(keymap.overrides().value(QStringLiteral("view.branches")),
             QKeySequence(QStringLiteral("Ctrl+Alt+B")));

    keymap.reset(QStringLiteral("view.branches"));
    QCOMPARE(keymap.effective(QStringLiteral("view.branches"), fallback), fallback);
    QVERIFY(keymap.overrides().isEmpty());
}

void KeymapTest::anUnsetCommandKeepsItsDefault() {
    Keymap keymap;
    const QKeySequence fallback(QStringLiteral("Ctrl+K"));
    QCOMPARE(keymap.effective(QStringLiteral("x.thing"), fallback), fallback);
    keymap.setOverride(QStringLiteral("x.thing"), QKeySequence());
    QVERIFY(keymap.effective(QStringLiteral("x.thing"), fallback).isEmpty());
}

void KeymapTest::theWindowRebindsAndReports() {
    MainWindow window;

    const QStringList listed = window.keymapForDiagnostics(QStringLiteral("list"), {});
    QVERIFY2(listed.join(QLatin1Char('\n')).contains(QStringLiteral("view.branches=Ctrl+B")),
             qPrintable(listed.join(QLatin1Char('\n'))));
    QVERIFY(window.commandSequence(QStringLiteral("view.conflicts")).isEmpty());

    QStringList out = window.keymapForDiagnostics(
        QStringLiteral("set"), {QStringLiteral("view.conflicts"), QStringLiteral("Ctrl+Shift+C")});
    QCOMPARE(out.value(0), QStringLiteral("set: view.conflicts=Ctrl+Shift+C"));
    QCOMPARE(window.commandSequence(QStringLiteral("view.conflicts")),
             QStringLiteral("Ctrl+Shift+C"));

    out = window.keymapForDiagnostics(QStringLiteral("reset"),
                                      {QStringLiteral("view.conflicts")});
    QVERIFY(window.commandSequence(QStringLiteral("view.conflicts")).isEmpty());
}

void KeymapTest::aDuplicateIsRefused() {
    MainWindow window;
    const QStringList out = window.keymapForDiagnostics(
        QStringLiteral("set"), {QStringLiteral("app.open"), QStringLiteral("Ctrl+B")});
    QVERIFY2(out.value(0).contains(QStringLiteral("view.branches")), qPrintable(out.value(0)));
    QCOMPARE(window.commandSequence(QStringLiteral("app.open")), QStringLiteral("Ctrl+O"));
}

void KeymapTest::aMenuItemActionIsReboundNotDuplicated() {
    MainWindow window;
    window.keymapForDiagnostics(QStringLiteral("set"),
                                {QStringLiteral("edit.undo"), QStringLiteral("Ctrl+Alt+Z")});
    QCOMPARE(window.commandSequence(QStringLiteral("edit.undo")), QStringLiteral("Ctrl+Alt+Z"));

    // The action carries it; a second shortcut beside it would make the key
    // ambiguous and is exactly what this guards against.
    int shortcuts = 0;
    for (QShortcut* shortcut : window.findChildren<QShortcut*>()) {
        if (shortcut->key() == QKeySequence(QStringLiteral("Ctrl+Alt+Z"))) {
            ++shortcuts;
        }
    }
    QCOMPARE(shortcuts, 0);
}

void KeymapTest::theDialogRefusesAConflict() {
    QList<KeymapRow> rows;
    rows.append({QStringLiteral("a.one"), QStringLiteral("One"), QStringLiteral("Test"),
                 QKeySequence(QStringLiteral("Ctrl+1")), QKeySequence(QStringLiteral("Ctrl+1"))});
    rows.append({QStringLiteral("a.two"), QStringLiteral("Two"), QStringLiteral("Test"),
                 QKeySequence(QStringLiteral("Ctrl+2")), QKeySequence(QStringLiteral("Ctrl+2"))});

    PreferencesDialog dialog(QString(), {}, RererePreferences{}, rows,
                             {QStringLiteral("Ctrl+P")});
    auto* one = dialog.findChild<QKeySequenceEdit*>(QStringLiteral("a.one"));
    auto* two = dialog.findChild<QKeySequenceEdit*>(QStringLiteral("a.two"));
    QVERIFY(one);
    QVERIFY(two);
    QVERIFY(dialog.keymapProblem().isEmpty());

    two->setKeySequence(QKeySequence(QStringLiteral("Ctrl+1")));
    QVERIFY2(!dialog.keymapProblem().isEmpty(), "a duplicate must be caught");
    two->setKeySequence(QKeySequence(QStringLiteral("Ctrl+2")));
    one->setKeySequence(QKeySequence(QStringLiteral("Ctrl+P")));
    QVERIFY(!dialog.keymapProblem().isEmpty());

    one->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Alt+1")));
    QVERIFY(dialog.keymapProblem().isEmpty());
    dialog.accept();
    QCOMPARE(dialog.keymapRows().at(0).sequence, QKeySequence(QStringLiteral("Ctrl+Alt+1")));
}

QTEST_MAIN(KeymapTest)
#include "test_keymap.moc"
