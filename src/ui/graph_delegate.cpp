#include "ui/graph_delegate.h"

#include "model/commit_model.h"
#include "theme/theme.h"

#include <QApplication>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>

#include <algorithm>

namespace gk {
namespace {

constexpr qreal kLineWidth = 2.4;

/// Dark enough to push the lanes behind it out of focus, light enough to still see
/// they are there.
constexpr int kStripAlpha = 150;

/// Opacity for a node whose lane has gone behind that strip: dimmed, not hidden.
constexpr qreal kCoveredNodeOpacity = 0.62;

/// Faint enough to recede, solid enough that the history stays legible.
constexpr qreal kDimmedRowOpacity = 0.32;

constexpr qreal kMergeNodeScale = 0.5;

/// An L-shaped connector with one rounded corner: straight along the first leg, a
/// quarter turn at `corner`, straight along the second. Not an S-curve: the line leaves a commit sideways and joins its
/// lane through one turn, so branches read as a trunk with taps.
QPainterPath elbowConnector(const QPointF& from, const QPointF& corner, const QPointF& to,
                            qreal radius) {
    // 4/3 * tan(pi/8): the classic approximation of a quarter circle by a cubic.
    constexpr qreal kArcConstant = 0.5522847498307936;

    const auto unit = [](const QPointF& v) {
        const qreal length = std::hypot(v.x(), v.y());
        return length < 1e-9 ? QPointF(0.0, 0.0) : QPointF(v.x() / length, v.y() / length);
    };
    const auto span = [](const QPointF& a, const QPointF& b) {
        return std::hypot(b.x() - a.x(), b.y() - a.y());
    };

    const QPointF incoming = unit(corner - from);
    const QPointF outgoing = unit(to - corner);

    QPainterPath path;
    path.moveTo(from);
    if (incoming.isNull() || outgoing.isNull()) {
        path.lineTo(to);
        return path;
    }

    // A radius longer than either leg would push the arc past the ends.
    const qreal r = std::min({radius, span(from, corner), span(corner, to)});
    const qreal k = r * kArcConstant;

    const QPointF arcStart = corner - incoming * r;
    const QPointF arcEnd = corner + outgoing * r;

    path.lineTo(arcStart);
    path.cubicTo(arcStart + incoming * k, arcEnd - outgoing * k, arcEnd);
    path.lineTo(to);
    return path;
}

/// Roughly half a lane, so a turn reads as a turn without swallowing the lane.
qreal connectorRadius() {
    return GraphDelegate::laneWidth() * 0.45;
}

} // namespace

GraphDelegate::GraphDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void GraphDelegate::setAnimationHooks(std::function<double(const QString& oid)> fadeAlpha,
                                      std::function<bool(const QString& oid)> isMorphing) {
    m_fadeAlpha = std::move(fadeAlpha);
    m_isMorphing = std::move(isMorphing);
}

int GraphDelegate::widthForLanes(int lanes) {
    if (lanes < 1) {
        lanes = 1;
    }
    // The first lane sits lanePadding() in from the left edge and the last one
    // lanePadding() in from the right, so `lanes` lanes need one gap fewer than
    // there are lanes.
    return lanePadding() * 2 + (lanes - 1) * laneWidth();
}

void GraphDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const {
    if (index.column() != CommitModel::Graph) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();
    const QWidget* widget = option.widget;
    QStyle* style = widget ? widget->style() : QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);

    const auto* model = qobject_cast<const CommitModel*>(index.model());
    if (!model) {
        return;
    }
    const GraphNode* node = model->graphNodeAt(index.row());
    if (!node) {
        return;
    }

    const QString oid = index.data(CommitModel::CommitIdRole).toString();
    if (m_isMorphing && m_isMorphing(oid)) {
        // The view's overlay is gliding this row's old snapshot into place. Drawing
        // the new graph underneath it as well would show two graphs for one commit.
        return;
    }

    const QRect rect = option.rect;
    const qreal centerY = rect.center().y();
    const qreal bottom = rect.bottom() + 1.0;

    // The graph is never rescaled: narrowing the column does not squeeze the lanes or
    // shrink the nodes, it covers the surplus with a translucent strip. That is the
    // difference between a graph that goes behind something and one that stains the
    // commit messages.
    const int laneCount = std::max(1, model->laneCount());
    const qreal inset = lanePadding();
    const qreal pitch = laneWidth();
    const qreal radius = nodeRadius();
    const qreal lineWidth = kLineWidth;
    const qreal corner = connectorRadius();

    // Lane positions are absolute within the cell, never derived from its width: that
    // keeps lane 0 nailed to its pixel while the column is dragged.
    const auto xOf = [&](int lane) { return rect.left() + inset + pitch * lane; };

    // Less than two lanes of room is the collapsed state: every node joins lane 0 at
    // the left edge, at full strength, so the column reads as a line of commit circles.
    // Lane 0 must not move, so the others come to it and not the other way round.
    const bool collapsed = rect.width() < inset + 2.0 * pitch;
    const qreal nodeColumnX = xOf(0);

