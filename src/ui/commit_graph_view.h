#pragma once

#include "core/types.h"
#include "graph/graph_animation.h"
#include "model/commit_model.h"
#include "model/ref_model.h"
#include "ui/badge_flight.h"
#include "ui/graph_operation_panel.h"
#include "ui/ref_pill_layout.h"

#include <QPoint>
#include <QPixmap>
#include <QTreeView>
#include <QVariantAnimation>
#include <QVector>

#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

class QHideEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QResizeEvent;
class QTimer;

namespace gk {


/// The commit list of the graph, with drag-and-drop *inside* it: a row can carry several
/// labels, and only the one under the cursor is the one grabbed. The view resolves it with
/// the same RefPillLayout the delegate paints with.
class CommitGraphView : public QTreeView {
    Q_OBJECT

public:
    explicit CommitGraphView(QWidget* parent = nullptr);

    struct HoverLabel {
        bool valid = false;
        QString text;
        int colorIndex = 0;
        RefKind kind = RefKind::LocalBranch;
    };

    HoverLabel hoverLabelAt(int row) const;

    /// The branches a collapsed pill on `row` hides, in the order the overlay reveals
    /// them. Empty when the row wears no collapsed branch pill.
    QStringList hoverBranchesAt(int row) const;

    QString describeHover(int row) const;

    QString ghostBranchAt(int row) const;

    QString highlightBranchAt(int row) const;

    void activateGhostAt(int row);

    void openContextMenuAt(int row);

    void setHoverRowForDiagnostics(int row);

    void setHoverExpansionForDiagnostics(int row);
    QStringList expandedBranchesForDiagnostics() const;
    bool hoverExpansionOpen() const { return !m_expandPills.empty(); }
    QRect hoverExpansionSourceRect() const { return m_expandSourceRect; }
    void hoverMoveForDiagnostics(const QPoint& viewportPos);
    void pressRefForDiagnostics(const QPoint& viewportPos);
    QRect hoverExpansionPillRect(const QString& branch) const;
    /// Public so a check can compare what is painted with what is hit-tested — the two
    /// must be the same rectangle, tracking badge and all.
    std::vector<RefPill> rowPillsForDiagnostics(int row) const { return rowPills(row); }
    int ghostBadgeRowForDiagnostics() const { return ghostBadgeRow(); }
    QString pressedRefForDiagnostics() const {
        return m_pressedRefValid ? QString::fromStdString(m_pressedRef.shortName) : QString();
    }

    void selectRowsForDiagnostics(const QVector<int>& rows);

    QVector<int> selectedRows() const;

    std::vector<Oid> selectedCommitIds() const;

    void setAcceptedMimeType(const QString& mimeType);

    /// Turns the graph's motion on or off. Off for headless runs and `--shot`,
    /// where a frame caught mid-fade would make the output non-deterministic.
    void setAnimationsEnabled(bool on);
    bool animationsEnabled() const { return m_animationsEnabled; }
    /// The branch a drag in flight would land on if released now; empty otherwise.
    QString dropTargetForDiagnostics() const { return m_dropTarget; }

    void clearAnimation();

    /// While busy the graph shows a quiet sweep along the HEAD lane, so a long fetch or
    /// rebase reads as work in progress.
    void setBusy(bool busy);
    bool busy() const { return m_busy; }

    double animationFadeAlpha(const QString& oid) const;
    /// While the overlay owns the row's graph the delegate must not draw it, or the
    /// gliding snapshot and the new drawing would both show.
    bool animationIsMorphing(const QString& oid) const;
    double animationLabelAlpha(const QString& name) const;

    QStringList animationForDiagnostics() const;
    /// The transitions the last change produced, kept after they expired — a headless
    /// check runs long after a 200 ms fade is over.
    QStringList lastAnimationTrace() const { return m_lastAnimationTrace; }


    void setModel(QAbstractItemModel* model) override;

    /// Moves the selection to the selected commit's parent (`up`) or to the nearest commit
    /// above it that has it as a parent (`down`). Returns the id landed on, or empty.
    QString navigate(const QString& direction);

    /// Runs the badge flow for two branches and, when `pick` is not empty, clicks the list
    /// entry that starts with it; returns one line per step. The chain from a click to an
    /// operation cannot be checked by looking at the screen.
    QStringList simulateBranchDrop(const QString& source, const QString& target,
                                   const QString& pick = QString());

    /// Called from the handler of `dropOperationsOpened`, which fires just before the list
    /// is shown, so the text is in place when the list measures itself around it.
    void setDropPreview(const QString& text, bool warns = false);

signals:
    void refDroppedOnRef(gk::RefModel::DraggedRef source, QString targetShortName);
    /// The window answers with setDropPreview, synchronously, so the list can carry the
    /// merge-tree prediction before it is measured.
    void dropOperationsOpened(QString source, QString target);
    void refDroppedOnCommit(gk::RefModel::DraggedRef source, QString commitId);
    /// A label was left-clicked: focused, not checked out. Focus decides which
    /// branch gets the leftmost lane in the graph.
    void refFocused(QString shortName);
    void refHovered(QString shortName);
    void commitContextMenuRequested(QString commitId, QPoint globalPos);
    /// A branch label was double-clicked: check that branch out. Fired for the
    /// label only, never for the row — a double-click on a commit does not move the
    /// working tree, and a client where it does is a client that loses work.
    void branchCheckoutRequested(QString shortName, gk::RefKind kind);
    void mergeRequested(QString source, QString target);
    void rebaseRequested(QString source, QString target);
    void squashRequested(QString source, QString target);
    void interactiveRebaseRequested(QString source, QString target);
    void checkoutTargetRequested(QString target);

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void startDrag(Qt::DropActions supportedActions) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    /// Ghosts and gliding snapshots live in viewport coordinates; scrolling moves
    /// the rows under them, so the only honest thing is to drop them.
    void scrollContentsBy(int dx, int dy) override;

