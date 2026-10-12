#pragma once

#include <QDialog>
#include <QString>

class QComboBox;
class QLineEdit;
class QPushButton;

namespace gk {

/// Drives the gitflow workflow: initialize it, or start and finish a topic branch.
/// The dialog knows nothing about the repository — it returns what the reader asked
/// for and the window performs it, so a button can never act on a stale branch.
class GitFlowDialog : public QDialog {
    Q_OBJECT

public:
    enum class Action { None, Initialize, Start, Finish };

    GitFlowDialog(bool initialized, const QString& master, const QString& develop,
                  QWidget* parent = nullptr);

    Action action() const { return m_action; }
    QString masterBranch() const;
    QString developBranch() const;
    /// feature / hotfix / release / bugfix.
    QString kind() const;
    QString name() const;
    /// Only used by release and hotfix finishes.
    QString version() const;

private:
    bool m_initialized = false;
    Action m_action = Action::None;
    QLineEdit* m_master = nullptr;
    QLineEdit* m_develop = nullptr;
    QComboBox* m_kind = nullptr;
    QLineEdit* m_name = nullptr;
    QLineEdit* m_version = nullptr;
    QPushButton* m_startButton = nullptr;
    QPushButton* m_finishButton = nullptr;
};

} // namespace gk
