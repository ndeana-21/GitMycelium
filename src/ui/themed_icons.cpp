#include "ui/themed_icons.h"

#include "theme/theme.h"

#include <QAbstractButton>
#include <QAction>
#include <QHash>
#include <QObject>

namespace gk {
namespace {

struct Entry {
    enum class Kind { Icon, Glyph };
    Kind kind;
    int which;
    IconTint tint;
    int side;
};

QHash<QObject*, Entry>& registry() {
    static QHash<QObject*, Entry> map;
    return map;
}

void remember(QObject* object, const Entry& entry) {
    registry().insert(object, entry);
    QObject::connect(object, &QObject::destroyed, object,
                     [](QObject* self) { registry().remove(self); });
}

void applyTo(QObject* object, const Entry& entry) {
    const QColor color = tintColor(entry.tint);
    const QIcon rendered = entry.kind == Entry::Kind::Icon
                               ? icon(static_cast<Icon>(entry.which), color, entry.side)
                               : glyphIcon(static_cast<Glyph>(entry.which), color, entry.side);
    if (auto* action = qobject_cast<QAction*>(object)) {
        action->setIcon(rendered);
    } else if (auto* button = qobject_cast<QAbstractButton*>(object)) {
        button->setIcon(rendered);
    } else if (auto* menu = qobject_cast<QMenu*>(object)) {
        menu->setIcon(rendered);
    }
}

} // namespace

QColor tintColor(IconTint tint) {
    switch (tint) {
    case IconTint::Text: return theme::text();
    case IconTint::Dim: return theme::textDim();
    case IconTint::Accent: return theme::accent();
    case IconTint::Modified: return theme::modified();
    case IconTint::Danger: return theme::danger();
    case IconTint::Success: return theme::success();
    case IconTint::Warning: return theme::warning();
    case IconTint::Added: return theme::diffAddedFg();
    case IconTint::Removed: return theme::diffRemovedFg();
    case IconTint::OnAccent: return theme::onAccent();
    }
    return theme::text();
}

void setThemedIcon(QAction* action, Icon which, IconTint tint) {
    if (!action) {
        return;
    }
    action->setIcon(icon(which, tintColor(tint), theme::iconSize()));
    remember(action, Entry{Entry::Kind::Icon, static_cast<int>(which), tint, theme::iconSize()});
}

void setThemedIcon(QAbstractButton* button, Icon which, IconTint tint) {
    if (!button) {
        return;
    }
    button->setIcon(icon(which, tintColor(tint), theme::iconSize()));
    remember(button, Entry{Entry::Kind::Icon, static_cast<int>(which), tint, theme::iconSize()});
}

void setThemedIcon(QMenu* menu, Icon which, IconTint tint) {
    if (!menu) {
        return;
    }
    menu->setIcon(icon(which, tintColor(tint), theme::iconSize()));
    remember(menu, Entry{Entry::Kind::Icon, static_cast<int>(which), tint, theme::iconSize()});
}

void setThemedGlyphIcon(QAction* action, Glyph glyph, IconTint tint, int side) {
    if (!action) {
        return;
    }
    action->setIcon(glyphIcon(glyph, tintColor(tint), side));
    remember(action, Entry{Entry::Kind::Glyph, static_cast<int>(glyph), tint, side});
}

void retintThemedIcons() {
    clearIconCache();
    for (auto it = registry().begin(); it != registry().end(); ++it) {
        applyTo(it.key(), it.value());
    }
}

} // namespace gk
