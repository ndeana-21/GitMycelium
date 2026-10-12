#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QLineEdit;
class QListWidget;
class QPushButton;

namespace gk {

/// Git LFS: what is tracked, what is under it, and the operations that act on it.
///
/// The dialog performs nothing itself — it returns the action the reader asked for
/// and the window runs it, so a button can never act on a repository that moved
/// underneath it. The window reopens the dialog after each action, which is how one
/// can track several patterns in a row.
class LfsDialog : public QDialog {
    Q_OBJECT

public:
    enum class Action { None, Install, Track, Untrack, Checkout, Prune, Fetch, Pull, Push };

    LfsDialog(bool available, const QString& version, bool configured,
              const QStringList& patterns, const QStringList& files, QWidget* parent = nullptr);

    Action action() const { return m_action; }
    /// The pattern to track (the field) or untrack (the selected row), without the
    /// source suffix git-lfs prints.
    QString pattern() const;

private:
    Action m_action = Action::None;
    QLineEdit* m_patternField = nullptr;
    QListWidget* m_patternList = nullptr;
};

} // namespace gk
