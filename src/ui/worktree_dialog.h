#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLineEdit;

namespace gk {

/// Asks how to create a worktree: where it goes, which branch it checks out, and —
/// when the branch does not exist yet — what it starts from. Four questions, which
/// is more than a `QInputDialog` can carry, and the start point is the one people
/// forget: `git worktree add -b fix path` starts from HEAD, not from the branch
/// they meant.
class WorktreeDialog : public QDialog {
    Q_OBJECT

public:
    explicit WorktreeDialog(QWidget* parent = nullptr);

    /// Pre-fills the branch picker with the repository's local branches. The field
    /// stays editable, so a new name can be typed.
    void setBranches(const QStringList& branches);

    QString path() const;
    QString branch() const;
    /// True when `git worktree add -b` should create the branch.
    bool createBranch() const;
    /// Where a new branch starts; empty means HEAD.
    QString startPoint() const;

private:
    QLineEdit* m_path = nullptr;
    QComboBox* m_branch = nullptr;
    QCheckBox* m_create = nullptr;
    QLineEdit* m_start = nullptr;
};

} // namespace gk
