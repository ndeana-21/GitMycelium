#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLineEdit;

namespace gk {

class WorktreeDialog : public QDialog {
    Q_OBJECT

public:
    explicit WorktreeDialog(QWidget* parent = nullptr);

    void setBranches(const QStringList& branches);

    QString path() const;
    QString branch() const;
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
