#pragma once

#include <QHash>
#include <QKeySequence>
#include <QString>

namespace gk {

/// Configurable shortcuts for the window's commands.
///
/// The command registry already knows each command's default sequence, so a keymap
/// is a sparse override on top of it: `keymap/<id>` in QSettings holds the reader's
/// sequence and clearing the override returns the command to its default. Nothing
/// here installs a binding — that is the window's job, because only it knows which
/// commands have a menu action and which need a shortcut of their own.
class Keymap {
public:
    static QString settingsKey(const QString& id);

    /// The sequence `id` is bound to: the override when there is one, else
    /// `fallback` (the command's own default).
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
