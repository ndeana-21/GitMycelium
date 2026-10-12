#include "ui/commit_graph_view.h"

#include "ui/drop_point.h"

#include "model/commit_model.h"
#include "ui/graph_delegate.h"
#include "ui/ref_pill_layout.h"
#include "ui/refs_delegate.h"

#include "theme/theme.h"

#include <QApplication>
#include <QDateTime>
#include <QDrag>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QKeyEvent>
#include <QPushButton>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QThread>
#include <QScrollBar>
#include <QTimer>

#include <algorithm>
#include <cstdio>

namespace gk {

CommitGraphView::CommitGraphView(QWidget* parent) : QTreeView(parent) {
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            &CommitGraphView::fetchIfNearEnd);
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this,
            &CommitGraphView::fetchIfNearEnd);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
    // The pointer has to be followed without a button held down: the hover badge has no
    // click to hang it off.
    setMouseTracking(true);

    m_flightTimer = new QTimer(this);
    m_flightTimer->setInterval(16);
    connect(m_flightTimer, &QTimer::timeout, this, &CommitGraphView::advanceFlight);

    // The graph's own motion — fades, glides, the checkout pulse — on a timer of its own,
    // so it runs whether or not a badge is in the air.
    m_animTimer = new QTimer(this);
    m_animTimer->setInterval(16);
    connect(m_animTimer, &QTimer::timeout, this, &CommitGraphView::advanceAnimation);

    // A child of the viewport, clamped into it when the badge is near the edge. A
    // popup would be dismissed by the release that ended the drag.
    m_panel = new GraphOperationPanel(viewport());
    connect(m_panel, &GraphOperationPanel::chosen, this, [this](RefOperation operation) {
        const QString source = m_flightSource;
        const QString target = m_flightTarget;
        logBadgeFlow(QStringLiteral("view: chosen %1 — %2 onto %3")
                         .arg(static_cast<int>(operation))
                         .arg(source, target));
        // The badge starts home first: whatever the operation does, the list is closed
        // and nothing is waiting on the air any more.
        finishFlight();
        switch (operation) {
        case RefOperation::Merge:
            logBadgeFlow(QStringLiteral("view: emitting mergeRequested %1 -> %2")
                             .arg(source, target));
            emit mergeRequested(source, target);
            break;
        case RefOperation::Rebase:
            logBadgeFlow(QStringLiteral("view: emitting rebaseRequested %1 -> %2")
                             .arg(source, target));
            emit rebaseRequested(source, target);
            break;
        case RefOperation::Squash:
            logBadgeFlow(QStringLiteral("view: emitting squashRequested %1 -> %2")
                             .arg(source, target));
            emit squashRequested(source, target);
            break;
        case RefOperation::Checkout:
            emit checkoutTargetRequested(target);
            break;
        case RefOperation::InteractiveRebase:
            logBadgeFlow(QStringLiteral("view: emitting interactiveRebaseRequested %1 -> %2")
                             .arg(source, target));
            emit interactiveRebaseRequested(source, target);
            break;
        }
    });
    connect(m_panel, &GraphOperationPanel::dismissed, this, [this] {
        // Nothing was chosen, but the badge still has to come home — this is the path
        // that used to leave it hanging.
        finishFlight();
    });
}

void CommitGraphView::setAnimationsEnabled(bool on) {
    m_animationsEnabled = on;
    if (!on) {
        clearAnimation();
    }
}

void CommitGraphView::clearAnimation() {
    m_animation.clear();
    m_ghosts.clear();
    m_capturePending = false;
    if (m_animTimer) {
        m_animTimer->stop();
    }
    viewport()->update();
}

void CommitGraphView::setBusy(bool busy) {
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    if (busy) {
        startAnimationTimer();
    } else if (m_animation.empty() && m_animTimer) {
        m_animTimer->stop();
    }
    viewport()->update();
}

void CommitGraphView::setModel(QAbstractItemModel* model) {
    if (model == this->model()) {
        // `setModel` is called again when the window rebinds its views, and it is the
        // same model. Falling through would disconnect every connection from the model
        // to this view — including the ones QTreeView made — and then
        // QAbstractItemView::setModel would return early because the model has not
        // changed, so nothing would be reconnected. The view then stops reacting to
        // the model: rows appear and disappear and the layout never learns, which is
        // the "the graph shows nothing until I reopen the repository" bug.
        return;
    }
    if (QAbstractItemModel* previous = this->model()) {
        disconnect(previous, nullptr, this, nullptr);
    }
    QTreeView::setModel(model);
    if (!model) {
        return;
    }
    connect(model, &QAbstractItemModel::modelAboutToBeReset, this,
            &CommitGraphView::onModelAboutToBeReset);
    connect(model, &QAbstractItemModel::rowsAboutToBeInserted, this,
            &CommitGraphView::onRowsAboutToBeInserted);
    connect(model, &QAbstractItemModel::rowsInserted, this, &CommitGraphView::onRowsInserted);
    connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this,
            &CommitGraphView::onRowsAboutToBeRemoved);
    connect(model, &QAbstractItemModel::rowsRemoved, this, &CommitGraphView::onRowsRemoved);
    // Queued, so the check runs after the insertion has finished, not inside it.
    connect(model, &QAbstractItemModel::rowsInserted, this, &CommitGraphView::fetchIfNearEnd,
            Qt::QueuedConnection);
    connect(model, &QAbstractItemModel::modelReset, this, &CommitGraphView::fetchIfNearEnd,
            Qt::QueuedConnection);
}

void CommitGraphView::fetchIfNearEnd() {
    auto* commits = qobject_cast<CommitModel*>(model());
    if (!commits || !commits->canLoadMore()) {
        return;
    }
    // The scrollbar is only right once pending rows are laid out; read before that, a
    // page that just landed looks like the end of the history and asks for another.
    executeDelayedItemsLayout();
    // One page ahead of the reader: within two screens of the end of what is loaded.
    const QScrollBar* bar = verticalScrollBar();
    if (bar->maximum() - bar->value() <= 2 * std::max(1, bar->pageStep())) {
        commits->fetchMore(QModelIndex());
    }
}

std::vector<GraphRowKey> CommitGraphView::animationRows() const {
    std::vector<GraphRowKey> out;
    auto* model = qobject_cast<CommitModel*>(this->model());
    if (!model) {
        return out;
    }
    const int rows = model->rowCount();
    out.reserve(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        const GraphNode* node = model->graphNodeAt(row);
        out.push_back({commitIdAtRow(row).toStdString(), row, node ? node->lane : 0});
    }
    return out;
}

std::set<std::string> CommitGraphView::animationLabels() const {
    std::set<std::string> out;
    auto* model = qobject_cast<CommitModel*>(this->model());
    if (!model) {
        return out;
    }
    const int rows = model->rowCount();
    for (int row = 0; row < rows; ++row) {
        for (const RefInfo& ref : model->refsAt(row)) {
            out.insert(ref.shortName);
        }
    }
    return out;
}

QString CommitGraphView::animationHead() const {
    auto* model = qobject_cast<CommitModel*>(this->model());
    return model ? QString::fromStdString(model->headTipId()) : QString();
}

void CommitGraphView::captureGhosts() {
    m_ghosts.clear();
    auto* model = qobject_cast<CommitModel*>(this->model());
    if (!model) {
        return;
    }
    const QRect viewportRect = viewport()->rect();
    const int rows = model->rowCount();
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = model->index(row, CommitModel::Graph);
        const QRect rect = visualRect(index);
        if (!rect.intersects(viewportRect)) {
            continue;
        }
        const QString oid = commitIdAtRow(row);
        if (oid.isEmpty()) {
            continue;
        }
        const GraphNode* node = model->graphNodeAt(row);
        Ghost ghost;
        ghost.pixmap = viewport()->grab(rect);
        ghost.rect = rect;
        ghost.lane = node ? node->lane : 0;
        m_ghosts[oid.toStdString()] = std::move(ghost);
    }
}