    // The strip appears as soon as the lanes need more room than the cell has, and
    // stops growing at roughly one node across: wide enough to read as "the rest is
    // behind here" without eating what is visible.
    const qreal surplus = 2.0 * inset + laneCount * pitch - rect.width();
    const qreal stripWidth = std::min(surplus, radius * 2.0 + 3.0);
    const qreal stripLeft = rect.right() + 1.0 - std::max<qreal>(0.0, stripWidth);

    // The strip fades out as the column approaches the collapsed width, where it has
    // nothing left to point at and a dark band would just look like dirt.
    const qreal fadeSpan = 2.0 * pitch;
    const qreal stripFade =
        std::clamp((rect.width() - (inset + 2.0 * pitch)) / fadeSpan, 0.0, 1.0);
    const int stripAlpha = static_cast<int>(kStripAlpha * stripFade);

    // Outside the collapsed state a node may not cross the column's right edge: a lane
    // that no longer fits lines its commit up against it instead of over the messages.
    const auto nodeX = [&](int lane) {
        if (collapsed) {
            return nodeColumnX;
        }
        return std::min(xOf(lane), static_cast<qreal>(rect.right()) - radius);
    };

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    // Never paint outside the cell: however narrow the column gets, the graph cannot
    // draw over the commit messages next to it.
    painter->setClipRect(rect, Qt::IntersectClip);

    qreal opacity = 1.0;
    if (model->isDimmedRow(index.row())) {
        opacity *= kDimmedRowOpacity;
    }
    if (m_fadeAlpha) {
        opacity *= m_fadeAlpha(oid);
    }
    if (opacity < 1.0) {
        painter->setOpacity(opacity);
    }

    // One role per lane, and the drawing follows directly from the role: a pass-through
    // line is a vertical, a starting line a curve out of the node, an ending line a curve
    // into it. That is what keeps a born lane from being drawn as both a curve and a
    // vertical, and a meeting from being drawn on the child row.
    const std::vector<GraphRowEdge> rowLanes = rowEdgesOf(*node);

    // 1. Lines that carry through this row. Drawing the whole row height is what makes
    //    consecutive rows join into one continuous line instead of a dashed one.
    for (const GraphRowEdge& edge : rowLanes) {
        if (collapsed && edge.lane != 0) {
            // Collapsed, only the trunk is left; the other lanes' rails would be lines
            // without commits beside the line of circles.
            continue;
        }
        qreal top = rect.top();
        if (edge.role == GraphEdgeRole::PassThrough) {
            top = rect.top();
        } else if (edge.role == GraphEdgeRole::Starting && edge.lane == node->lane) {
            // The commit's own first-parent line has nothing above it, so it begins at
            // the node rather than at the top of the cell.
            top = centerY;
        } else {
            continue;
        }
        const qreal x = xOf(edge.lane);
        if (x - lineWidth > rect.right()) {
            // Past the edge. With thousands of live branches these are nearly all the
            // lines in the row, and the clip would discard them only after stroking.
            continue;
        }
        painter->setPen(QPen(theme::branchColor(edge.colorIndex), lineWidth, Qt::SolidLine,
                             Qt::RoundCap));
        painter->drawLine(QPointF(x, top), QPointF(x, bottom));
    }

    // 2. Curves where an edge changes lane within this row: a merge's extra parent line
    //    is born here and heads down to its own lane.
    for (const GraphRowEdge& edge : rowLanes) {
        if (edge.role != GraphEdgeRole::Starting || edge.lane == node->lane || collapsed) {
            continue;
        }
        const qreal x1 = xOf(node->lane);
        const qreal x2 = xOf(edge.lane);
        const QPainterPath path = elbowConnector(
            QPointF(x1, centerY), QPointF(x2, centerY), QPointF(x2, bottom), corner);
        painter->setPen(QPen(theme::branchColor(edge.colorIndex), lineWidth, Qt::SolidLine,
                             Qt::RoundCap));
        painter->drawPath(path);
    }

    // 3. Lines arriving from above that end at this node — where branches meet. Drawn
    //    from the top of the row in the arriving lane, bending into the node.
    for (const GraphRowEdge& edge : rowLanes) {
        if (edge.role != GraphEdgeRole::Ending || collapsed) {
            continue;
        }
        const qreal x1 = xOf(edge.lane);
        const qreal x2 = xOf(node->lane);
        if (qFuzzyCompare(x1, x2)) {
            continue;
        }
        const QPainterPath path = elbowConnector(QPointF(x1, rect.top()),
                                                 QPointF(x1, centerY), QPointF(x2, centerY),
                                                 corner);
        painter->setPen(QPen(theme::branchColor(edge.colorIndex), lineWidth, Qt::SolidLine,
                             Qt::RoundCap));
        painter->drawPath(path);
    }

