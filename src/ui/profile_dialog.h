#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QLineEdit;
class QListWidget;

namespace gk {

class ProfileDialog : public QDialog {
    Q_OBJECT

public:
    explicit ProfileDialog(const QString& activeName, QWidget* parent = nullptr);

    /// The profile the reader asked to apply, empty when they only edited.
    QString chosenProfile() const { return m_chosen; }

private:
    void reloadList(const QString& select);
    void loadSelectionIntoFields();
    void saveFields();

    QListWidget* m_list = nullptr;
    QLineEdit* m_name = nullptr;
    QLineEdit* m_user = nullptr;
    QLineEdit* m_email = nullptr;
    QLineEdit* m_key = nullptr;
    QCheckBox* m_sign = nullptr;
    QString m_chosen;
};

} // namespace gk