void CommitGraphView::observeAnimation() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_animation.observe(animationRows(), animationLabels(), animationHead().toStdString(), now);
    m_capturePending = false;
    m_lastAnimationTrace = formatTransitions();
    if (!m_animationsEnabled || m_animation.empty()) {
        m_ghosts.clear();
        return;
    }
    startAnimationTimer();
}

void CommitGraphView::startAnimationTimer() {
    if (!m_animationsEnabled || !m_animTimer) {
        return;
    }
    if (!m_animTimer->isActive()) {
        m_animTimer->start();
    }
}

void CommitGraphView::advanceAnimation() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool more = m_animation.advance(now);
    if (more) {
        // A ghost is only worth keeping while its commit still has something to do.
        for (auto it = m_ghosts.begin(); it != m_ghosts.end();) {
            if (!m_animation.find(it->first, GraphMotion::Move) &&
                !m_animation.find(it->first, GraphMotion::Vanish)) {
                it = m_ghosts.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        m_ghosts.clear();
        if (!m_busy) {
            m_animTimer->stop();
        }
    }
    viewport()->update();
}

void CommitGraphView::onModelAboutToBeReset() {
    // The overlay points at rows that are about to be replaced; its coordinates
    // would be stale, so it goes with them.
    clearHoverExpansion();
    if (!m_animationsEnabled) {
        return;
    }
    // The rows are still in place; a reset empties them and the first page arrives a
    // moment later, so the change is measured across that gap.
    captureGhosts();
    m_capturePending = true;
}

void CommitGraphView::onRowsAboutToBeInserted(const QModelIndex& parent, int first, int last) {
    Q_UNUSED(parent);
    Q_UNUSED(last);
    if (!m_animationsEnabled || first != 0 || m_capturePending) {
        return;
    }
    // The working-tree row is appearing (staging, a fresh change): its arrival at row 0
    // is the signal, a page appended at the tail is not.
    captureGhosts();
    m_capturePending = true;
}

void CommitGraphView::onRowsInserted(const QModelIndex& parent, int first, int last) {
    Q_UNUSED(parent);
    Q_UNUSED(first);
    Q_UNUSED(last);
    if (!m_animationsEnabled) {
        m_animation.baseline(animationRows(), animationLabels(), animationHead().toStdString());
        return;
    }
    if (m_capturePending) {
        observeAnimation();
        return;
    }
    // Either the first page after an open (the starting point, not a change) or a page
    // paged in on scroll. Neither should animate.
    m_animation.baseline(animationRows(), animationLabels(), animationHead().toStdString());
}

void CommitGraphView::onRowsAboutToBeRemoved(const QModelIndex& parent, int first, int last) {
    Q_UNUSED(parent);
    Q_UNUSED(last);
    if (!m_animationsEnabled || first != 0 || m_capturePending) {
        return;
    }
    captureGhosts();
    m_capturePending = true;
}

void CommitGraphView::onRowsRemoved(const QModelIndex& parent, int first, int last) {
    Q_UNUSED(parent);
    Q_UNUSED(first);
    Q_UNUSED(last);
    if (!m_animationsEnabled) {
        m_animation.baseline(animationRows(), animationLabels(), animationHead().toStdString());
        return;
    }
    if (m_capturePending) {
        observeAnimation();
    }
}

double CommitGraphView::animationFadeAlpha(const QString& oid) const {
    if (!m_animationsEnabled || m_animation.empty()) {
        return 1.0;
    }
    const GraphTransition* appear = m_animation.find(oid.toStdString(), GraphMotion::Appear);
    if (!appear) {
        return 1.0;
    }
    return GraphAnimation::progress(*appear, QDateTime::currentMSecsSinceEpoch());
}

bool CommitGraphView::animationIsMorphing(const QString& oid) const {
    if (!m_animationsEnabled || m_animation.empty()) {
        return false;
    }
    return m_animation.find(oid.toStdString(), GraphMotion::Move) != nullptr;
}

double CommitGraphView::animationLabelAlpha(const QString& name) const {
    if (!m_animationsEnabled || m_animation.empty()) {
        return 1.0;
    }
    const GraphTransition* in =
        m_animation.findLabel(name.toStdString(), GraphMotion::LabelIn);
    if (!in) {
        return 1.0;
    }
    return GraphAnimation::progress(*in, QDateTime::currentMSecsSinceEpoch());
}

QStringList CommitGraphView::animationForDiagnostics() const {
    return formatTransitions();
}

QStringList CommitGraphView::formatTransitions() const {
    QStringList report;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const GraphTransition& t : m_animation.transitions()) {
        const double p = GraphAnimation::progress(t, now);
        QString what;
        switch (t.motion) {
        case GraphMotion::Appear: what = QStringLiteral("appear"); break;
        case GraphMotion::Vanish: what = QStringLiteral("vanish"); break;
        case GraphMotion::Move: what = QStringLiteral("move"); break;
        case GraphMotion::Pulse: what = QStringLiteral("pulse"); break;
        case GraphMotion::LabelIn: what = QStringLiteral("label-in"); break;
        case GraphMotion::LabelOut: what = QStringLiteral("label-out"); break;
        }
        const QString id = !t.oid.empty()
                               ? QString::fromStdString(t.oid).left(10)
                               : QStringLiteral("label:%1")
                                     .arg(QString::fromStdString(t.label));
        report << QStringLiteral("%1 %2 row %3->%4 lane %5->%6 p=%7")
                      .arg(what, id)
                      .arg(t.fromRow)
                      .arg(t.toRow)
                      .arg(t.fromLane)
                      .arg(t.toLane)
                      .arg(p, 0, 'f', 2);
    }
    return report;
}

void CommitGraphView::paintAnimation(QPainter& painter) const {
    if (!m_animationsEnabled) {
        return;
    }
    auto* model = qobject_cast<CommitModel*>(this->model());
    if (!model) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QRect viewportRect = viewport()->rect();

    painter.save();
    painter.setClipRect(viewportRect, Qt::IntersectClip);

    // A running operation: a quiet band sweeping down the graph, so a long fetch or
    // rebase reads as work rather than as a frozen window.
    if (m_busy) {
        const qreal phase = double(now % 1000) / 1000.0;
        const int y = viewportRect.top() + int(phase * viewportRect.height());
        QColor band = theme::accent();
        band.setAlpha(26);
        painter.fillRect(QRect(viewportRect.left(), y - 14, viewportRect.width(), 28), band);
    }

    for (const auto& [oid, ghost] : m_ghosts) {
        const GraphTransition* move = m_animation.find(oid, GraphMotion::Move);
        const GraphTransition* vanish = m_animation.find(oid, GraphMotion::Vanish);
        if (!move && !vanish) {
            continue;
        }
        const GraphTransition& t = move ? *move : *vanish;
        const double p = GraphAnimation::progress(t, now);

        QPoint target = ghost.rect.topLeft();
        if (move) {
            int newTop = ghost.rect.top();
            int newLeft = ghost.rect.left();
            const int newRow = model->rowForCommit(QString::fromStdString(oid));
            if (newRow >= 0) {
                const QRect rect = visualRect(model->index(newRow, CommitModel::Graph));
                newTop = rect.top();
                newLeft = rect.left();
            }
            // The lane the commit left, still drawn at its old x, drifts to the new one.
            const double laneShift =
                (t.fromLane - t.toLane) * double(GraphDelegate::laneWidth()) * (1.0 - p);
            target.setX(newLeft + int(laneShift));
            target.setY(int(newTop + (ghost.rect.top() - newTop) * (1.0 - p)));
        }
        painter.setOpacity(move ? 1.0 : (1.0 - p));
        painter.drawPixmap(target, ghost.pixmap);
    }
    painter.setOpacity(1.0);

    for (const GraphTransition& t : m_animation.transitions()) {
        if (t.motion == GraphMotion::Pulse) {
            paintPulse(painter, t);
        }
    }
    painter.restore();
}

