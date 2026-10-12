#pragma once

#include "core/types.h"

#include <QDialog>
#include <QString>

#include <vector>

class QPushButton;
class QTreeWidget;

namespace gk {

/// The repository's reflog, newest first, as `git reflog show` reports it.
///
/// Every move a ref has made is here — commits, checkouts, rebases, resets, and the
/// moves another client made — which is the only place to answer "where was this
/// branch before". The dialog knows nothing about the repository: it returns the
/// commit and the action the reader chose, and the window performs it, so a row can
/// never act on a stale ref.
class ReflogDialog : public QDialog {
    Q_OBJECT

public:
    explicit ReflogDialog(const std::vector<ReflogEntry>& entries, QWidget* parent = nullptr);

    /// The commit of the chosen row, empty when nothing was chosen.
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
