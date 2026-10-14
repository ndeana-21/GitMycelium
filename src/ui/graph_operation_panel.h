#pragma once

#include "core/types.h"

#include <QWidget>

class QLabel;
class QPushButton;

namespace gk {

/// One short line describing a merge prediction for the drop list. Never empty:
/// "nothing to predict" is said out loud rather than left blank.
QString describeMergePreview(const MergePreview& preview);

enum class RefOperation {
    Rebase,
    Merge,
    Squash,
    InteractiveRebase,
    Checkout,
};

/// The operations offered when a branch badge is dropped on another branch, as a list
/// beside the badge rather than a menu over it.
///
/// Reports one thing and only one thing — a choice or a dismissal — because the badge
/// is waiting to fly home and must not wait for a second answer that never comes.
class GraphOperationPanel : public QWidget {
    Q_OBJECT

public:
    explicit GraphOperationPanel(QWidget* parent = nullptr);

    /// Shows the list with its top-left at `at`, pressed back inside `room` when it would
    /// hang over the edge (`at` and `room` in the parent's coordinates). `previewWarns`
    /// paints the prediction as a warning rather than as a plain note.
    void present(const QString& source, const QString& target, const QPoint& at,
                 const QRect& room = QRect(), const QString& preview = QString(),
                 bool previewWarns = false);

    /// Closes and reports a dismissal, unless a choice was already reported. `reason`
    /// reaches the flow log and nowhere else.
    void dismiss(const QString& reason = QStringLiteral("closed"));

signals:
    void chosen(gk::RefOperation operation);
    void dismissed();

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;

private:
    void report(gk::RefOperation operation);
    /// Called at construction and each time the list is shown, since the panel
    /// outlives a theme change but is only visible during a drop.
    void applyTheme();

    QLabel* m_title = nullptr;
    QLabel* m_preview = nullptr;
    QPushButton* m_rebase = nullptr;
    QPushButton* m_merge = nullptr;
    QPushButton* m_squash = nullptr;
    QPushButton* m_interactive = nullptr;
    QPushButton* m_checkout = nullptr;
    bool m_reported = false;
    /// Watches the whole application while the list is open: a press outside it and
    /// Escape dismiss, nothing else. Focus changes do not: the list appears inside a
    /// drop, and on some window managers the focus it asks for arrives late or not at
    /// all, which is not the reader walking away.
    bool eventFilter(QObject* watched, QEvent* event) override;
};

} // namespace gk
