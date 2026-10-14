/// The themed-icon registry: an icon is remembered with the tint role it was
/// built from, and a theme change redraws it. This is what keeps a toolbar icon
/// visible after the reader switches to the light theme, where the QIcon an
/// action already holds would otherwise stay near-white on white.

#include "theme/theme.h"
#include "ui/themed_icons.h"

#include <QAction>
#include <QColor>
#include <QImage>
#include <QtTest>

using namespace gk;

namespace {

QColor strongestPixel(const QImage& image) {
    int bestAlpha = -1;
    QColor best;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = QColor::fromRgba(image.pixel(x, y));
            if (color.alpha() > bestAlpha) {
                bestAlpha = color.alpha();
                best = color;
            }
        }
    }
    return best;
}

} // namespace

class ThemedIconsTest : public QObject {
    Q_OBJECT

private slots:
    void cleanup();

    void aTintFollowsTheTheme();
    void aRegisteredActionIconIsRedrawnOnAThemeChange();
};

void ThemedIconsTest::cleanup() {
    theme::setMode(theme::ThemeMode::Dark);
    theme::setZoomPercent(100);
    retintThemedIcons();
}

void ThemedIconsTest::aTintFollowsTheTheme() {
    QCOMPARE(tintColor(IconTint::Text).name(), theme::text().name());
    QCOMPARE(tintColor(IconTint::Danger).name(), theme::danger().name());

    theme::setMode(theme::ThemeMode::Light);
    QCOMPARE(tintColor(IconTint::Text).name(), theme::text().name());
    QCOMPARE(tintColor(IconTint::Text).name(), QStringLiteral("#1a1a1a"));
}

void ThemedIconsTest::aRegisteredActionIconIsRedrawnOnAThemeChange() {
    QAction action;
    setThemedIcon(&action, Icon::Gear);
    const QColor dark = strongestPixel(action.icon().pixmap(QSize(18, 18)).toImage());

    theme::setMode(theme::ThemeMode::Light);
    retintThemedIcons();
    const QColor light = strongestPixel(action.icon().pixmap(QSize(18, 18)).toImage());

    QVERIFY(dark.lightness() > light.lightness());
}

QTEST_MAIN(ThemedIconsTest)
#include "test_themed_icons.moc"
