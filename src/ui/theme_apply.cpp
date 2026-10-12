#include "ui/theme_apply.h"

#include "theme/theme.h"
#include "ui/settings_store.h"

#include <QApplication>
#include <QFont>

namespace gk {

void loadSavedAppearance() {
    const SettingsStore store;
    const QString themeName =
        store.stringValue(SettingsStore::globalKey(QStringLiteral("theme")),
                          QStringLiteral("dark"));
    theme::setMode(themeName.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0
                       ? theme::ThemeMode::Light
                       : theme::ThemeMode::Dark);
    theme::setZoomPercent(store.intValue(SettingsStore::globalKey(QStringLiteral("zoom")), 100));
}

void applyApplicationTheme(QApplication* app) {
    if (!app) {
        return;
    }
    app->setPalette(theme::palette());
    QFont baseFont = app->font();
    baseFont.setFamily(theme::uiFontFamily());
    baseFont.setStyleHint(QFont::Monospace);
    baseFont.setPixelSize(theme::baseFontPixelSize());
    app->setFont(baseFont);
    app->setStyleSheet(theme::styleSheet());
}

} // namespace gk
