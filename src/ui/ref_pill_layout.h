#pragma once

#include "core/types.h"

#include <QPoint>
#include <QRect>
#include <QString>
#include <QVariantList>
#include <QStringList>

#include <vector>

namespace gk {

/// One drawn branch/tag label inside the graph's label column.
struct RefPill {
    QRect rect;
    /// Kept here rather than recomputed by the painter, so the text cannot drift
    /// from the icon beside it.
    QRect textRect;
    /// Null when the kind has no icon.
    QRect iconRect;
    /// The remote counterpart's cloud, when a local branch is drawn with its
    /// remote-tracking branch. Null otherwise.
    QRect remoteRect;
    /// A leading glyph, before the name: it answers "where am I", so it comes first.
    QRect headRect;
    /// Between the name and the kind icon. Null when there is no upstream, or level.
    QRect badgeRect;
    /// "↑" (ahead), "↓" (behind), "↑↓" (diverged). Empty when there is no badge.
    QString trackingText;
    bool isHead = false;
    QString text;      ///< elided for display
    QString shortName; ///< full name, for the drag payload
    int colorIndex = 0;
    RefKind kind = RefKind::Other;
    /// A local branch drawn with its remote-tracking counterpart at the same commit.
    bool hasRemote = false;
    bool overflow = false; ///< the "+N" badge rather than a real reference
    int hiddenCount = 0;   ///< how many labels the badge stands for
    /// The names folded behind "name +N", in order; empty unless this pill stands
    /// for several branches, and revealed under the pill on hover.
    QStringList collapsedNames;
    /// Those of `collapsedNames` whose remote-tracking twin sits on the same commit.
    QStringList collapsedWithRemote;
};

/// Geometry of the label column, shared by painting and by hit-testing.
///
/// Both must agree exactly: the delegate decides where a label is drawn, and
/// the view decides which label the user grabbed or dropped onto. Computing
/// that twice is how drag-and-drop ends up off by one row.
class RefPillLayout {
public:
    /// Lays out up to `maxVisible` labels stacked in the cell, plus a "+N" badge when
    /// more references point at the same commit. Branches at the same commit collapse
    /// into one pill — named after `headBranch`, else `focusedBranch`, else a local
    /// branch, else name order — captioned `name +N`. Tags keep their own pill: a tag
    /// is not a place the working tree can be.
    ///
    /// `headBranch`'s pill gets the leading checkmark. `showIcon` false drops the kind
    /// glyph, the checked-out tick and the remote cloud, giving their width back to the
    /// name. Painting and hit-testing callers must pass the same values, or the two
    /// disagree by exactly the icon's width.
    ///
    /// `trackings` is parallel to `labels`: bit 0 ahead of its upstream, bit 1 behind.
    /// A separate list, and *required* rather than defaulted, so a caller cannot forget
    /// it and draw a narrower pill than the hit test resolves; a missing entry is "no
    /// badge", never a crash.
    static std::vector<RefPill> arrange(const QStringList& labels, const QVariantList& colors,
                                        const QVariantList& kinds, const QVariantList& trackings,
                                        const QRect& cell,
                                        const QString& headBranch = QString(),
                                        const QString& focusedBranch = QString(),
                                        bool showIcon = true, int maxVisible = 2);

    static const RefPill* hitTest(const std::vector<RefPill>& pills, const QPoint& pos);

    /// The badge text for tracking flags: bit 0 ahead, bit 1 behind. One spelling for
    /// the pill layout and the reveal overlay, which builds its pills by hand.
    static QString trackingText(int flags);
};

} // namespace gk