void CommitGraphView::paintPulse(QPainter& painter, const GraphTransition& pulse) const {
    auto* model = qobject_cast<CommitModel*>(this->model());
    if (!model) {
        return;
    }
    const int row = model->rowForCommit(QString::fromStdString(pulse.oid));
    if (row < 0) {
        return;
    }
    const GraphNode* node = model->graphNodeAt(row);
    if (!node) {
        return;
    }
    const QRect rect = visualRect(model->index(row, CommitModel::Graph));
    if (!rect.intersects(viewport()->rect())) {
        return;
    }
    const qreal x = rect.left() + GraphDelegate::lanePadding() +
                    GraphDelegate::laneWidth() * node->lane;
    const qreal y = rect.center().y();
    const double p = GraphAnimation::progress(pulse, QDateTime::currentMSecsSinceEpoch());
    const double radius = double(GraphDelegate::nodeRadius()) + 2.0 + 11.0 * p;
    QColor colour = theme::accent();
    colour.setAlphaF(qBound(0.0, 1.0 - p, 1.0) * 0.9);
    QPen pen(colour);
    pen.setWidthF(2.0);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QPointF(x, y), radius, radius);
}

QStringList CommitGraphView::simulateBranchDrop(const QString& source, const QString& target,
                                                const QString& pick) {
    QStringList report;
    bool rebased = false;
    bool merged = false;
    bool checkedOut = false;
    const QMetaObject::Connection r = connect(this, &CommitGraphView::rebaseRequested, this,
                                              [&rebased](const QString&, const QString&) {
                                                  rebased = true;
                                              });
    const QMetaObject::Connection m = connect(this, &CommitGraphView::mergeRequested, this,
                                              [&merged](const QString&, const QString&) {
                                                  merged = true;
                                              });
    const QMetaObject::Connection c = connect(this, &CommitGraphView::checkoutTargetRequested, this,
                                              [&checkedOut](const QString&) { checkedOut = true; });
    bool interactive = false;
    const QMetaObject::Connection i =
        connect(this, &CommitGraphView::interactiveRebaseRequested, this,
                [&interactive](const QString&, const QString&) { interactive = true; });

    QRect pill;
    const QPoint home = homeOf(target, QPoint(320, 120));
    pill = m_vacatedPill.isNull() ? QRect(home.x() - 40, home.y() - 10, 80, 20) : m_vacatedPill;

    RefModel::DraggedRef drag;
    drag.shortName = source;
    drag.kind = RefKind::LocalBranch;
    beginFlight(drag, target, pill);
    report << QStringLiteral("panel visible: %1").arg(m_panel->isVisible() ? QStringLiteral("yes")
                                                                          : QStringLiteral("no"));
    report << QStringLiteral("badge lifted: %1").arg(m_flight.lifted() ? QStringLiteral("yes")
                                                                      : QStringLiteral("no"));
    report << QStringLiteral("entries: %1")
                  .arg(m_panel->findChildren<QPushButton*>().size());
    report << QStringLiteral("preview: %1")
                  .arg(m_dropPreview.isEmpty() ? QStringLiteral("(none)") : m_dropPreview);

    if (!pick.isEmpty()) {
        QPushButton* hit = nullptr;
        for (QPushButton* button : m_panel->findChildren<QPushButton*>()) {
            if (button->isEnabled() && button->text().startsWith(pick)) {
                hit = button;
                break;
            }
        }
        report << QStringLiteral("button found: %1").arg(hit ? QStringLiteral("yes")
                                                             : QStringLiteral("no"));
        if (hit) {
            // Who is under the pointer matters more than whether the widget exists: a
            // button nobody can click looks exactly like a button that does nothing. This
            // is the hit test the platform would perform.
            const QPoint centre = hit->mapToGlobal(hit->rect().center());
            QWidget* under = QApplication::widgetAt(centre);
            report << QStringLiteral("under the button: %1 (button is %2)")
                          .arg(under ? QString::fromLatin1(under->metaObject()->className())
                                     : QStringLiteral("nothing"),
                               QString::fromLatin1(hit->metaObject()->className()));
            report << QStringLiteral("same widget: %1")
                          .arg(under == hit ? QStringLiteral("yes") : QStringLiteral("no"));
        }
        if (hit) {
            hit->click();
        }
        report << QStringLiteral("signals — rebase: %1 merge: %2 interactive: %3 checkout: %4")
                      .arg(rebased ? QStringLiteral("yes") : QStringLiteral("no"),
                           merged ? QStringLiteral("yes") : QStringLiteral("no"),
                           interactive ? QStringLiteral("yes") : QStringLiteral("no"),
                           checkedOut ? QStringLiteral("yes") : QStringLiteral("no"));
        // Let the return flight actually happen before asking whether it did: a report
        // taken mid-air says nothing.
        for (int waited = 0; waited < 400 && !m_flight.grounded(); waited += 20) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(20);
        }
        report << QStringLiteral("badge grounded: %1").arg(m_flight.grounded()
                                                               ? QStringLiteral("yes")
                                                               : QStringLiteral("no"));
    }

    disconnect(r);
    disconnect(m);
    disconnect(c);
    disconnect(i);
    return report;
}

void CommitGraphView::beginFlight(const RefModel::DraggedRef& source, const QString& target,
                                  const QRect& targetPill) {
    const QFontMetrics metrics(font());
    const int badgeWidth = badgeWidthFor(source.shortName, metrics);
    const QPoint anchor = parkedBadgeCentre(targetPill, badgeWidth);
    const QPoint fallback = viewport()->mapFromGlobal(lastDropPoint());
    m_flightSource = source.shortName;
    m_flightTarget = target;
    m_vacatedPill = QRect();
    m_flight.lift(source.shortName, homeOf(source.shortName, fallback), anchor);

    // The prediction is asked for just before the list is shown and answered
    // synchronously: the window owns git access, this view does not, and the answer has to
    // be in place before the list measures itself around it.
    m_dropPreview.clear();
    m_dropPreviewWarns = false;
    emit dropOperationsOpened(source.shortName, target);

    // The list opens to the right of the badge, itself to the right of the branch it was
    // dropped on. A child of the viewport: a popup here is closed by the mouse release
    // that ended the drag, before anyone can read it.
    m_panel->present(source.shortName, target, listTopLeft(anchor, badgeWidth), viewport()->rect(),
                     m_dropPreview, m_dropPreviewWarns);
    m_flightTimer->start();
    viewport()->update();
}

void CommitGraphView::setDropPreview(const QString& text, bool warns) {
    m_dropPreview = text;
    m_dropPreviewWarns = warns;
}

void CommitGraphView::finishFlight(bool immediately) {
    if (!m_flight.lifted()) {
        return;
    }
    if (immediately) {
        m_flight.landImmediately();
        m_flightTimer->stop();
    } else {
        m_flight.land();
        if (!m_flightTimer->isActive()) {
            m_flightTimer->start();
        }
    }
    if (m_panel->isVisible()) {
        // Closing it here would report a dismissal and call back into this function; the
        // panel's reported flag makes that a no-op, so the badge ends up home exactly once.
        m_panel->hide();
    }
    viewport()->update();
}

