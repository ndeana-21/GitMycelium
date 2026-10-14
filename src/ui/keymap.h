#pragma once

#include <QHash>
#include <QKeySequence>
#include <QString>

namespace gk {

class Keymap {
public:
    static QString settingsKey(const QString& id);

    QKeySequence effective(const QString& id, const QKeySequence& fallback) const;

    /// Stores `id`'s override. An empty sequence unbinds the command (it had a
    /// default and the reader removed it); `reset` is what returns it to that
    /// default.
    void setOverride(const QString& id, const QKeySequence& sequence);
    void reset(const QString& id);
    void resetAll();

    QHash<QString, QKeySequence> overrides() const;
};

} // namespace gk
