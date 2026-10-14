#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;

namespace gk {

class SubmoduleDialog : public QDialog {
    Q_OBJECT

public:
    explicit SubmoduleDialog(QWidget* parent = nullptr);

    QString url() const;
    /// Empty lets git derive it from the URL.
    QString path() const;
    /// `submodule.<name>.branch`; empty leaves it unset.
    QString branch() const;

private:
    QLineEdit* m_url = nullptr;
    QLineEdit* m_path = nullptr;
    QLineEdit* m_branch = nullptr;
};

} // namespace gk
