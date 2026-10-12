/// The theme is mutable state: a mode switch swaps every token at once, and the
/// zoom scales the base font and the geometry tokens together. These are pure
/// checks — no window — so a regression says "the palette moved" rather than
/// showing up as a wrong pixel in one widget's snapshot.

#include "theme/theme.h"

#include <QColor>
#include <QPalette>
#include <QtTest>

using namespace gk;

namespace {

bool lighterThan(const QColor& a, const QColor& b) {
    return a.lightness() > b.lightness();
}

} // namespace

class ThemeTest : public QObject {
    Q_OBJECT

private slots:
    void cleanup();

    void theDefaultIsDark();
    void theModeSwapsEveryToken();
    void theStylesheetFollowsTheMode();
    void thePaletteFollowsTheMode();
    void aBranchLabelReadsTheOtherWayOnLight();
    void theZoomScalesFontAndGeometry();
    void theZoomIsClamped();
    void theGenerationChangesWithTheLook();
};

void ThemeTest::cleanup() {
    theme::setMode(theme::ThemeMode::Dark);
    theme::setZoomPercent(100);
}

void ThemeTest::theDefaultIsDark() {
    QCOMPARE(theme::mode(), theme::ThemeMode::Dark);
    QCOMPARE(theme::background().name(), QStringLiteral("#0a0a0a"));
}

void ThemeTest::theModeSwapsEveryToken() {
    const QColor darkBackground = theme::background();
    const QColor darkText = theme::text();

    theme::setMode(theme::ThemeMode::Light);
    QCOMPARE(theme::mode(), theme::ThemeMode::Light);
    QVERIFY(theme::background() != darkBackground);
    QVERIFY(theme::text() != darkText);
    QCOMPARE(theme::background().name(), QStringLiteral("#ffffff"));
    QCOMPARE(theme::text().name(), QStringLiteral("#1a1a1a"));

    theme::setMode(theme::ThemeMode::Dark);
    QCOMPARE(theme::background(), darkBackground);
    QCOMPARE(theme::text(), darkText);
}

void ThemeTest::theStylesheetFollowsTheMode() {
    const QString dark = theme::styleSheet();
    QVERIFY(dark.contains(QStringLiteral("#0a0a0a")));

    theme::setMode(theme::ThemeMode::Light);
    const QString light = theme::styleSheet();
    QVERIFY(light.contains(QStringLiteral("#ffffff")));
    QVERIFY(light != dark);
}

void ThemeTest::thePaletteFollowsTheMode() {
    const QPalette dark = theme::palette();
    QCOMPARE(dark.color(QPalette::Base).name(), QStringLiteral("#0a0a0a"));

    theme::setMode(theme::ThemeMode::Light);
    const QPalette light = theme::palette();
    QCOMPARE(light.color(QPalette::Base).name(), QStringLiteral("#ffffff"));
    QCOMPARE(light.color(QPalette::Text).name(), QStringLiteral("#1a1a1a"));
}

void ThemeTest::aBranchLabelReadsTheOtherWayOnLight() {
    const QColor branch = theme::branchColor(0);

    // On a dark row the text is lifted towards white...
    QVERIFY(lighterThan(theme::branchLabelText(branch), branch));

    // ...and on a light one it must sink towards black, or it disappears.
    theme::setMode(theme::ThemeMode::Light);
    QVERIFY(theme::branchLabelText(branch).lightness() < branch.lightness());
}

void ThemeTest::theZoomScalesFontAndGeometry() {
    QCOMPARE(theme::baseFontPixelSize(), 13);
    QCOMPARE(theme::radius(), 4);
    QCOMPARE(theme::iconSize(), 18);
    QCOMPARE(theme::rowHeight(), 30);

    theme::setZoomPercent(150);
    QCOMPARE(theme::baseFontPixelSize(), 20);
    QCOMPARE(theme::radius(), 6);
    QCOMPARE(theme::iconSize(), 27);
    QCOMPARE(theme::rowHeight(), 45);
}

void ThemeTest::theZoomIsClamped() {
    theme::setZoomPercent(10);
    QCOMPARE(theme::zoomPercent(), 50);
    theme::setZoomPercent(1000);
    QCOMPARE(theme::zoomPercent(), 250);
}

void ThemeTest::theGenerationChangesWithTheLook() {
    const int before = theme::generation();
    theme::setMode(theme::ThemeMode::Light);
    QVERIFY(theme::generation() > before);

    const int afterMode = theme::generation();
    theme::setZoomPercent(125);
    QVERIFY(theme::generation() > afterMode);

    // Setting the same value is not a change.
    const int afterZoom = theme::generation();
    theme::setZoomPercent(125);
    QCOMPARE(theme::generation(), afterZoom);
}

QTEST_MAIN(ThemeTest)
#include "test_theme.moc"
