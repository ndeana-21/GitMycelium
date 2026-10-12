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

/// The interactive rebase editor: the commits of a range, oldest first, each with what
/// should happen to it, reorderable by dragging a row.
///
/// The widget holds no repository and runs nothing. It hands back a plan — as rewrite
/// steps — and the window decides whether to run it, exactly as the palette chooses a
/// command but does not run it.
class RebaseEditor : public QDialog {
    Q_OBJECT

public:
    explicit RebaseEditor(QWidget* parent = nullptr);

    /// Fills the list, replacing anything there. Oldest first, as the repository listed
    /// them; the order shown is the order they will be replayed.
    void setCandidates(const std::vector<RebaseCandidate>& candidates);

    /// The plan as steps, in the order the rows now stand. A reword carries the message
    /// that was entered for it.
    std::vector<RewriteStep> steps() const;

    int rowCount() const;
    void setAction(int row, RewriteAction action);
    RewriteAction actionAt(int row) const;
    void setMessage(int row, const QString& message);
    QString messageAt(int row) const;
    void moveRow(int from, int to);
    /// The current reason Run is unavailable, or empty when the plan is runnable.
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
