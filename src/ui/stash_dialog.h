#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QLineEdit;

namespace gk {

/// Asks how a stash should be written: an optional message, and whether untracked
/// files come along. Two questions, so two controls — a bare QInputDialog has no
/// room for the second one, and the choice matters: an untracked file is invisible
/// to a plain `git stash` and disappears from the working tree only with `-u`.
class StashDialog : public QDialog {
    Q_OBJECT

public:
    explicit StashDialog(QWidget* parent = nullptr);

    QString message() const;
    bool includeUntracked() const;

private:
    QLineEdit* m_message = nullptr;
    QCheckBox* m_untracked = nullptr;
};

} // namespace gk
