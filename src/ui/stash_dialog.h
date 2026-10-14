#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QLineEdit;

namespace gk {

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
