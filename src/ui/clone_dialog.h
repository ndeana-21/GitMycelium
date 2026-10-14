#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QLineEdit;
class QSpinBox;

namespace gk {

class CloneDialog : public QDialog {
    Q_OBJECT

public:
    explicit CloneDialog(QWidget* parent = nullptr);

    QString url() const;
    QString destination() const;
    bool partial() const;
    QString branch() const;
    int depth() const;
    /// The `--shallow-since` date, empty when not asked for. Mutually exclusive with
    /// the depth in the UI, because git refuses the two together.
    QString shallowSince() const;

private:
    QLineEdit* m_url = nullptr;
    QLineEdit* m_dest = nullptr;
    QCheckBox* m_partial = nullptr;
    QLineEdit* m_branch = nullptr;
    QSpinBox* m_depth = nullptr;
    QLineEdit* m_since = nullptr;
};

} // namespace gk
