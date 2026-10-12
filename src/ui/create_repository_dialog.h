#pragma once

#include <QDialog>

class QLineEdit;

namespace gk {

/// Collects where a new repository should live and what its first branch is called.
/// It only gathers the answers — the window creates the repository, because the
/// dialog has no session.
class CreateRepositoryDialog : public QDialog {
    Q_OBJECT

public:
    explicit CreateRepositoryDialog(QWidget* parent = nullptr);

    QString path() const;
    QString initialBranch() const;

private:
    QLineEdit* m_path = nullptr;
    QLineEdit* m_branch = nullptr;
};

} // namespace gk
