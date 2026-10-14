#pragma once

#include <QColor>
#include <QIcon>
#include <QRectF>

class QPainter;

namespace gk {

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
