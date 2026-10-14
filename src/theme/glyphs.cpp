#include "theme/glyphs.h"

#include "theme/theme.h"

#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTransform>

#include <algorithm>

namespace gk {
namespace {

/// Material Design's cloud glyph, in its own 24x24 coordinates. Taken from the
/// icon set rather than invented here: a hand-built cloud outline at eleven
/// pixels closes its own corners and comes out looking like a mountain.
QPainterPath cloudGlyph() {
    QPainterPath p;
    p.moveTo(19.35, 10.04);
    p.cubicTo(18.67, 6.59, 15.64, 4.0, 12.0, 4.0);
    p.cubicTo(9.11, 4.0, 6.60, 5.64, 5.35, 8.04);
    p.cubicTo(2.34, 8.36, 0.0, 10.91, 0.0, 14.0);
    p.cubicTo(0.0, 17.31, 2.69, 20.0, 6.0, 20.0);
    p.lineTo(19.0, 20.0);
    p.cubicTo(21.76, 20.0, 24.0, 17.76, 24.0, 15.0);
    p.cubicTo(24.0, 12.36, 21.95, 10.22, 19.35, 10.04);
    p.closeSubpath();
    return p;
}

QPainterPath fittedIn(const QPainterPath& source, const QRectF& box) {
    const QRectF bounds = source.boundingRect();
    QTransform transform;
    transform.translate(box.center().x(), box.center().y());
    const qreal scale = std::min(box.width() / bounds.width(), box.height() / bounds.height());
    transform.scale(scale, scale);
    transform.translate(-bounds.center().x(), -bounds.center().y());
    return transform.map(source);
}

void drawMonitor(QPainter* painter, const QRectF& box, const QColor& colour) {
    QPen pen(colour);
    pen.setWidthF(std::max(1.0, box.height() * 0.11));
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    const qreal w = box.width();
    const qreal h = box.height();
    const QRectF screen(box.left() + w * 0.04, box.top() + h * 0.06, w * 0.92, h * 0.60);
    painter->drawRoundedRect(screen, 1.5, 1.5);
    const qreal midX = box.center().x();
    painter->drawLine(QPointF(midX, screen.bottom()), QPointF(midX, box.bottom() - h * 0.10));
    painter->drawLine(QPointF(midX - w * 0.26, box.bottom() - h * 0.10),
                      QPointF(midX + w * 0.26, box.bottom() - h * 0.10));
}

void drawCloud(QPainter* painter, const QRectF& box, const QColor& colour) {
    // Outlined, not filled: chosen from a sheet of nine candidates rendered at the
    // real eleven pixels, where every filled variant collapsed into a lump.
    QPen pen(colour);
    pen.setWidthF(std::max(1.0, box.height() * 0.11));
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(fittedIn(cloudGlyph(),
                               box.adjusted(box.width() * 0.03, box.height() * 0.12,
                                            -box.width() * 0.03, -box.height() * 0.12)));
}

void drawFolder(QPainter* painter, const QRectF& box, const QColor& colour) {
    // Filled, unlike the other glyphs: a folder outline at fourteen pixels is a
    // rectangle with a nick in it, and the nick is the only thing that says
    // "folder". Solid, the silhouette reads at a glance.
    const qreal w = box.width();
    const qreal h = box.height();
    const qreal top = box.top() + h * 0.22;
    const qreal bodyH = h * 0.54;

    QPainterPath path;
    path.addRoundedRect(
        QRectF(box.left() + w * 0.05, top, w * 0.90, bodyH), h * 0.13, h * 0.13);
    path.addRoundedRect(
        QRectF(box.left() + w * 0.05, top - h * 0.14, w * 0.46, h * 0.30), h * 0.10, h * 0.10);

    painter->setPen(Qt::NoPen);
    painter->setBrush(colour);
    painter->drawPath(path.simplified());
}

void drawBranch(QPainter* painter, const QRectF& box, const QColor& colour) {
    QPen pen(colour);
    pen.setWidthF(std::max(1.0, box.height() * 0.10));
    pen.setCapStyle(Qt::RoundCap);
    painter->setPen(pen);

    const qreal w = box.width();
    const qreal h = box.height();
    const QPointF top(box.left() + w * 0.32, box.top() + h * 0.22);
    const QPointF bottom(box.left() + w * 0.32, box.top() + h * 0.80);
    const QPointF side(box.left() + w * 0.72, box.top() + h * 0.40);

    painter->drawLine(top, bottom);
    QPainterPath curve;
    curve.moveTo(bottom);
    curve.quadTo(QPointF(bottom.x(), side.y()), side);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(curve);

    painter->setPen(Qt::NoPen);
    painter->setBrush(colour);
    const qreal r = std::max(1.4, w * 0.13);
    painter->drawEllipse(top, r, r);
    painter->drawEllipse(bottom, r, r);
    painter->drawEllipse(side, r, r);
}

void drawTag(QPainter* painter, const QRectF& box, const QColor& colour) {
    const qreal w = box.width();
    const qreal h = box.height();

    QPainterPath tag;
    tag.moveTo(box.left() + w * 0.10, box.top() + h * 0.20);
    tag.lineTo(box.left() + w * 0.58, box.top() + h * 0.20);
    tag.lineTo(box.left() + w * 0.90, box.top() + h * 0.50);
    tag.lineTo(box.left() + w * 0.58, box.top() + h * 0.80);
    tag.lineTo(box.left() + w * 0.10, box.top() + h * 0.80);
    tag.closeSubpath();

    painter->setPen(Qt::NoPen);
    painter->setBrush(colour);
    painter->drawPath(tag);

    painter->setBrush(gk::theme::background());
    painter->drawEllipse(QPointF(box.left() + w * 0.28, box.top() + h * 0.50),
                         std::max(1.0, w * 0.07), std::max(1.0, w * 0.07));
}

void drawStash(QPainter* painter, const QRectF& box, const QColor& colour) {
    QPen pen(colour);
    pen.setWidthF(std::max(1.0, box.height() * 0.10));
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    const qreal w = box.width();
    const qreal h = box.height();
    const QRectF lid(box.left() + w * 0.08, box.top() + h * 0.14, w * 0.84, h * 0.22);
    painter->drawRoundedRect(lid, h * 0.06, h * 0.06);
    painter->drawLine(QPointF(box.left() + w * 0.16, lid.bottom()),
                      QPointF(box.left() + w * 0.16, box.bottom() - h * 0.06));
    painter->drawLine(QPointF(box.right() - w * 0.16, lid.bottom()),
                      QPointF(box.right() - w * 0.16, box.bottom() - h * 0.06));
    painter->drawLine(QPointF(box.left() + w * 0.16, box.bottom() - h * 0.06),
                      QPointF(box.right() - w * 0.16, box.bottom() - h * 0.06));
    painter->drawLine(QPointF(box.center().x() - w * 0.14, box.top() + h * 0.52),
                      QPointF(box.center().x() + w * 0.14, box.top() + h * 0.52));
}

void drawWorktree(QPainter* painter, const QRectF& box, const QColor& colour) {
    QPen pen(colour);
    pen.setWidthF(std::max(1.0, box.height() * 0.11));
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    const qreal w = box.width();
    const qreal h = box.height();
    painter->drawRoundedRect(
        QRectF(box.left() + w * 0.10, box.top() + h * 0.12, w * 0.60, h * 0.50), 1.5, 1.5);
    painter->drawRoundedRect(
        QRectF(box.left() + w * 0.30, box.top() + h * 0.38, w * 0.60, h * 0.50), 1.5, 1.5);
}

/// A box holding a smaller box: a submodule is a repository inside another. Kept
/// distinct from the worktree glyph — two checkouts of one repository and one
/// repository nested in another are different things and must not share a shape.
void drawSubmodule(QPainter* painter, const QRectF& box, const QColor& colour) {
    QPen pen(colour);
    pen.setWidthF(std::max(1.0, box.height() * 0.11));
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    const qreal w = box.width();
    const qreal h = box.height();
    painter->drawRoundedRect(
        QRectF(box.left() + w * 0.08, box.top() + h * 0.08, w * 0.84, h * 0.84), 2.0, 2.0);
    painter->drawRoundedRect(
        QRectF(box.left() + w * 0.40, box.top() + h * 0.40, w * 0.36, h * 0.36), 1.0, 1.0);
}

void drawPaperclip(QPainter* painter, const QRectF& box, const QColor& colour) {
    QPen pen(colour);
    pen.setWidthF(std::max(1.0, box.height() * 0.10));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    const qreal w = box.width();
    const qreal h = box.height();
    const qreal left = box.left() + w * 0.30;
    const qreal right = box.left() + w * 0.66;
    const qreal top = box.top() + h * 0.22;
    const qreal bottom = box.top() + h * 0.72;
    const qreal r = (right - left) / 2.0;

    QPainterPath clip;
    clip.moveTo(right, top);
    clip.lineTo(right, bottom - r);
    clip.arcTo(QRectF(left, bottom - 2.0 * r, 2.0 * r, 2.0 * r), 0.0, -180.0);
    clip.lineTo(left, top - r);
    clip.arcTo(QRectF(left, top - 2.0 * r, 2.0 * r, 2.0 * r), 180.0, -180.0);
    clip.lineTo(right, box.top() + h * 0.56);
    painter->drawPath(clip);
}

void drawPlus(QPainter* painter, const QRectF& box, const QColor& colour) {
    QPen pen(colour);
    pen.setWidthF(std::max(1.4, box.height() * 0.20));
    pen.setCapStyle(Qt::RoundCap);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    const qreal inset = box.height() * 0.20;
    painter->drawLine(QPointF(box.center().x(), box.top() + inset),
                      QPointF(box.center().x(), box.bottom() - inset));
    painter->drawLine(QPointF(box.left() + inset, box.center().y()),
                      QPointF(box.right() - inset, box.center().y()));
}

void drawMinus(QPainter* painter, const QRectF& box, const QColor& colour) {
    QPen pen(colour);
    pen.setWidthF(std::max(1.4, box.height() * 0.20));
    pen.setCapStyle(Qt::RoundCap);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    const qreal inset = box.height() * 0.20;
    painter->drawLine(QPointF(box.left() + inset, box.center().y()),
                      QPointF(box.right() - inset, box.center().y()));
}

void drawPencil(QPainter* painter, const QRectF& box, const QColor& colour) {
    painter->save();
    // Drawn upright inside a rotated frame: a pencil is a rectangle with a
    // triangular tip, and both are easy to place in axis-aligned coordinates.
    // Placing the same shape by hand on the diagonal is where it goes crooked.
    painter->translate(box.center());
    painter->rotate(-45.0);

    const qreal s = std::min(box.width(), box.height());
    const qreal halfWidth = s * 0.15;
    const qreal top = -s * 0.44;
    const qreal shoulders = s * 0.16;
    const qreal tip = s * 0.46;

    painter->setPen(Qt::NoPen);
    painter->setBrush(colour);
    painter->drawRect(QRectF(-halfWidth, top, 2.0 * halfWidth, shoulders - top));

    QPainterPath point;
    point.moveTo(-halfWidth, shoulders);
    point.lineTo(halfWidth, shoulders);
    point.lineTo(0.0, tip);
    point.closeSubpath();
    painter->drawPath(point);
    painter->restore();
}

void drawArrow(QPainter* painter, const QRectF& box, const QColor& colour) {
    QPen pen(colour);
    pen.setWidthF(std::max(1.2, box.height() * 0.14));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    const qreal y = box.center().y();
    painter->drawLine(QPointF(box.left() + box.width() * 0.08, y),
                      QPointF(box.right() - box.width() * 0.10, y));
    QPainterPath head;
    head.moveTo(box.right() - box.width() * 0.42, y - box.height() * 0.26);
    head.lineTo(box.right() - box.width() * 0.08, y);
    head.lineTo(box.right() - box.width() * 0.42, y + box.height() * 0.26);
    painter->drawPath(head);
}

} // namespace

void paintGlyph(QPainter* painter, Glyph glyph, const QRectF& box, const QColor& colour) {
    painter->save();
    switch (glyph) {
    case Glyph::Monitor: drawMonitor(painter, box, colour); break;
    case Glyph::Cloud: drawCloud(painter, box, colour); break;
    case Glyph::Folder: drawFolder(painter, box, colour); break;
    case Glyph::Branch: drawBranch(painter, box, colour); break;
    case Glyph::Tag: drawTag(painter, box, colour); break;
    case Glyph::Stash: drawStash(painter, box, colour); break;
    case Glyph::Paperclip: drawPaperclip(painter, box, colour); break;
    case Glyph::Worktree: drawWorktree(painter, box, colour); break;
    case Glyph::Submodule: drawSubmodule(painter, box, colour); break;
    case Glyph::Plus: drawPlus(painter, box, colour); break;
    case Glyph::Pencil: drawPencil(painter, box, colour); break;
    case Glyph::Minus: drawMinus(painter, box, colour); break;
    case Glyph::Arrow: drawArrow(painter, box, colour); break;
    }
    painter->restore();
}

QIcon glyphIcon(Glyph glyph, const QColor& colour, int side) {
    // The branch tree asks for the same few icons once per row, and a repository
    // with thousands of branches painted thousands of identical pixmaps on every
    // refresh. The colour is part of the key, so a theme change cannot hit a stale
    // entry; QIcon is shared, so a hit is a reference, not a copy.
    static QHash<quint64, QIcon> cache;
    const quint64 key = (static_cast<quint64>(static_cast<quint8>(glyph)) << 56) |
                        (static_cast<quint64>(static_cast<quint16>(side)) << 40) |
                        static_cast<quint64>(colour.rgba());
    if (const auto it = cache.constFind(key); it != cache.constEnd()) {
        return *it;
    }
    constexpr qreal kScale = 2.0;
    QPixmap pixmap(QSize(static_cast<int>(side * kScale), static_cast<int>(side * kScale)));
    pixmap.setDevicePixelRatio(kScale);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintGlyph(&painter, glyph, QRectF(0.5, 0.5, side - 1.0, side - 1.0), colour);
    painter.end();

    const QIcon icon(pixmap);
    cache.insert(key, icon);
    return icon;
}

} // namespace gk
