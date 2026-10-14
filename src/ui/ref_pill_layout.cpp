#include "ui/ref_pill_layout.h"

#include "theme/theme.h"

#include <QFontMetrics>
#include <QHash>

namespace gk {
namespace {

constexpr int kPadX = 6;
constexpr int kGap = 2;
constexpr int kMarginY = 2;
constexpr int kMaxPillH = 18; ///< a single label gets a taller pill than stacked ones
constexpr int kBadgeW = 24;
constexpr int kIconSide = 11;
constexpr int kHeadSide = 9;
constexpr int kTrackGap = 3;

/// Kept next to the geometry so painting and layout cannot disagree about whether a
/// pill is wider than its text.
bool kindHasIcon(RefKind kind) {
    return kind == RefKind::LocalBranch || kind == RefKind::RemoteBranch;
}

int iconRoom(RefKind kind, bool hasRemote) {
    int room = kindHasIcon(kind) ? kIconSide + 3 : 0;
    if (hasRemote) {
        room += kIconSide + 2;
    }
    return room;
}

int headRoom(bool isHead) {
    return isHead ? kHeadSide + 3 : 0;
}

bool isBranchKind(RefKind kind) {
    return kind == RefKind::LocalBranch || kind == RefKind::RemoteBranch;
}

/// `origin/main` names a local `main`; the first path segment is the remote. Empty
/// when there is no slash to split on.
QString remoteCounterpart(const QString& remoteShortName) {
    const int slash = remoteShortName.indexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : remoteShortName.mid(slash + 1);
}

} // namespace

QString RefPillLayout::trackingText(int flags) {
    // Arrows, not numbers: git answers the relation for every branch in one
    // `for-each-ref`, but the counts only for the checked-out one, and a badge that
    // reads "2" on HEAD and "↑" elsewhere would be worse than one that reads the
    // same everywhere.
    switch (flags & 3) {
    case 1: return QStringLiteral("\u2191");
    case 2: return QStringLiteral("\u2193");
    case 3: return QStringLiteral("\u2191\u2193");
    default: return QString();
    }
}

std::vector<RefPill> RefPillLayout::arrange(const QStringList& labels, const QVariantList& colors,
                                            const QVariantList& kinds,
                                            const QVariantList& trackings, const QRect& cell,
                                            const QString& headBranch, const QString& focusedBranch,
                                            bool showIcon, int maxVisible) {
    std::vector<RefPill> pills;
    if (labels.isEmpty() || cell.height() <= 0) {
        return pills;
    }

    const auto kindAt = [&kinds](int i) {
        return i < kinds.size() ? static_cast<RefKind>(kinds.at(i).toInt()) : RefKind::Other;
    };
    const auto colorAt = [&colors](int i) { return i < colors.size() ? colors.at(i).toInt() : 0; };
    const auto trackingAt = [&trackings](int i) {
        return i < trackings.size() ? (trackings.at(i).toInt() & 3) : 0;
    };

    // Branches sharing a commit collapse into the branch the reader most likely means,
    // marked with how many others are on it: "main +3". A local branch and its
    // remote-tracking counterpart are one logical branch — `main` and `origin/main` are
    // not two places to stand — so the pair never reads as "+1".
    struct Group {
        QVector<int> members;
        int best = -1;
        int bestScore = 1000;
        bool hasRemote = false;
    };
    std::vector<Group> groups;
    QHash<QString, int> localGroup;
    for (int i = 0; i < labels.size(); ++i) {
        if (kindAt(i) == RefKind::LocalBranch) {
            Group group;
            group.members << i;
            localGroup.insert(labels.at(i), static_cast<int>(groups.size()));
            groups.push_back(group);
        }
    }
    for (int i = 0; i < labels.size(); ++i) {
        if (kindAt(i) != RefKind::RemoteBranch) {
            continue;
        }
        const auto known = localGroup.constFind(remoteCounterpart(labels.at(i)));
        if (known != localGroup.constEnd()) {
            groups[known.value()].members << i;
            groups[known.value()].hasRemote = true;
        } else {
            Group group;
            group.members << i;
            group.hasRemote = true;
            groups.push_back(group);
        }
    }
    for (Group& group : groups) {
        for (const int index : group.members) {
            int score = kindAt(index) == RefKind::LocalBranch ? 2 : 3;
            const QString name = labels.at(index);
            if (!headBranch.isEmpty() && name == headBranch) {
                score = 0;
            } else if (!focusedBranch.isEmpty() && name == focusedBranch) {
                score = 1;
            }
            if (score < group.bestScore) {
                group.bestScore = score;
                group.best = index;
            }
        }
    }

    int bestBranch = -1;
    int bestScore = 1000;
    bool bestHasRemote = false;
    for (const Group& group : groups) {
        if (group.bestScore < bestScore) {
            bestScore = group.bestScore;
            bestBranch = group.best;
            bestHasRemote = group.hasRemote && kindAt(group.best) == RefKind::LocalBranch;
        }
    }
    const int branchCount = static_cast<int>(groups.size());
    // One name per other branch: a local branch stands for its remote-tracking twin
    // on the same commit (marked with the cloud), and the shown branch's twin is
    // already the cloud on its own pill.
    QStringList collapsed;
    QStringList collapsedWithRemote;
    for (const Group& group : groups) {
        if (group.members.contains(bestBranch)) {
            continue;
        }
        const int local = group.members.front();
        const bool paired = kindAt(local) == RefKind::LocalBranch && group.members.size() > 1;
        if (paired) {
            collapsed << labels.at(local);
            collapsedWithRemote << labels.at(local);
            continue;
        }
        for (const int index : group.members) {
            collapsed << labels.at(index);
        }
    }

    QStringList shown;
    QVariantList shownColors;
    QVariantList shownKinds;
    QVariantList shownTrackings;
    QVector<int> others;
    QVector<bool> shownRemote;
    QList<QStringList> shownCollapsed;
    QList<QStringList> shownCollapsedRemote;
    for (int i = 0; i < labels.size(); ++i) {
        if (isBranchKind(kindAt(i))) {
            if (i != bestBranch) {
                continue;
            }
            shown << labels.at(i);
            shownColors << colorAt(i);
            shownKinds << static_cast<int>(kindAt(i));
            shownTrackings << trackingAt(i);
            others << (branchCount - 1);
            shownRemote << bestHasRemote;
            shownCollapsed << collapsed;
            shownCollapsedRemote << collapsedWithRemote;
            continue;
        }
        shown << labels.at(i);
        shownColors << colorAt(i);
        shownKinds << static_cast<int>(kindAt(i));
        shownTrackings << trackingAt(i);
        others << 0;
        shownRemote << false;
        shownCollapsed << QStringList{};
        shownCollapsedRemote << QStringList{};
    }

    const int total = shown.size();
    int visible = std::min(total, std::max(1, maxVisible));
    const bool overflow = total > visible;

    // Reserve room for the badge on the last visible line.
    const int available = cell.height() - 2 * kMarginY;
    int pillH = (available - (visible - 1) * kGap) / visible;
    pillH = std::min(pillH, kMaxPillH);
    if (pillH < 8) {
        pillH = 8;
    }

    const QFontMetrics metrics(theme::labelFont());
    int y = cell.top() + kMarginY;

    for (int i = 0; i < visible; ++i) {
        const bool last = (i == visible - 1);
        const int rightLimit = overflow && last ? cell.right() - kBadgeW : cell.right();

        RefPill pill;
        // The name stays the real one — it is what a drag carries and what a
        // hit-test resolves — while the drawn text may carry the count.
        pill.shortName = shown.at(i);
        pill.colorIndex = shownColors.at(i).toInt();
        pill.kind = static_cast<RefKind>(shownKinds.at(i).toInt());
        pill.hasRemote = shownRemote.at(i);
        pill.collapsedNames = shownCollapsed.at(i);
        pill.collapsedWithRemote = shownCollapsedRemote.at(i);

        pill.isHead = !headBranch.isEmpty() && pill.shortName == headBranch;

        QString caption = pill.shortName;
        if (others.at(i) > 0) {
            caption += QStringLiteral(" +%1").arg(others.at(i));
        }

        pill.trackingText = RefPillLayout::trackingText(shownTrackings.at(i).toInt());

        const int room = showIcon ? iconRoom(pill.kind, pill.hasRemote) : 0;
        const int lead = showIcon ? headRoom(pill.isHead) : 0;
        const int badgeW =
            pill.trackingText.isEmpty() ? 0 : metrics.horizontalAdvance(pill.trackingText);
        const int badgeRoom = badgeW > 0 ? badgeW + kTrackGap : 0;
        const int maxTextW =
            std::max(20, rightLimit - cell.left() - 2 * kPadX - room - lead - badgeRoom);
        pill.text = metrics.elidedText(caption, Qt::ElideRight, maxTextW);
        const int w =
            std::min(rightLimit - cell.left(), metrics.horizontalAdvance(pill.text) + 2 * kPadX +
                                                   room + lead + badgeRoom);
        pill.rect = QRect(cell.left(), y, w, pillH);

        if (showIcon && pill.isHead) {
            const int side = std::min(kHeadSide, std::max(6, pillH - 6));
            pill.headRect = QRect(pill.rect.left() + kPadX - 1,
                                  pill.rect.center().y() - side / 2, side, side);
        }

        const int textW = std::max(0, w - 2 * kPadX - room - lead - badgeRoom);
        pill.textRect = QRect(pill.rect.left() + kPadX + lead, pill.rect.top(), textW, pillH);

        // The right-hand furniture is placed from the right edge inwards, so the three
        // cannot claim the same pixels and the reservation above matches what is drawn.
        const int iconSide = std::min(kIconSide, std::max(6, pillH - 5));
        int xRight = pill.rect.right() - kPadX / 2;
        if (showIcon && kindHasIcon(pill.kind)) {
            if (pill.hasRemote) {
                pill.remoteRect = QRect(xRight - iconSide,
                                        pill.rect.center().y() - iconSide / 2 + 1, iconSide,
                                        iconSide);
                xRight -= iconSide + 2;
            }
            pill.iconRect = QRect(xRight - iconSide, pill.rect.center().y() - iconSide / 2 + 1,
                                  iconSide, iconSide);
            xRight -= iconSide + 2;
        }
        if (badgeW > 0) {
            pill.badgeRect = QRect(xRight - badgeW, pill.rect.top(), badgeW, pillH);
        }

        pills.push_back(pill);
        y += pillH + kGap;
    }

    if (overflow) {
        // Dragging this badge is not meaningful, so it is marked and never treated as
        // a reference.
        RefPill badge;
        badge.overflow = true;
        badge.hiddenCount = total - visible;
        badge.text = QStringLiteral("+%1").arg(badge.hiddenCount);
        const int w = std::min(kBadgeW, metrics.horizontalAdvance(badge.text) + 12);
        const int top = y - kGap - pillH;
        badge.rect = QRect(cell.right() - w, top, w, pillH);
        pills.push_back(badge);
    }

    return pills;
}

const RefPill* RefPillLayout::hitTest(const std::vector<RefPill>& pills, const QPoint& pos) {
    for (const RefPill& pill : pills) {
        if (pill.rect.contains(pos)) {
            return &pill;
        }
    }
    return nullptr;
}

} // namespace gk
