#pragma once

#include "core/types.h"

#include <QDialog>
#include <QString>

#include <vector>

class QPushButton;
class QTreeWidget;

namespace gk {

class ReflogDialog : public QDialog {
    Q_OBJECT

public:
    explicit ReflogDialog(const std::vector<ReflogEntry>& entries, QWidget* parent = nullptr);

    QString chosenCommit() const { return m_chosenCommit; }
    /// What the reader asked for: "checkout", "branch", or empty.
    QString chosenAction() const { return m_chosenAction; }

private:
    void choose(const QString& action);
    QString currentCommit() const;

    QTreeWidget* m_list = nullptr;
    QPushButton* m_checkoutButton = nullptr;
    QPushButton* m_branchButton = nullptr;
    QString m_chosenCommit;
    QString m_chosenAction;
};

} // namespace gk
