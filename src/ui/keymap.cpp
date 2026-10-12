#include "ui/keymap.h"

#include <QSettings>

namespace gk {

namespace {
constexpr const char* kGroup = "keymap";
} // namespace

QString Keymap::settingsKey(const QString& id) {
    return QString::fromLatin1(kGroup) + QLatin1Char('/') + id;
}

QKeySequence Keymap::effective(const QString& id, const QKeySequence& fallback) const {
    QSettings settings;
    const QVariant stored = settings.value(settingsKey(id));
    // No entry is not the same as an empty one: an unset command keeps its default,
    // while an override of "" is a command the reader unbound on purpose.
    if (!stored.isValid()) {
        return fallback;
    }
    return QKeySequence(stored.toString());
}

void Keymap::setOverride(const QString& id, const QKeySequence& sequence) {
    // Stored even when empty: an empty override is a command the reader unbound,
    // which is not the same as no override at all. `reset` is what returns a
    // command to its default.
    QSettings settings;
    settings.setValue(settingsKey(id), sequence.toString(QKeySequence::PortableText));
}

void Keymap::reset(const QString& id) {
    QSettings settings;
    settings.remove(settingsKey(id));
}

void Keymap::resetAll() {
    QSettings settings;
    settings.remove(QString::fromLatin1(kGroup));
}

QHash<QString, QKeySequence> Keymap::overrides() const {
    QHash<QString, QKeySequence> out;
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kGroup));
    for (const QString& id : settings.childKeys()) {
        out.insert(id, QKeySequence(settings.value(id).toString()));
    }
    settings.endGroup();
    return out;
}

} // namespace gk
