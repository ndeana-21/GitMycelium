#pragma once

#include "theme/glyphs.h"
#include "theme/icons.h"

#include <QMenu>

#include <utility>

class QAbstractButton;
class QAction;

namespace gk {

/// Which theme token an icon is tinted with. The indirection exists so a theme
/// switch can redraw every remembered icon from the *new* palette: a QAction or
/// a button keeps the QIcon it was handed, and re-running every construction
/// site is not an option.
enum class IconTint {
    Text,
    Dim,
    Accent,
    Modified,
    Danger,
    Success,
    Warning,
    Added,
    Removed,
    OnAccent, ///< drawn on a filled, coloured button
};

QColor tintColor(IconTint tint);

/// Sets and remembers an icon on an action or a button. The side is not passed:
/// it follows `theme::iconSize()` so icons stay sharp when the reader zooms.
void setThemedIcon(QAction* action, Icon which, IconTint tint = IconTint::Text);
void setThemedIcon(QAbstractButton* button, Icon which, IconTint tint = IconTint::Text);
/// A submenu's own icon (`QMenu` is neither an action nor a button).
void setThemedIcon(QMenu* menu, Icon which, IconTint tint = IconTint::Text);

/// `menu->addAction(text, args...)` and remembers the icon, so a menu entry built
/// once still redraws its icon when the theme changes. Returns the action, as
/// `addAction` does, so shortcuts and checkable state can still be set on it.
template <typename... Args>
QAction* addThemedAction(QMenu* menu, Icon which, const QString& text, Args&&... args) {
    QAction* action = menu->addAction(text, std::forward<Args>(args)...);
    setThemedIcon(action, which);
    return action;
}

/// The same for a hand-drawn glyph (a pin, a paperclip), at an explicit side.
void setThemedGlyphIcon(QAction* action, Glyph glyph, IconTint tint, int side);

/// Re-renders every remembered icon from the active theme. Call after
/// `theme::setMode`/`theme::setZoomPercent`; nothing else has to be told.
void retintThemedIcons();

} // namespace gk
