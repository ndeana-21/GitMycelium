/// The chrome icon set: every icon must actually draw, in the colour it is asked
/// for. An icon that renders as nothing is invisible in a screenshot but obvious
/// in a toolbar, and a tint that silently fails looks like a theme bug.

#include "theme/icons.h"
#include "theme/theme.h"

#include <QColor>
#include <QImage>
#include <QtTest>

using namespace gk;

namespace {

bool hasOpaquePixel(const QImage& image) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) {
                return true;
            }
        }
    }
    return false;
}

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

class ThemeIconsTest : public QObject {
    Q_OBJECT

private slots:
    void everyIconRendersSomething();
    void theColourIsApplied();
};

void ThemeIconsTest::everyIconRendersSomething() {
    for (const Icon which : allIcons()) {
        const QIcon rendered = icon(which, QColor(QStringLiteral("#ffffff")), 18);
        QVERIFY2(!rendered.isNull(), qPrintable(iconResource(which)));
        const QImage image = rendered.pixmap(QSize(18, 18)).toImage();
        QVERIFY2(!image.isNull(), qPrintable(iconResource(which)));
        QVERIFY2(hasOpaquePixel(image), qPrintable(iconResource(which)));
    }
}

void ThemeIconsTest::theColourIsApplied() {
    const QImage image =
        icon(Icon::Gear, QColor(QStringLiteral("#ff0000")), 18).pixmap(QSize(18, 18)).toImage();
    QVERIFY(hasOpaquePixel(image));

    const QColor drawn = strongestPixel(image);
    QVERIFY2(drawn.red() > 200, qPrintable(drawn.name()));
    QVERIFY2(drawn.green() < 80, qPrintable(drawn.name()));
    QVERIFY2(drawn.blue() < 80, qPrintable(drawn.name()));
}

QTEST_MAIN(ThemeIconsTest)
#include "test_theme_icons.moc"
