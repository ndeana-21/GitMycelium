#include "ui/refs_delegate.h"

#include "model/commit_model.h"
#include "theme/glyphs.h"
#include "theme/theme.h"
#include "ui/ref_pill_layout.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>

namespace gk {
namespace {

/// The row's first cell carries the accent bar that marks the current commit.
void paintSelectedRowStart(QPainter* painter, const QRect& rect, const QBrush& fill) {
    painter->fillRect(rect, fill);
    painter->fillRect(QRect(rect.left(), rect.top(), theme::scaledPixels(2), rect.height()),
                      theme::accent());
}


/// A monitor for a local branch, a cloud for a remote. The glyphs come from
/// theme/glyphs so the graph and the branch tree cannot drift apart.
void drawKindIcon(QPainter* painter, const QRectF& box, RefKind kind, const QColor& color) {
    const Glyph glyph = kind == RefKind::RemoteBranch ? Glyph::Cloud : Glyph::Monitor;
    paintGlyph(painter, glyph, box, color);
}

/// Stroked, not filled: a solid tick closes its own corners and reads as a blob.
void drawHeadCheck(QPainter* painter, const QRectF& box, const QColor& color) {
    QPen pen(color);
    pen.setWidthF(1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);

    painter->save();
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    QPainterPath tick;
    tick.moveTo(box.left(), box.top() + box.height() * 0.55);
    tick.lineTo(box.left() + box.width() * 0.36, box.bottom());
    tick.lineTo(box.right(), box.top() + box.height() * 0.06);
    painter->drawPath(tick);
    painter->restore();
}

constexpr qreal kDimmedRowOpacity = 0.32;

} // namespace

void paintRefPill(QPainter* painter, const RefPill& pill, const QFont& font) {
    painter->save();
    painter->setFont(font);

    const QColor branch = theme::branchColor(pill.colorIndex);
    painter->setPen(Qt::NoPen);
    painter->setBrush(theme::branchLabelFill(branch));
    painter->drawRoundedRect(pill.rect, 3, 3);

    QPen border(theme::branchLabelBorder(branch));
    border.setWidth(1);
    // Shape carries the kind as well as colour: a remote-tracking branch is
    // dashed, a tag dotted. Colour alone would be ambiguous for the colour
    // blind and when the palette wraps.
    switch (pill.kind) {
    case RefKind::RemoteBranch: border.setStyle(Qt::DashLine); break;
    case RefKind::Tag: border.setStyle(Qt::DotLine); break;
    default: break;
    }
    painter->setPen(border);
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(pill.rect.adjusted(0, 0, -1, -1), 3, 3);

    const QColor text = theme::branchLabelText(branch);
    painter->setPen(text);
    painter->drawText(pill.textRect, Qt::AlignVCenter | Qt::AlignLeft, pill.text);
    if (!pill.trackingText.isEmpty() && pill.badgeRect.isValid()) {
        QColor badge = text;
        badge.setAlpha(200);
        painter->setPen(badge);
        painter->drawText(pill.badgeRect, Qt::AlignVCenter | Qt::AlignHCenter, pill.trackingText);
    }
    if (pill.headRect.isValid()) {
        drawHeadCheck(painter, pill.headRect, text);
    }
    if (pill.iconRect.isValid()) {
        drawKindIcon(painter, pill.iconRect, pill.kind, text);
    }
    if (pill.remoteRect.isValid()) {
        paintGlyph(painter, Glyph::Cloud, pill.remoteRect, text);
    }
    painter->restore();
}

RefsDelegate::RefsDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void RefsDelegate::setLabelAlphaHook(std::function<double(const QString& name)> alpha) {
    m_labelAlpha = std::move(alpha);
}

void RefsDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                         const QModelIndex& index) const {
    const auto* commits = qobject_cast<const CommitModel*>(index.model());
    if (!commits) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    // The four lists come from the model, not from the roles: the view hit-tests a
    // pill it lays out from the same call, so the painted pill and the grabbed pill
    // are the same rectangle by construction.
    const CommitModel::RefPillData data = commits->refPillData(index.row());
    if (option.state & QStyle::State_Selected) {
        paintSelectedRowStart(painter, option.rect, option.palette.highlight());
    }
    if (data.labels.isEmpty()) {
        return;
    }
    const QString headBranch = commits->data(index, CommitModel::RefsHeadRole).toString();

    painter->save();
    if (commits->isDimmedRow(index.row())) {
        painter->setOpacity(kDimmedRowOpacity);
    }

    const QRect cell = option.rect.adjusted(2, 0, -2, 0);
    const std::vector<RefPill> pills =
        RefPillLayout::arrange(data.labels, data.colors, data.kinds, data.trackings, cell,
                               headBranch, commits->focusedBranch(), commits->refIcons());

    const QFont font = theme::labelFont();
    painter->setFont(font);

    for (const RefPill& pill : pills) {
        if (pill.overflow) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme::surfaceAlt());
            painter->drawRoundedRect(pill.rect, 3, 3);
            painter->setPen(theme::textDim());
            painter->drawText(pill.rect, Qt::AlignCenter, pill.text);
            continue;
        }

        painter->save();
        if (m_labelAlpha) {
            const double alpha = m_labelAlpha(pill.text);
            if (alpha < 1.0) {
                // Multiplied into the row's own opacity: a label appearing on a row
                // that is already dimmed must not become fully opaque.
                painter->setOpacity(painter->opacity() * alpha);
            }
        }
        paintRefPill(painter, pill, font);
        painter->restore();
    }

    painter->restore();
}

QSize RefsDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    Q_UNUSED(option);
    Q_UNUSED(index);
    return QSize(0, 30);
}

QPixmap RefsDelegate::dragPixmap(const QString& name, int colorIndex, RefKind kind) {
    const QColor branch = theme::branchColor(colorIndex);
    const QFont font = theme::labelFont();
    const QFontMetrics metrics(font);

    const int pad = 8;
    const QSize size(metrics.horizontalAdvance(name) + 2 * pad, metrics.height() + 6);

    QPixmap pixmap(size * 2); // drawn at 2x so it stays sharp while moving
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(font);

    const QRectF box(0.5, 0.5, size.width() - 1.0, size.height() - 1.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::branchLabelFill(branch));
    painter.drawRoundedRect(box, 4, 4);

    QPen border(theme::branchLabelBorder(branch));
    if (kind == RefKind::RemoteBranch) {
        border.setStyle(Qt::DashLine);
    } else if (kind == RefKind::Tag) {
        border.setStyle(Qt::DotLine);
    }
    painter.setPen(border);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(box, 4, 4);

    painter.setPen(theme::branchLabelText(branch));
    painter.drawText(box, Qt::AlignCenter, name);
    return pixmap;
}

} // namespace gk
