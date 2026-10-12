#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

#include <vector>

namespace gk {

/// The icon vocabulary of the window chrome.
///
/// The glyphs in `theme/glyphs.h` are drawn as paths because they live inside the
/// commit graph, at eleven pixels, and must be tinted exactly to a lane's colour.
/// The chrome — toolbar, menus, panel bars — wants a real icon set: more shapes
/// than are worth drawing by hand, at a size where a 24px stroke reads cleanly.
/// These are Lucide (ISC), embedded as SVG and recoloured at render time.
enum class Icon {
    Undo,
    Redo,
    Fetch,
    Pull,
    Push,
    Branch,
    Merge,
    Checkout,
    Stash,
    Pop,
    Conflicts,
    Gear,
    Search,
    Plus,
    Close,
    ChevronUp,
    ChevronDown,
    ChevronRight,
    Monitor,
    Cloud,
    Tag,
    Folder,
    Paperclip,
    Ellipsis,
    FileAdded,
    FileModified,
    FileDeleted,
    FileRenamed,
    Split,
    Wrap,
    Map,
    Check,
    Info,
    Success,
    Alert,
    Worktree,
    Submodule,
    Terminal,
    Eye,
    EyeOff,
    Pin,
    Command,
    Lock,
    FileCode,
    Trash,
    SortAscending,
    SortDescending,
};

/// `which`, drawn in `color` at `side` device-independent pixels. The SVGs are
/// monochrome, so the colour is applied when the pixmap is rendered rather than
/// shipped once per variant. The result is cached and owned by the caller's
/// QIcon (implicitly shared), so callers may keep or discard it freely.
QIcon icon(Icon which, const QColor& color, int side = 18);

QIcon icon(Icon which);

void clearIconCache();

QString iconResource(Icon which);

const std::vector<Icon>& allIcons();

} // namespace gk