    // 4. The backing strip, over the lines only: a lane that no longer fits fades into
    //    it rather than being cut off in mid-air, and the nodes are drawn after it so no
    //    commit is hidden by its own strip.
    if (stripWidth > 0.5 && stripAlpha > 0) {
        QColor veil = theme::background();
        veil.setAlpha(stripAlpha);
        painter->fillRect(QRectF(stripLeft, rect.top(), rect.right() + 1.0 - stripLeft,
                                 rect.height()),
                          veil);
    }

    // 5. The commit node: a filled circle carrying the author's initials.
    const QColor color = theme::branchColor(node->colorIndex);
    const QPointF center(nodeX(node->lane), centerY);

    // A node behind the strip is darkened, except in the collapsed state, where the node
    // column is all that is left and dimming it would look broken. A node the column had
    // to pull out of its own lane is *not* faded however far it ends up: a commit without
    // a circle looks like it does not exist.
    if (!collapsed && xOf(node->lane) >= stripLeft) {
        painter->setOpacity(kCoveredNodeOpacity);
    }

    // Each node type gets its own shape so the row reads as the state it is.
    GraphNodeType type = node->type;
    if (type == GraphNodeType::Commit && model->data(index, CommitModel::WipRole).toBool()) {
        // A caller that paints without the model having run still gets a ring, not a disc.
        type = GraphNodeType::WorkDir;
    }

    const auto ringNode = [&](const QColor& pen) {
        painter->setPen(QPen(pen, kLineWidth, Qt::SolidLine, Qt::RoundCap));
        painter->setBrush(theme::background());
        painter->drawEllipse(center, radius, radius);
    };

    switch (type) {
    case GraphNodeType::WorkDir: {
        // The working tree is a hollow ring rather than a disc, and it wears the
        // colour of the branch it sits on: an accent-coloured ring on a blue line
        // reads as a second branch with its tip on this one, which is exactly what
        // uncommitted changes are not.
        ringNode(color);
        painter->restore();
        return;
    }
    case GraphNodeType::MergeConflict: {
        // A warning ring with a solid core, so it cannot be read as the working-tree ring.
        ringNode(theme::danger());
        painter->setPen(Qt::NoPen);
        painter->setBrush(theme::danger());
        painter->drawEllipse(center, radius * 0.34, radius * 0.34);
        painter->restore();
        return;
    }
    case GraphNodeType::UnsupportedRebase: {
        // A ring with a pause bar in it: "this wants you to continue or abort it".
        ringNode(theme::warning());
        painter->setPen(Qt::NoPen);
        painter->setBrush(theme::warning());
        const qreal barW = radius * 0.22;
        const qreal barH = radius * 0.9;
        painter->drawRect(QRectF(center.x() - radius * 0.34, center.y() - barH / 2, barW, barH));
        painter->drawRect(QRectF(center.x() + radius * 0.12, center.y() - barH / 2, barW, barH));
        painter->restore();
        return;
    }
    case GraphNodeType::Stash: {
        // A rounded square in the modified colour, as the stash section in the ref tree is.
        const qreal side = radius * 1.7;
        const QRectF box(center.x() - side / 2, center.y() - side / 2, side, side);
        painter->setPen(QPen(theme::modified().darker(150), 1.0));
        painter->setBrush(theme::modified());
        painter->drawRoundedRect(box, 2.5, 2.5);
        painter->restore();
        return;
    }
    case GraphNodeType::Merge: {
        // A junction, not a commit you look up by its author, so it carries no initials:
        // half size is the whole signal, and it keeps the lines that meet here readable.
        const qreal mergeRadius = radius * kMergeNodeScale;
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawEllipse(center, mergeRadius, mergeRadius);
        painter->restore();
        return;
    }
    case GraphNodeType::Timeline: {
        // Not a node: its lanes were already drawn as pass-through verticals in step 1,
        // so the graph carries on through it and a faint rule marks the boundary. The
        // label itself belongs to the message column.
        painter->setPen(QPen(theme::border(), 1.0));
        painter->drawLine(QPointF(rect.left(), centerY), QPointF(rect.right(), centerY));
        painter->restore();
        return;
    }
    case GraphNodeType::Commit:
        break;
    }

    // There are no avatar pictures, so the disc is the author's own colour and the
    // ring around it is the branch's: who wrote it and which line it is on, at once.
    const QColor authorFill =
        theme::authorColor(model->data(index, CommitModel::AuthorIdentityRole).toString());
    const qreal ring = std::max<qreal>(2.0, radius * 0.22);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawEllipse(center, radius, radius);
    painter->setBrush(authorFill);
    painter->drawEllipse(center, radius - ring, radius - ring);

    const QString initials = model->data(index, Qt::UserRole).toString();
    if (!initials.isEmpty()) {
        QFont font = option.font;
        font.setPointSizeF(std::max(6.0, option.font.pointSizeF() - 2.5));
        font.setBold(true);
        painter->setFont(font);
        painter->setPen(theme::textOn(authorFill));
        painter->drawText(QRectF(center.x() - radius, center.y() - radius, radius * 2, radius * 2),
                          Qt::AlignCenter, initials);
    }

    painter->restore();
}

} // namespace gk