    QString commitIdAtRow(int row) const;

private:
    /// Puts the badge down on the branch it was dropped on and opens the list of
    /// operations beside it. Every exit from that list ends in `finishFlight()`.
    void beginFlight(const RefModel::DraggedRef& source, const QString& target,
                     const QRect& targetPill);
    void finishFlight(bool immediately = false);
    void advanceFlight();
    /// Where the badge lives when it is not lifted: the centre of its own label, or
    /// the drop point when its row is not loaded.
    QPoint homeOf(const QString& branch, const QPoint& fallback);
    void paintFlyingBadge(QPainter& painter);
    void paintGhostBadge(QPainter& painter);
    /// The row whose ghost badge is on screen: the hovered row when it has one,
    /// else the current row — so the badge is there to double-click, not only to
    /// look at. -1 when there is nothing honest to show.
    int ghostBadgeRow() const;
    /// The ghost badge pill for `row`, laid out exactly as the painter draws it —
    /// the rect and the text rect both. Shared by painting and by the double-click
    /// hit-test, so the two cannot disagree.
    bool ghostBadgePill(int row, RefPill* out) const;
    void setHoveredRef(const QString& name);
    int rowAtViewportPos(const QPoint& pos) const;
    /// The topmost labelled row on the line `row` is drawn on, or -1.
    int nearestLabelledChildRow(int row) const;
    void setHoveredRow(int row);

    const RefInfo* refAt(const QPoint& viewportPos, QString* shortNameOut = nullptr) const;

    std::vector<RefPill> rowPills(int row) const;

    void updateHoverExpansion(const QPoint& pos);
    void clearHoverExpansion();
    void paintHoverExpansion(QPainter& painter);

private slots:
    /// Asks for the next page of history once the reader is near the end of what is
    /// loaded; the model never pages on its own.
    void fetchIfNearEnd();
    void onModelAboutToBeReset();
    void onRowsAboutToBeInserted(const QModelIndex& parent, int first, int last);
    void onRowsInserted(const QModelIndex& parent, int first, int last);
    void onRowsAboutToBeRemoved(const QModelIndex& parent, int first, int last);
    void onRowsRemoved(const QModelIndex& parent, int first, int last);
    void advanceAnimation();

private:
    std::vector<GraphRowKey> animationRows() const;
    std::set<std::string> animationLabels() const;
    QString animationHead() const;
    void captureGhosts();
    void observeAnimation();
    void startAnimationTimer();
    void paintAnimation(QPainter& painter) const;
    void paintPulse(QPainter& painter, const GraphTransition& pulse) const;
    QStringList formatTransitions() const;


    struct Ghost {
        QPixmap pixmap;
        QRect rect;
        int lane = 0;
    };

    GraphAnimation m_animation;
    bool m_animationsEnabled = true;
    bool m_busy = false;
    /// True once a change has been captured and is waiting for the model to settle:
    /// a reset empties the model and the first page arrives a moment later.
    bool m_capturePending = false;
    std::unordered_map<std::string, Ghost> m_ghosts;
    QTimer* m_animTimer = nullptr;
    QStringList m_lastAnimationTrace;

    QPoint m_pressPos;
    QString m_acceptedMimeType;
    /// The row the pointer is over, -1 when it is over none. Painting reads it and nothing
    /// else, so a badge cannot be drawn for a row the pointer has left.
    int m_hoverRow = -1;
    /// The reference the pointer is over, empty when it is over none. Kept so
    /// `refHovered` only fires when the answer actually changes.
    QString m_hoveredRef;

    /// The hover overlay that reveals the branches a collapsed pill hides. Laid out
    /// once per hover and shared by painting and hit-testing; viewport coordinates,
    /// so it is cleared when the rows move under it.
    int m_expandRow = -1;
    QString m_expandSource;
    QString m_expandHovered;
    std::vector<RefPill> m_expandPills;
    /// The source pill's rect and the region that keeps the overlay open: the pill,
    /// the stack, and the gap between them. The pointer has to cross that gap to get
    /// from one to the other, and closing on the way makes the stack unreachable.
    QRect m_expandSourceRect;
    QRect m_expandKeepOpen;
    /// True while the pointer is on the reveal overlay (a revealed pill or the way
    /// to it). The ghost badge is suppressed then: the reader is looking at the
    /// branches under the commit, not asking what branch the commit belongs to.
    bool m_expandHovering = false;
    /// The label captured when the mouse went down. A drag starts from the press,
    /// but the hover overlay may close as the pointer moves off it; capturing the
    /// label here keeps the revealed branch draggable for merge and rebase.
    RefInfo m_pressedRef;
    bool m_pressedRefValid = false;

    BadgeFlight m_flight;
    GraphOperationPanel* m_panel = nullptr;
    QTimer* m_flightTimer = nullptr;
    QString m_flightSource;
    QString m_flightTarget;
    /// Filled in when the badge is lifted so the delegate's own label can be covered
    /// while the badge is elsewhere.
    QRect m_vacatedPill;
    /// The prediction shown in the open operation list, filled by the window in answer
    /// to `dropOperationsOpened`. Cleared before that signal fires.
    QString m_dropPreview;
    bool m_dropPreviewWarns = false;

    void setDropTarget(const QString& name, const QRect& pill);
    void paintDropTarget(QPainter& painter);
    QString m_dropTarget;
    QRect m_dropTargetRect;
    QVariantAnimation* m_dropPulse = nullptr;
};

} // namespace gk
