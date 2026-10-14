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

    /// The branch a commit belongs to, read from the nearest labelled commit above it along
    /// the chain of children. `valid` false means there is nothing honest to show, and an
    /// invented badge is worse than none.
    struct HoverLabel {
        bool valid = false;
        QString text;
        int colorIndex = 0;
        RefKind kind = RefKind::LocalBranch;
    };

    /// The badge for a row, whether or not it is the hovered one.
    HoverLabel hoverLabelAt(int row) const;

    /// The branches a collapsed pill on `row` hides, in the order the overlay reveals
    /// them. Empty when the row wears no collapsed branch pill.
    QStringList hoverBranchesAt(int row) const;

    QString describeHover(int row) const;

    /// The branch a double-click on `row`'s ghost badge would check out, or empty.
    QString ghostBranchAt(int row) const;

    QString highlightBranchAt(int row) const;

    /// Emits `branchCheckoutRequested` exactly as a double-click on the real label would.
    void activateGhostAt(int row);

    /// Runs the real context-menu path for a row without a mouse.
    void openContextMenuAt(int row);

    void setHoverRowForDiagnostics(int row);

    void setHoverExpansionForDiagnostics(int row);
    QStringList expandedBranchesForDiagnostics() const;
    bool hoverExpansionOpen() const { return !m_expandPills.empty(); }
    QRect hoverExpansionSourceRect() const { return m_expandSourceRect; }
    void hoverMoveForDiagnostics(const QPoint& viewportPos);
    /// Captures the drag source as a real press would.
    void pressRefForDiagnostics(const QPoint& viewportPos);
    QRect hoverExpansionPillRect(const QString& branch) const;
    /// Public so a check can compare what is painted with what is hit-tested — the two
    /// must be the same rectangle, tracking badge and all.
    std::vector<RefPill> rowPillsForDiagnostics(int row) const { return rowPills(row); }
    /// Suppressed while the pointer is on the reveal overlay.
    int ghostBadgeRowForDiagnostics() const { return ghostBadgeRow(); }
    /// The branch a press captured as a drag source, empty when none.
    QString pressedRefForDiagnostics() const {
        return m_pressedRefValid ? QString::fromStdString(m_pressedRef.shortName) : QString();
    }

    /// The same selection a reader makes with shift and ctrl, so the checks exercise the
    /// real path.
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

    /// Called when a repository is opened, so the first page is the starting point
    /// rather than a change from nothing.
    void clearAnimation();

    /// While busy the graph shows a quiet sweep along the HEAD lane, so a long fetch or
    /// rebase reads as work in progress.
    void setBusy(bool busy);
    bool busy() const { return m_busy; }

    /// Opacity the delegate should paint a commit's graph with (1.0 normally, less
    /// while it is fading in).
    double animationFadeAlpha(const QString& oid) const;
    /// While the overlay owns the row's graph the delegate must not draw it, or the
    /// gliding snapshot and the new drawing would both show.
    bool animationIsMorphing(const QString& oid) const;
    double animationLabelAlpha(const QString& name) const;

    QStringList animationForDiagnostics() const;
    /// The transitions the last change produced, kept after they expired — a headless
    /// check runs long after a 200 ms fade is over.
    QStringList lastAnimationTrace() const { return m_lastAnimationTrace; }


    /// Public because the window sets the model; the header must keep Qt's signature.
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
    /// The reference under the pointer changed, empty when it left every label and
    /// ghost badge. The window turns this into the graph's hover highlight.
    void refHovered(QString shortName);
    /// Right-click on a commit row. `globalPos` is where the menu belongs.
    void commitContextMenuRequested(QString commitId, QPoint globalPos);
    /// A branch label was double-clicked: check that branch out. Fired for the
    /// label only, never for the row — a double-click on a commit does not move the
    /// working tree, and a client where it does is a client that loses work.
    void branchCheckoutRequested(QString shortName, gk::RefKind kind);
    /// The badge has already started home by the time this fires.
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

    /// The reference under a viewport position, or nullptr when the point is
    /// not on a label.
    const RefInfo* refAt(const QPoint& viewportPos, QString* shortNameOut = nullptr) const;

    /// The pills laid out for a labelled row, exactly as the delegate paints them.
    std::vector<RefPill> rowPills(int row) const;

    /// Builds the hover overlay for a collapsed pill under `pos`, keeps it while the
    /// pointer is inside it, and clears it otherwise.
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
    /// What the animator diffs: id, row and lane of every loaded commit.
    std::vector<GraphRowKey> animationRows() const;
    std::set<std::string> animationLabels() const;
    QString animationHead() const;
    /// Grabs the graph cell of every visible row, so it can glide after a change.
    void captureGhosts();
    /// Computes the transitions for the change that just settled, starts the timer.
    void observeAnimation();
    void startAnimationTimer();
    void paintAnimation(QPainter& painter) const;
    void paintPulse(QPainter& painter, const GraphTransition& pulse) const;
    /// Formats the animator's transitions, one line each.
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
    /// What the last change produced. Kept past expiration for the headless check.
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
    /// The badge that is in the air and the branch it is over.
    QString m_flightSource;
    QString m_flightTarget;
    /// Filled in when the badge is lifted so the delegate's own label can be covered
    /// while the badge is elsewhere.
    QRect m_vacatedPill;
    /// The prediction shown in the open operation list, filled by the window in answer
    /// to `dropOperationsOpened`. Cleared before that signal fires.
    QString m_dropPreview;
    bool m_dropPreviewWarns = false;

    /// The branch label under a dragged branch: outlined and pulsing, with what
    /// releasing would offer, so the drop's target is never a guess.
    void setDropTarget(const QString& name, const QRect& pill);
    void paintDropTarget(QPainter& painter);
    QString m_dropTarget;
    QRect m_dropTargetRect;
    QVariantAnimation* m_dropPulse = nullptr;
};

} // namespace gk