void CommitGraphView::advanceFlight() {
    m_flight.advance(m_flightTimer->interval());
    if (m_flight.grounded()) {
        m_flightTimer->stop();
        m_vacatedPill = QRect();
        viewport()->update();
        return;
    }
    // Repaint while the badge is moving, stop when it is parked: a full-viewport repaint
    // sixty times a second on a graph of a hundred thousand rows makes every click dead.
    if (!m_flight.flying()) {
        m_flightTimer->stop();
        viewport()->update();
        return;
    }
    viewport()->update();
}

QPoint CommitGraphView::homeOf(const QString& branch, const QPoint& fallback) {
    const auto* model = qobject_cast<const CommitModel*>(this->model());
    if (!model) {
        return fallback;
    }
    // The label lives on whichever loaded row carries it; a branch whose row is not
    // loaded has no row to come home to, so the drop point is the honest answer.
    const int rows = model->rowCount();
    for (int row = 0; row < rows; ++row) {
        // Through rowPills, not a private copy of the layout: a tracking badge widens a
        // pill, and a home computed without it would park the badge beside a pill that is
        // not there.
        for (const RefPill& pill : rowPills(row)) {
            if (pill.shortName == branch) {
                m_vacatedPill = pill.rect;
                return pill.rect.center();
            }
        }
    }
    return fallback;
}

void CommitGraphView::hideEvent(QHideEvent* event) {
    // The window is going away and no animation will ever run again: a badge left in the
    // air would be stuck there.
    finishFlight(true);
    QTreeView::hideEvent(event);
}

void CommitGraphView::scrollContentsBy(int dx, int dy) {
    QTreeView::scrollContentsBy(dx, dy);
    if (dy == 0 && dx == 0) {
        return;
    }
    // Ghosts live in viewport coordinates; once the rows move under them they point at the
    // wrong place, so they go. The hover overlay is viewport-anchored to a row the same way.
    m_ghosts.clear();
    clearHoverExpansion();
    if (m_animationsEnabled && !m_animation.empty()) {
        viewport()->update();
    }
}

void CommitGraphView::paintEvent(QPaintEvent* event) {
    QTreeView::paintEvent(event);

    QPainter animationPainter(viewport());
    paintAnimation(animationPainter);

    // The ghost badge belongs to the row under the pointer, or to the current row so it can
    // be double-clicked. Painted every frame rather than cached anywhere.
    QPainter hoverPainter(viewport());
    paintHoverExpansion(hoverPainter);
    paintGhostBadge(hoverPainter);

    if (!m_flight.lifted()) {
        return;
    }
    QPainter painter(viewport());
    // Cover the label the badge came from while it is elsewhere, so the journey reads as
    // one badge moving rather than two existing at once.
    if (!m_vacatedPill.isNull()) {
        painter.fillRect(m_vacatedPill, palette().base());
    }
    paintFlyingBadge(painter);
}

int CommitGraphView::rowAtViewportPos(const QPoint& pos) const {
    const QModelIndex index = indexAt(pos);
    return index.isValid() ? index.row() : -1;
}

void CommitGraphView::setHoveredRow(int row) {
    if (m_hoverRow == row) {
        return;
    }
    m_hoverRow = row;
    viewport()->update();
}

void CommitGraphView::setHoverRowForDiagnostics(int row) {
    setHoveredRow(row);
    // The headless hover drives the whole chain: view → refHovered → model highlight →
    // delegate dimming. Marking only the row would test the badge and nothing else.
    setHoveredRef(row >= 0 ? highlightBranchAt(row) : QString());
}

void CommitGraphView::setHoverExpansionForDiagnostics(int row) {
    for (const RefPill& pill : rowPills(row)) {
        if (!pill.overflow && !pill.collapsedNames.isEmpty()) {
            updateHoverExpansion(pill.rect.center());
            return;
        }
    }
    clearHoverExpansion();
}

QStringList CommitGraphView::expandedBranchesForDiagnostics() const {
    QStringList names;
    for (const RefPill& pill : m_expandPills) {
        names << pill.shortName;
    }
    return names;
}

void CommitGraphView::mouseMoveEvent(QMouseEvent* event) {
    const QPoint pos = event->position().toPoint();
    setHoveredRow(rowAtViewportPos(pos));
    // The hover overlay reveals the branches a collapsed pill hides; it is recomputed
    // before the hovered reference, because a revealed pill is a label the pointer can be
    // on.
    updateHoverExpansion(pos);

    // A real label first, then the ghost badge on an unlabelled row, which names the same
    // branch the badge shows.
    QString name;
    if (refAt(pos, &name)) {
        setHoveredRef(name);
    } else {
        const int row = rowAtViewportPos(pos);
        const HoverLabel label = row >= 0 ? hoverLabelAt(row) : HoverLabel{};
        setHoveredRef(label.valid ? label.text : QString());
    }

    QTreeView::mouseMoveEvent(event);
}

void CommitGraphView::setHoveredRef(const QString& name) {
    if (m_hoveredRef == name) {
        return;
    }
    m_hoveredRef = name;
    emit refHovered(name);
}

void CommitGraphView::leaveEvent(QEvent* event) {
    setHoveredRow(-1);
    setHoveredRef(QString());
    clearHoverExpansion();
    QTreeView::leaveEvent(event);
}

int CommitGraphView::nearestLabelledChildRow(int row) const {
    const auto* model = qobject_cast<const CommitModel*>(this->model());
    if (!model || row < 0) {
        return -1;
    }

    // Walk the chain of children, one commit at a time, until one wearing a label is
    // reached. Children rather than rows: a row above may belong to a different branch,
    // and its label would name the wrong one. Among several children the one in the same
    // lane continues this commit's own line, so it is preferred — a shared ancestor is
    // named after the line it is drawn on.
    int current = row;
    for (int step = 0; step < 500; ++step) {
        const QString id =
            model->data(model->index(current, 0), CommitModel::CommitIdRole).toString();
        if (id.isEmpty()) {
            return -1;
        }
        const GraphNode* node = model->graphNodeAt(current);
        const int lane = node ? node->lane : -1;
        int sameLane = -1;
        int anyChild = -1;
        for (int candidate = current - 1; candidate >= 0; --candidate) {
            const auto* commit = model->commitAt(candidate);
            if (!commit) {
                continue;
            }
            bool isChild = false;
            for (const std::string& parent : commit->parents) {
                if (QString::fromStdString(parent) == id) {
                    isChild = true;
                    break;
                }
            }
            if (!isChild) {
                continue;
            }
            if (anyChild < 0) {
                anyChild = candidate;
            }
            const GraphNode* childNode = model->graphNodeAt(candidate);
            if (lane >= 0 && childNode && childNode->lane == lane) {
                sameLane = candidate;
                break;
            }
        }
        const int child = sameLane >= 0 ? sameLane : anyChild;
        if (child < 0) {
            // Nothing above descends from it: a branch tip with no label, or the oldest
            // commit loaded. There is no branch to name.
            return -1;
        }
        if (!model->refsAt(child).empty()) {
            return child;
        }
        current = child;
    }
    return -1;
}

