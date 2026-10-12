#pragma once

#include <QColor>
#include <QIcon>
#include <QRectF>

class QPainter;

namespace gk {

/// Small glyphs drawn as paths rather than loaded from an icon theme.
///
/// They have to look the same on every platform, at sizes no icon theme ships
/// (eleven pixels in a label pill, fourteen in a tree), and without an asset to
/// install. The graph's pill icons and the branch panel's tree icons come from
/// here for the same reason: two drawings of "cloud" drift apart, and then the
/// panel and the graph disagree about what a remote branch looks like.
enum class Glyph {
    Monitor,   ///< a branch that exists on this machine
    Cloud,     ///< a remote-tracking branch
    Folder,    ///< a group of references sharing a name prefix
    Branch,    ///< a single reference
    Tag,
    Paperclip, ///< pinned to the top of the list
    Stash,
    Worktree,  ///< a linked working tree
    Submodule, ///< a repository nested inside another

    // File status. One glyph per way a file can change, so the list says *how*
    // it changed and not only that it did.
    Plus,      ///< the file was created
    Pencil,    ///< the file was edited
    Minus,     ///< the file was deleted
    Arrow,     ///< the file was moved or copied
};

void paintGlyph(QPainter* painter, Glyph glyph, const QRectF& box, const QColor& colour);

/// The same glyph as an icon, for a model's DecorationRole. Rendered at twice
/// `side` with a matching device pixel ratio, so it stays sharp on HiDPI.
QIcon glyphIcon(Glyph glyph, const QColor& colour, int side);

} // namespace gk
