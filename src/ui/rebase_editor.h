#pragma once

#include "core/rebase_plan.h"
#include "core/types.h"

#include <QDialog>
#include <QString>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace gk {

class RebaseEditor : public QDialog {
    Q_OBJECT

public:
    explicit RebaseEditor(QWidget* parent = nullptr);

    void setCandidates(const std::vector<RebaseCandidate>& candidates);

    std::vector<RewriteStep> steps() const;

    int rowCount() const;
    void setAction(int row, RewriteAction action);
    RewriteAction actionAt(int row) const;
    void setMessage(int row, const QString& message);
    QString messageAt(int row) const;
    void moveRow(int from, int to);
    QString refusal() const;

private:
    std::vector<RebasePlanRow> plan() const;
    void refreshValidation();
    /// Asks for a message for every reword that has none yet. Returns false if the
    /// reader cancelled one.
    bool collectMessages();
    void rebuild();

    QTreeWidget* m_tree = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_problem = nullptr;
    QPushButton* m_run = nullptr;
};

} // namespace gk