CommitGraphView::HoverLabel CommitGraphView::hoverLabelAt(int row) const {
    HoverLabel label;
    const auto* model = qobject_cast<const CommitModel*>(this->model());
    if (!model || row < 0) {
        return label;
    }
    if (!model->refsAt(row).empty()) {
        return label;
    }

    // The branch that owns the commit's line: the nearest labelled commit above it,
    // following the child drawn in the same lane. A shared ancestor is named after the line
    // it is drawn on, not after whichever branch passes nearer.
    const int labelled = nearestLabelledChildRow(row);
    if (labelled < 0) {
        return label;
    }

    const std::vector<RefInfo>& refs = model->refsAt(labelled);
    // The pill stack the delegate would paint on that row, tracking badge and all.
    const std::vector<RefPill> pills = rowPills(labelled);
    if (pills.empty() || pills.front().overflow) {
        return label;
    }

    // The first pill is the branch that row would name, and the collapsed-pill rule already
    // decided which one that is. The hover badge says the same thing in the same colour.
    label.valid = true;
    label.text = pills.front().shortName;
    label.colorIndex = model->refColorIndex(label.text);
    label.kind = RefKind::LocalBranch;
    for (const RefInfo& ref : refs) {
        if (QString::fromStdString(ref.shortName) == label.text) {
            label.kind = ref.kind;
            break;
        }
    }
    return label;
}

QString CommitGraphView::describeHover(int row) const {
    const auto* model = qobject_cast<const CommitModel*>(this->model());
    if (!model || row < 0 || row >= model->rowCount()) {
        return QStringLiteral("row %1: out of range").arg(row);
    }
    QStringList own;
    for (const RefInfo& ref : model->refsAt(row)) {
        own << QString::fromStdString(ref.shortName);
    }
    const HoverLabel label = hoverLabelAt(row);
    if (!own.isEmpty()) {
        return QStringLiteral("row %1: labels %2 — no badge").arg(row).arg(own.join(QLatin1Char(',')));
    }
    if (!label.valid) {
        return QStringLiteral("row %1: unlabelled — no badge").arg(row);
    }
    return QStringLiteral("row %1: unlabelled — badge \"%2\" colour %3")
        .arg(row)
        .arg(label.text)
        .arg(label.colorIndex);
}

QStringList CommitGraphView::hoverBranchesAt(int row) const {
    for (const RefPill& pill : rowPills(row)) {
        if (!pill.overflow && !pill.collapsedNames.isEmpty()) {
            return pill.collapsedNames;
        }
    }
    return {};
}

void CommitGraphView::clearHoverExpansion() {
    if (m_expandPills.empty() && m_expandRow < 0 && m_expandHovered.isEmpty()) {
        return;
    }
    m_expandPills.clear();
    m_expandRow = -1;
    m_expandSource.clear();
    m_expandHovered.clear();
    m_expandSourceRect = QRect();
    m_expandKeepOpen = QRect();
    m_expandHovering = false;
    viewport()->update();
}

