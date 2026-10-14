#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QListWidget;

namespace gk {

class SparseCheckoutDialog : public QDialog {
    Q_OBJECT

public:
    enum class Action { None, Apply, Disable };

    /// `directories` are the top-level directories at HEAD; `inCone` are the ones
    /// currently kept. `enabled` marks whether sparse checkout is on at all.
    SparseCheckoutDialog(const QStringList& directories, const QStringList& inCone, bool enabled,
                         QWidget* parent = nullptr);

    Action action() const { return m_action; }
    QStringList chosenDirectories() const;

private:
    QListWidget* m_list = nullptr;
    Action m_action = Action::None;
};

} // namespace gk
