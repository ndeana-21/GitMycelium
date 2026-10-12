#include "theme/icons.h"

#include "theme/theme.h"

#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace gk {
namespace {

QString resourceFor(Icon which) {
    switch (which) {
    case Icon::Undo: return QStringLiteral(":/icons/undo-2.svg");
    case Icon::Redo: return QStringLiteral(":/icons/redo-2.svg");
    case Icon::Fetch: return QStringLiteral(":/icons/refresh-cw.svg");
    case Icon::Pull: return QStringLiteral(":/icons/arrow-down-to-line.svg");
    case Icon::Push: return QStringLiteral(":/icons/arrow-up-from-line.svg");
    case Icon::Branch: return QStringLiteral(":/icons/git-branch.svg");
    case Icon::Merge: return QStringLiteral(":/icons/git-merge.svg");
    case Icon::Checkout: return QStringLiteral(":/icons/git-compare.svg");
    case Icon::Stash: return QStringLiteral(":/icons/archive.svg");
    case Icon::Pop: return QStringLiteral(":/icons/archive-restore.svg");
    case Icon::Conflicts: return QStringLiteral(":/icons/triangle-alert.svg");
    case Icon::Gear: return QStringLiteral(":/icons/settings.svg");
    case Icon::Search: return QStringLiteral(":/icons/search.svg");
    case Icon::Plus: return QStringLiteral(":/icons/plus.svg");
    case Icon::Close: return QStringLiteral(":/icons/x.svg");
    case Icon::ChevronUp: return QStringLiteral(":/icons/chevron-up.svg");
    case Icon::ChevronDown: return QStringLiteral(":/icons/chevron-down.svg");
    case Icon::ChevronRight: return QStringLiteral(":/icons/chevron-right.svg");
    case Icon::Monitor: return QStringLiteral(":/icons/monitor.svg");
    case Icon::Cloud: return QStringLiteral(":/icons/cloud.svg");
    case Icon::Tag: return QStringLiteral(":/icons/tag.svg");
    case Icon::Folder: return QStringLiteral(":/icons/folder.svg");
    case Icon::Paperclip: return QStringLiteral(":/icons/paperclip.svg");
    case Icon::Ellipsis: return QStringLiteral(":/icons/ellipsis.svg");
    case Icon::FileAdded: return QStringLiteral(":/icons/file-plus.svg");
    case Icon::FileModified: return QStringLiteral(":/icons/file-pen.svg");
    case Icon::FileDeleted: return QStringLiteral(":/icons/file-minus.svg");
    case Icon::FileRenamed: return QStringLiteral(":/icons/arrow-right-left.svg");
    case Icon::Split: return QStringLiteral(":/icons/columns-2.svg");
    case Icon::Wrap: return QStringLiteral(":/icons/text-wrap.svg");
    case Icon::Map: return QStringLiteral(":/icons/map.svg");
    case Icon::Check: return QStringLiteral(":/icons/check.svg");
    case Icon::Info: return QStringLiteral(":/icons/info.svg");
    case Icon::Success: return QStringLiteral(":/icons/circle-check.svg");
    case Icon::Alert: return QStringLiteral(":/icons/triangle-alert.svg");
    case Icon::Worktree: return QStringLiteral(":/icons/git-fork.svg");
    case Icon::Submodule: return QStringLiteral(":/icons/package.svg");
    case Icon::Terminal: return QStringLiteral(":/icons/terminal.svg");
    case Icon::Eye: return QStringLiteral(":/icons/eye.svg");
    case Icon::EyeOff: return QStringLiteral(":/icons/eye-off.svg");
    case Icon::Pin: return QStringLiteral(":/icons/pin.svg");
    case Icon::Command: return QStringLiteral(":/icons/command.svg");
    case Icon::Lock: return QStringLiteral(":/icons/lock.svg");
    case Icon::FileCode: return QStringLiteral(":/icons/file-code.svg");
    case Icon::Trash: return QStringLiteral(":/icons/trash-2.svg");
    case Icon::SortAscending: return QStringLiteral(":/icons/arrow-down-a-z.svg");
    case Icon::SortDescending: return QStringLiteral(":/icons/arrow-up-a-z.svg");
    }
    return {};
}

/// Renders `which` into an opaque-white pixmap and keeps only the drawn pixels,
/// tinted. The SVGs carry `stroke="currentColor"`, which Qt's renderer does not
/// resolve, so the colour is replaced before rendering and then overwritten by
/// the composition: the shape survives, its hue does not.
QPixmap renderTinted(const QString& resource, const QColor& color, int side) {
    QFile file(resource);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QByteArray svg = file.readAll();
    svg.replace("currentColor", "#ffffff");

    QSvgRenderer renderer(svg);
    // Twice the size with a device pixel ratio of two: the icon stays sharp on a
    // HiDPI screen without a second code path.
    QPixmap pixmap(side * 2, side * 2);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&painter, QRectF(0, 0, side, side));
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(QRectF(0, 0, side, side), color);
    }
    return pixmap;
}

QHash<QString, QIcon> g_cache;

} // namespace

QIcon icon(Icon which, const QColor& color, int side) {
    const QString key = QStringLiteral("%1|%2|%3")
                            .arg(static_cast<int>(which))
                            .arg(color.name(QColor::HexArgb))
                            .arg(side);
    // Look up the iterator: QHash::value() returns a copy, and handing out a
    // reference to it is how the keyword table dangled once already.
    const auto it = g_cache.constFind(key);
    if (it != g_cache.constEnd()) {
        return it.value();
    }

    const QPixmap pixmap = renderTinted(resourceFor(which), color, side);
    const QIcon result = pixmap.isNull() ? QIcon() : QIcon(pixmap);
    g_cache.insert(key, result);
    return result;
}

QIcon icon(Icon which) {
    return icon(which, theme::text());
}

void clearIconCache() {
    g_cache.clear();
}

QString iconResource(Icon which) {
    return resourceFor(which);
}

const std::vector<Icon>& allIcons() {
    static const std::vector<Icon> all = [] {
        std::vector<Icon> out;
        for (int i = 0; i <= static_cast<int>(Icon::FileCode); ++i) {
            out.push_back(static_cast<Icon>(i));
        }
        return out;
    }();
    return all;
}

} // namespace gk
