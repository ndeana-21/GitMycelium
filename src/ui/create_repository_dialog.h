#pragma once

#include <QDialog>

class QLineEdit;

namespace gk {

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