void CommitGraphView::updateHoverExpansion(const QPoint& pos) {
    // The pointer inside the revealed stack highlights that branch. The pointer in
    // the gap between the pill and the stack keeps the overlay open but highlights
    // the source: the reader is on their way from one to the other, and closing here
    // is what made the stack impossible to reach.
    if (!m_expandPills.empty()) {
        if (const RefPill* hit = RefPillLayout::hitTest(m_expandPills, pos)) {
            m_expandHovering = true;
            if (m_expandHovered != hit->shortName) {
                m_expandHovered = hit->shortName;
                viewport()->update();
            }
            return;
        }
        if (m_expandKeepOpen.contains(pos)) {
            m_expandHovering = true;
            if (m_expandHovered != m_expandSource) {
                m_expandHovered = m_expandSource;
                viewport()->update();
            }
            return;
        }
    }
    m_expandHovering = false;

    const QModelIndex index = indexAt(pos);
    const int row = index.isValid() ? index.row() : -1;
    RefPill source;
    bool found = false;
    if (row >= 0) {
        for (const RefPill& pill : rowPills(row)) {
            if (pill.rect.contains(pos) && !pill.overflow && !pill.collapsedNames.isEmpty()) {
                source = pill;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        if (!m_expandPills.empty()) {
            clearHoverExpansion();
        }
        return;
    }
    if (m_expandRow == row && m_expandSource == source.shortName) {
        return;
    }
    m_expandRow = row;
    m_expandSource = source.shortName;
    m_expandHovered = source.shortName;

    const auto* model = qobject_cast<const CommitModel*>(this->model());
    const QFont font = theme::labelFont();
    const QFontMetrics metrics(font);
    const int pad = 6;
    const int iconRoom = 14;
    const int pillH = std::max(14, metrics.height() + 6);

    int width = 0;
    for (const QString& name : source.collapsedNames) {
        const QString tracking =
            RefPillLayout::trackingText(model ? model->refTrackingFlags(name) : 0);
        const int badgeRoom = tracking.isEmpty() ? 0 : metrics.horizontalAdvance(tracking) + 4;
        width = std::max(width,
                         metrics.horizontalAdvance(name) + 2 * pad + iconRoom + badgeRoom);
    }
    width += 2;
    // At least as wide as the pill it opens from, so the stack lines up with it and the
    // pointer can travel straight down without leaving the keep-open region.
    width = std::max(width, source.rect.width());
    const int x = source.rect.left();
    int y = source.rect.bottom() + 2;
    const int totalH = static_cast<int>(source.collapsedNames.size()) * pillH + 2;
    if (y + totalH > viewport()->height() - 2) {
        // No room below: the stack opens above the pill instead.
        y = source.rect.top() - 2 - totalH;
    }
    y = std::max(y, 2);

    m_expandPills.clear();
    for (const QString& name : source.collapsedNames) {
        RefKind kind = RefKind::LocalBranch;
        int colorIndex = 0;
        if (model) {
            for (const RefInfo& ref : model->refsAt(row)) {
                if (QString::fromStdString(ref.shortName) == name) {
                    kind = ref.kind;
                    colorIndex = model->refColorIndex(name);
                    break;
                }
            }
        }
        const QString tracking =
            RefPillLayout::trackingText(model ? model->refTrackingFlags(name) : 0);
        const int badgeW = tracking.isEmpty() ? 0 : metrics.horizontalAdvance(tracking);
        const int badgeRoom = badgeW > 0 ? badgeW + 4 : 0;
        const bool hasKindIcon = kind == RefKind::LocalBranch || kind == RefKind::RemoteBranch;
        const int iconSide = std::min(11, std::max(6, pillH - 5));

        RefPill pill;
        pill.shortName = name;
        pill.text = metrics.elidedText(name, Qt::ElideRight,
                                       width - 2 * pad - iconRoom - badgeRoom);
        pill.colorIndex = colorIndex;
        pill.kind = kind;
        pill.rect = QRect(x, y, width, pillH);
        pill.textRect = QRect(x + pad, y, width - 2 * pad - iconRoom - badgeRoom, pillH);
        if (hasKindIcon) {
            pill.iconRect = QRect(x + width - pad - iconSide, y + (pillH - iconSide) / 2, iconSide,
                                  iconSide);
        }
        if (badgeW > 0) {
            const int right = hasKindIcon ? x + width - pad - iconSide - 2 : x + width - pad;
            pill.trackingText = tracking;
            pill.badgeRect = QRect(right - badgeW, y, badgeW, pillH);
        }
        m_expandPills.push_back(pill);
        y += pillH;
    }

    // The region that keeps the overlay open: the pill, the stack, and the space
    // between them, so the pointer can travel without the popup vanishing.
    m_expandSourceRect = source.rect;
    QRect panel = m_expandPills.front().rect;
    for (const RefPill& pill : m_expandPills) {
        panel = panel.united(pill.rect);
    }
    m_expandKeepOpen = source.rect.united(panel.adjusted(-4, -4, 4, 4));
    viewport()->update();
}

void CommitGraphView::hoverMoveForDiagnostics(const QPoint& viewportPos) {
    updateHoverExpansion(viewportPos);
}

void CommitGraphView::pressRefForDiagnostics(const QPoint& viewportPos) {
    m_pressPos = viewportPos;
    m_pressedRefValid = false;
    QString focused;
    if (const RefInfo* ref = refAt(viewportPos, &focused)) {
        m_pressedRef = *ref;
        m_pressedRefValid = true;
    }
}

QRect CommitGraphView::hoverExpansionPillRect(const QString& branch) const {
    for (const RefPill& pill : m_expandPills) {
        if (pill.shortName == branch) {
            return pill.rect;
        }
    }
    return {};
}

void CommitGraphView::paintHoverExpansion(QPainter& painter) {
    if (m_expandPills.empty()) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    QRect panel = m_expandPills.front().rect;
    for (const RefPill& pill : m_expandPills) {
        panel = panel.united(pill.rect);
    }
    panel.adjust(-3, -3, 3, 3);
    painter.setPen(QPen(theme::border(), 1));
    painter.setBrush(theme::surface());
    painter.drawRoundedRect(panel, 4, 4);

    const QFont font = theme::labelFont();
    for (const RefPill& pill : m_expandPills) {
        paintRefPill(&painter, pill, font);
        if (pill.shortName == m_expandHovered) {
            painter.setPen(QPen(theme::branchColor(pill.colorIndex), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(pill.rect.adjusted(0, 0, -1, -1), 3, 3);
        }
    }
    painter.restore();
}

int CommitGraphView::ghostBadgeRow() const {
    // The pointer on the reveal overlay is asking about the branches under this
    // commit, not which branch contains it: no ghost badge while it is there.
    if (m_expandHovering) {
        return -1;
    }
    if (m_hoverRow >= 0 && hoverLabelAt(m_hoverRow).valid) {
        return m_hoverRow;
    }
    // No pointer on a ghost row: the current row's own badge is still useful, and
    // it is the one a double-click can act on without hunting for the pointer.
    const QModelIndex current = currentIndex();
    if (current.isValid() && hoverLabelAt(current.row()).valid) {
        return current.row();
    }
    return -1;
}

bool CommitGraphView::ghostBadgePill(int row, RefPill* out) const {
    const auto* model = qobject_cast<const CommitModel*>(this->model());
    if (!model || row < 0 || !out) {
        return false;
    }
    const HoverLabel label = hoverLabelAt(row);
    if (!label.valid) {
        return false;
    }
    const QModelIndex index = model->index(row, CommitModel::Refs);
    if (!index.isValid()) {
        return false;
    }
    // Laid out by the same code the delegate uses, over a single label, so the badge
    // lands exactly where a real one would sit — tracking badge included.
    const QRect cell = visualRect(index).adjusted(2, 0, -2, 0);
    const QVariantList trackings{model->refTrackingFlags(label.text)};
    const std::vector<RefPill> pills = RefPillLayout::arrange(
        {label.text}, {label.colorIndex}, {static_cast<int>(label.kind)}, trackings, cell,
        QString(), QString(), model->refIcons());
    if (pills.empty()) {
        return false;
    }
    *out = pills.front();
    return true;
}

QString CommitGraphView::ghostBranchAt(int row) const {
    const HoverLabel label = hoverLabelAt(row);
    return label.valid ? label.text : QString();
}

QString CommitGraphView::highlightBranchAt(int row) const {
    const auto* model = qobject_cast<const CommitModel*>(this->model());
    if (!model || row < 0) {
        return QString();
    }
    const std::vector<RefInfo>& refs = model->refsAt(row);
    if (!refs.empty()) {
        // The head branch wins when it is on the row, matching what a hover over the
        // row's first label resolves to.
        for (const RefInfo& ref : refs) {
            if (QString::fromStdString(ref.shortName) == model->headBranchName()) {
                return QString::fromStdString(ref.shortName);
            }
        }
        return QString::fromStdString(refs.front().shortName);
    }
    const HoverLabel label = hoverLabelAt(row);
    return label.valid ? label.text : QString();
}

void CommitGraphView::activateGhostAt(int row) {
    const HoverLabel label = hoverLabelAt(row);
    if (!label.valid) {
        return;
    }
    // Exactly the signal a double-click on the label emits: the window owns what a
    // checkout means, and a second path to it would be a second set of rules.
    emit branchCheckoutRequested(label.text, label.kind);
}

void CommitGraphView::paintGhostBadge(QPainter& painter) {
    const int row = ghostBadgeRow();
    if (row < 0) {
        return;
    }
    const HoverLabel label = hoverLabelAt(row);
    RefPill pill;
    if (!ghostBadgePill(row, &pill)) {
        return;
    }

    // Half-transparent: the shape and place are the same as a real label, the
    // certainty is not.
    const QColor branch = theme::branchColor(label.colorIndex);
    QColor fill = theme::branchLabelFill(branch);
    QColor border = theme::branchLabelBorder(branch);
    QColor text = theme::branchLabelText(branch);
    fill.setAlpha(120);
    border.setAlpha(150);
    text.setAlpha(170);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QFont font = theme::labelFont();
    painter.setFont(font);

    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(pill.rect, 3, 3);
    QPen outline(border);
    outline.setWidth(1);
    if (label.kind == RefKind::RemoteBranch) {
        outline.setStyle(Qt::DashLine);
    } else if (label.kind == RefKind::Tag) {
        outline.setStyle(Qt::DotLine);
    }
    painter.setPen(outline);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(pill.rect.adjusted(0, 0, -1, -1), 3, 3);
    painter.setPen(text);
    painter.drawText(pill.textRect, Qt::AlignVCenter | Qt::AlignLeft, pill.text);
    painter.restore();
}

void CommitGraphView::selectRowsForDiagnostics(const QVector<int>& rows) {
    if (!selectionModel() || !model()) {
        return;
    }
    QItemSelection selection;
    for (const int row : rows) {
        if (row < 0 || row >= model()->rowCount()) {
            continue;
        }
        const QModelIndex index = model()->index(row, 0);
        selection.select(index, index);
    }
    // The current index goes first, and without touching the selection. QTreeView::
    // setCurrentIndex() works out a selection command for a click that never happened, and
    // for an extended selection that command is ClearAndSelect — so setting it afterwards
    // leaves exactly one row selected, which is what a multi-row check kept reporting.
    if (!rows.isEmpty() && rows.first() >= 0 && rows.first() < model()->rowCount()) {
        selectionModel()->setCurrentIndex(model()->index(rows.first(), 0),
                                          QItemSelectionModel::NoUpdate);
    }
    selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect |
                                             QItemSelectionModel::Rows);
}

QVector<int> CommitGraphView::selectedRows() const {
    QVector<int> rows;
    if (!selectionModel()) {
        return rows;
    }
    // Read from the selected indexes rather than from selectedRows(): the latter asks
    // whether every column of the row is selected, which is a question about the model's
    // drag flags, not about what the reader has highlighted.
    for (const QModelIndex& index : selectionModel()->selectedIndexes()) {
        if (index.isValid() && index.column() == 0) {
            rows.push_back(index.row());
        }
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

std::vector<Oid> CommitGraphView::selectedCommitIds() const {
    std::vector<Oid> ids;
    if (!model()) {
        return ids;
    }
    for (const int row : selectedRows()) {
        // The working-tree row is not a commit: it carries no id any operation can
        // act on, and including it would make every batch command fail on it.
        if (model()->index(row, 0).data(CommitModel::WipRole).toBool()) {
            continue;
        }
        const QString id = model()->index(row, 0).data(CommitModel::CommitIdRole).toString();
        if (!id.isEmpty()) {
            ids.push_back(id.toStdString());
        }
    }
    return ids;
}

void CommitGraphView::paintFlyingBadge(QPainter& painter) {
    const auto* model = qobject_cast<const CommitModel*>(this->model());
    if (!model || m_flightSource.isEmpty()) {
        return;
    }
    const QPoint centre = m_flight.position();
    const QFontMetrics metrics(painter.font());
    const int width = badgeWidthFor(m_flightSource, metrics);
    const int height = metrics.height() + 4;
    const QRect pill(centre.x() - width / 2, centre.y() - height / 2, width, height);

    const int colourIndex = model->refColorIndex(m_flightSource);
    const QColor branch = theme::branchColor(colourIndex);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(theme::branchLabelBorder(branch));
    painter.setBrush(theme::branchLabelFill(branch));
    painter.drawRoundedRect(pill, 3, 3);
    painter.setPen(theme::branchLabelText(branch));
    painter.drawText(pill, Qt::AlignCenter, m_flightSource);
    painter.restore();
}

void CommitGraphView::setAcceptedMimeType(const QString& mimeType) {
    m_acceptedMimeType = mimeType;
}

std::vector<RefPill> CommitGraphView::rowPills(int row) const {
    const auto* model = qobject_cast<const CommitModel*>(this->model());
    if (!model || row < 0 || row >= model->rowCount()) {
        return {};
    }
    if (model->refsAt(row).empty()) {
        return {};
    }
    const QModelIndex index = model->index(row, CommitModel::Refs);
    if (!index.isValid()) {
        return {};
    }
    // The model owns the four parallel lists, and the delegate paints from the same
    // call: the pill that is drawn and the pill that is hit-tested are one
    // computation, whatever the tracking badge does to the width.
    const CommitModel::RefPillData data = model->refPillData(row);
    const QRect cell = visualRect(index).adjusted(2, 0, -2, 0);
    const QString head = model->data(index, CommitModel::RefsHeadRole).toString();
    return RefPillLayout::arrange(data.labels, data.colors, data.kinds, data.trackings, cell, head,
                                  model->focusedBranch(), model->refIcons());
}

const RefInfo* CommitGraphView::refAt(const QPoint& viewportPos, QString* shortNameOut) const {
    const auto* model = qobject_cast<const CommitModel*>(this->model());
    if (!model) {
        return nullptr;
    }

    // A revealed branch from the hover overlay is a label like any other: drag and
    // double-click have to resolve it, and it lives above the row it belongs to.
    if (!m_expandPills.empty()) {
        if (const RefPill* hit = RefPillLayout::hitTest(m_expandPills, viewportPos)) {
            if (!hit->shortName.isEmpty()) {
                if (shortNameOut) {
                    *shortNameOut = hit->shortName;
                }
                for (const RefInfo& ref : model->refsAt(m_expandRow)) {
                    if (QString::fromStdString(ref.shortName) == hit->shortName) {
                        return &ref;
                    }
                }
                return nullptr;
            }
        }
    }

    const QModelIndex index = indexAt(viewportPos);
    if (!index.isValid() || index.column() != CommitModel::Refs) {
        return nullptr;
    }
    const std::vector<RefInfo>& refs = model->refsAt(index.row());
    if (refs.empty()) {
        return nullptr;
    }

    const std::vector<RefPill> pills = rowPills(index.row());
    const RefPill* hit = RefPillLayout::hitTest(pills, viewportPos);
    if (!hit || hit->overflow) {
        return nullptr;
    }
    if (shortNameOut) {
        *shortNameOut = hit->shortName;
    }
    for (const RefInfo& ref : refs) {
        if (QString::fromStdString(ref.shortName) == hit->shortName) {
            return &ref;
        }
    }
    return nullptr;
}

void CommitGraphView::contextMenuEvent(QContextMenuEvent* event) {
    const QModelIndex index = indexAt(event->pos());
    if (!index.isValid()) {
        return;
    }
    const auto* model = qobject_cast<const CommitModel*>(this->model());
    const CommitInfo* commit = model ? model->commitAt(index.row()) : nullptr;
    if (!commit) {
        return;
    }
    // The menu acts on what is selected, so a right-click inside the selection leaves it
    // exactly as it is: setCurrentIndex() here would compute the selection command of a
    // click that carried a selection flag, and for an extended selection that command is
    // ClearAndSelect — it threw away every other commit the reader had picked. Outside the
    // selection the row becomes the only selected one, deliberately: the menu is about the
    // commit under the cursor then, and acting on a selection that points elsewhere is the
    // one outcome worse than losing it. Neither branch goes through setCurrentIndex().
    const bool insideSelection = selectionModel() && selectionModel()->isSelected(index);
    if (selectionModel()) {
        if (insideSelection) {
            selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
        } else {
            QItemSelection only(index, index);
            selectionModel()->select(only, QItemSelectionModel::ClearAndSelect |
                                               QItemSelectionModel::Rows);
            selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
        }
    }
    emit commitContextMenuRequested(QString::fromStdString(commit->id), event->globalPos());
}

void CommitGraphView::openContextMenuAt(int row) {
    if (!model() || row < 0 || row >= model()->rowCount()) {
        return;
    }
    const QRect rect = visualRect(model()->index(row, CommitModel::Message));
    const QPoint pos = rect.center();
    // The same two events a right-click produces, in the same order: the press that the
    // base class would use to change the selection, and then the context menu itself.
    QMouseEvent press(QEvent::MouseButtonPress, pos, viewport()->mapToGlobal(pos),
                      Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QCoreApplication::sendEvent(viewport(), &press);
    QContextMenuEvent menuEvent(QContextMenuEvent::Mouse, pos, viewport()->mapToGlobal(pos));
    contextMenuEvent(&menuEvent);
}

void CommitGraphView::mouseDoubleClickEvent(QMouseEvent* event) {
    const QPoint pos = event->position().toPoint();
    QString name;
    const RefInfo* ref = refAt(pos, &name);
    if (ref && (ref->kind == RefKind::LocalBranch || ref->kind == RefKind::RemoteBranch)) {
        // Only the label does this. A double-click on the row is left to the base
        // class, so the deliberate gesture of opening something never turns into
        // moving the working tree.
        emit branchCheckoutRequested(name, ref->kind);
        return;
    }

    // No real label under the pointer: a ghost badge on an unlabelled row names the
    // nearest branch that contains this commit, and the same double-click checks it
    // out. Bounded to the badge's own rect, so a double-click anywhere else on the
    // row still goes to the base class.
    const int row = rowAtViewportPos(pos);
    if (row >= 0 && row == ghostBadgeRow()) {
        RefPill pill;
        if (ghostBadgePill(row, &pill) && pill.rect.contains(pos)) {
            activateGhostAt(row);
            return;
        }
    }
    QTreeView::mouseDoubleClickEvent(event);
}

QString CommitGraphView::commitIdAtRow(int row) const {
    if (!model() || row < 0 || row >= model()->rowCount()) {
        return QString();
    }
    return model()->index(row, 0).data(CommitModel::CommitIdRole).toString();
}

QString CommitGraphView::navigate(const QString& direction) {
    const int row = currentIndex().row();
    if (row < 0 || !model()) {
        return QString();
    }

    if (direction == QLatin1String("start")) {
        return commitIdAtRow(row);
    }

    if (direction == QLatin1String("tip")) {
        if (model()->rowCount() > 0) {
            setCurrentIndex(model()->index(0, 0));
        }
        return commitIdAtRow(currentIndex().row());
    }

    if (direction == QLatin1String("parent")) {
        // The first parent is the commit the reader followed to get here; the row for it
        // can be anywhere below, so the whole loaded range is searched rather than
        // assuming the next row.
        const auto* commit = static_cast<CommitModel*>(model())->commitAt(row);
        if (!commit || commit->parents.empty()) {
            return QString();
        }
        const QString wanted = QString::fromStdString(commit->parents.front());
        for (int candidate = 0; candidate < model()->rowCount(); ++candidate) {
            if (commitIdAtRow(candidate) == wanted) {
                setCurrentIndex(model()->index(candidate, 0));
                return wanted;
            }
        }
        return QString();
    }

    if (direction == QLatin1String("child")) {
        // The graph is newest first, so a child of this commit lies above it. The nearest
        // one above is the first found walking up; where a commit has several children,
        // this picks the one closest in the drawing, which is the one a reader is looking
        // at.
        const QString wanted = commitIdAtRow(row);
        if (wanted.isEmpty()) {
            return QString();
        }
        for (int candidate = row - 1; candidate >= 0; --candidate) {
            const auto* commit = static_cast<CommitModel*>(model())->commitAt(candidate);
            if (!commit) {
                continue;
            }
            for (const std::string& parent : commit->parents) {
                if (QString::fromStdString(parent) == wanted) {
                    setCurrentIndex(model()->index(candidate, 0));
                    return commitIdAtRow(candidate);
                }
            }
        }
        return QString();
    }

    return QString();
}

void CommitGraphView::keyPressEvent(QKeyEvent* event) {
    QString direction;
    if (event->modifiers() == Qt::AltModifier && event->key() == Qt::Key_Up) {
        direction = QStringLiteral("parent");
    } else if (event->modifiers() == Qt::AltModifier && event->key() == Qt::Key_Down) {
        direction = QStringLiteral("child");
    } else if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_Home) {
        direction = QStringLiteral("tip");
    }
    if (!direction.isEmpty()) {
        if (!navigate(direction).isEmpty()) {
            return;
        }
    }
    QTreeView::keyPressEvent(event);
}

void CommitGraphView::mousePressEvent(QMouseEvent* event) {
    m_pressPos = event->position().toPoint();
    // A press is what a drag starts from, so the label under it is captured here and
    // survives the hover overlay closing as the pointer moves.
    m_pressedRefValid = false;
    // A click anywhere in the graph outside the open list closes it, and that is a
    // dismissal like any other: the badge is waiting to come home.
    if (m_panel->isVisible() && !m_panel->geometry().contains(m_pressPos)) {
        m_panel->dismiss(QStringLiteral("press in the graph"));
        finishFlight();
        return;
    }
    if (event->button() == Qt::RightButton) {
        // A right-click inside the selection opens the menu for what is selected. Qt's
        // default would collapse the selection to the row under the cursor first, and the
        // menu would then be built for one commit while the reader had picked five.
        const QModelIndex pressed = indexAt(m_pressPos);
        if (pressed.isValid() && selectionModel() && selectionModel()->isSelected(pressed)) {
            return;
        }
    }
    if (event->button() == Qt::LeftButton) {
        QString focused;
        if (const RefInfo* ref = refAt(m_pressPos, &focused)) {
            m_pressedRef = *ref;
            m_pressedRefValid = true;
            emit refFocused(focused);
        }
    }
    QTreeView::mousePressEvent(event);
}

void CommitGraphView::mouseReleaseEvent(QMouseEvent* event) {
    m_pressedRefValid = false;
    QTreeView::mouseReleaseEvent(event);
}

void CommitGraphView::startDrag(Qt::DropActions supportedActions) {
    Q_UNUSED(supportedActions);

    QString shortName;
    const RefInfo* ref = nullptr;
    if (m_pressedRefValid) {
        // The press captured the label; the hover overlay may have closed while the
        // pointer moved, but the branch the drag started from is still the one to carry.
        shortName = QString::fromStdString(m_pressedRef.shortName);
        ref = &m_pressedRef;
    } else {
        ref = refAt(m_pressPos, &shortName);
    }
    if (!ref) {
        // Not on a label: no drag. A half-working drag is worse than none.
        return;
    }

    auto* data = RefModel::mimeDataForRef(*ref);
    if (!data) {
        return;
    }

    auto* drag = new QDrag(this);
    drag->setMimeData(data);

    // Qt draws nothing during a drag unless the QDrag carries a pixmap. Without
    // it the user drags an invisible thing with no idea what they picked up.
    const auto* model = qobject_cast<const CommitModel*>(this->model());
    const int colorIndex = model ? model->refColorIndex(shortName) : 0;
    const QPixmap chip = RefsDelegate::dragPixmap(shortName, colorIndex, ref->kind);
    drag->setPixmap(chip);
    const QSizeF logical = chip.deviceIndependentSize();
    drag->setHotSpot(QPoint(static_cast<int>(logical.width() / 2.0),
                            static_cast<int>(logical.height() / 2.0)));

    drag->exec(Qt::MoveAction);
}

void CommitGraphView::dragMoveEvent(QDragMoveEvent* event) {
    if (!m_acceptedMimeType.isEmpty() &&
        !event->mimeData()->hasFormat(m_acceptedMimeType)) {
        event->ignore();
        return;
    }
    const QString source = RefModel::decode(event->mimeData()).shortName;
    if (source.isEmpty()) {
        event->ignore();
        return;
    }

    QString target;
    const RefInfo* ref = refAt(event->position().toPoint(), &target);
    if (ref) {
        if (target == source) {
            event->ignore();
            return;
        }
    } else if (!indexAt(event->position().toPoint()).isValid()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
}

void CommitGraphView::dropEvent(QDropEvent* event) {
    // The menu for this drop opens where the drop is. The cursor position is not a
    // reliable answer while a drag is in flight.
    rememberDropPoint(viewport()->mapToGlobal(event->position().toPoint()));

    const RefModel::DraggedRef source = RefModel::decode(event->mimeData());
    if (source.shortName.isEmpty()) {
        event->ignore();
        return;
    }

    const QPoint pos = event->position().toPoint();

    QString target;
    if (refAt(pos, &target)) {
        if (target == source.shortName) {
            event->ignore();
            return;
        }
        event->acceptProposedAction();
        // The badge stays up, beside the branch it was dropped on, while the list of
        // operations is open. The window is not asked for a menu: this flow owns it.
        QString pinned;
        const RefInfo* landed = refAt(pos, &pinned);
        QRect pillRect;
        if (landed) {
            const QModelIndex row = indexAt(pos);
            if (row.isValid()) {
                // Through rowPills: the badge the delegate draws changes the pill's
                // width, and the badge must park beside the pill that is really there.
                for (const RefPill& pill : rowPills(row.row())) {
                    if (pill.shortName == target) {
                        pillRect = pill.rect;
                    }
                }
            }
        }
        if (!pillRect.isNull()) {
            beginFlight(source, target, pillRect);
            return;
        }
        emit refDroppedOnRef(source, target);
        return;
    }

    // 2. Dropped onto a commit row: the row becomes a branch target.
    const QModelIndex index = indexAt(pos);
    if (!index.isValid()) {
        event->ignore();
        return;
    }
    const auto* model = qobject_cast<const CommitModel*>(this->model());
    const CommitInfo* commit = model ? model->commitAt(index.row()) : nullptr;
    if (!commit) {
        event->ignore();
        return;
    }

    event->acceptProposedAction();
    emit refDroppedOnCommit(source, QString::fromStdString(commit->id));
}

} // namespace gk
