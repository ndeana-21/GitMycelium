#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QListWidget;

namespace gk {

/// Chooses the cone of a sparse checkout: which top-level directories stay on
/// disk. The dialog knows nothing about the repository — it returns the choice and
/// the window performs it, so a checkbox can never act on a stale tree.
///
/// Removing a directory takes its files off disk (they stay in HEAD), so the
/// dialog says so rather than letting a click look like a filter.
class SparseCheckoutDialog : public QDialog {
    Q_OBJECT

public:
    enum class Action { None, Apply, Disable };

    /// `directories` are the top-level directories at HEAD; `inCone` are the ones
    /// currently kept. `enabled` marks whether sparse checkout is on at all.
    SparseCheckoutDialog(const QStringList& directories, const QStringList& inCone, bool enabled,
                         QWidget* parent = nullptr);

    Action action() const { return m_action; }
    /// The checked directories, in the list's order.
    QStringList chosenDirectories() const;

private:
    QListWidget* m_list = nullptr;
    Action m_action = Action::None;
};

} // namespace gk
