#pragma once

#include <QPoint>
#include <QRect>
#include <QString>

class QFontMetrics;

namespace gk {

inline constexpr int kBadgeGap = 8;

/// Appends one line to the badge-flow log, with a timestamp.
///
/// The chain from a click to an operation cannot be seen on the screen and cannot be
/// reproduced without a real drag, so it is written where it can be read afterwards.
/// Every step reports: which entry was clicked, which branch and target the view
/// emitted, and what the operation answered.
void logBadgeFlow(const QString& line);

QString badgeFlowLogPath();

/// Where a lifted badge parks: immediately to the right of the badge it was dropped
/// on, edge to edge. By the badge, not by the text — the two have to read as separate
/// things sitting side by side, and a centre placed at the pill's right edge would put
/// the lifted badge on top of the one it is rebasing onto.
QPoint parkedBadgeCentre(const QRect& targetPill, int badgeWidth);

QPoint listTopLeft(const QPoint& badgeCentre, int badgeWidth);

/// The width the badge is drawn at. One function, so the geometry the list is placed
/// by and the geometry the badge is painted at cannot disagree.
int badgeWidthFor(const QString& branch, const QFontMetrics& metrics);

/// A branch badge lifted off its row for as long as a decision is pending, and put
/// back afterwards.
///
/// The rule this class exists to keep is that a badge always comes home. Every exit
/// passes through `land()`, `land()` is safe from any state and any number of times,
/// and `grounded()` is the answer a test can read afterwards. A badge left hanging in
/// the air is the failure mode this is shaped around, so the state machine is here, in
/// a class with no widgets in it, rather than spread through the view's event
/// handlers where it cannot be tested without a display.
class BadgeFlight {
public:
    static constexpr int kLiftMs = 150;
    static constexpr int kLandMs = 130;

    /// `anchor` is the point just right of the badge it is dropped on, where the
    /// operation list opens.
    void lift(const QString& branch, const QPoint& home, const QPoint& anchor);

    /// Starts the way back. Safe to call twice, safe to call before any lift, safe to
    /// call mid-flight: the badge returns from wherever it currently is.
    void land();

    /// Puts the badge home at once, with no return flight. For the paths where no
    /// animation can run — the window hiding, the view being torn down — because a
    /// frozen badge is worse than a badge that never left.
    void landImmediately();

    void advance(int elapsedMs);

    bool lifted() const { return !m_branch.isEmpty(); }
    bool flying() const;
    /// Home, or never left. The invariant worth asserting.
    bool grounded() const { return !lifted(); }

    QPoint position() const;
    QString branch() const { return m_branch; }
    QPoint home() const { return m_home; }
    QPoint anchor() const { return m_anchor; }

private:
    enum class Phase { None, Out, Parked, Back };

    QString m_branch;
    QPoint m_home;
    QPoint m_anchor;
    Phase m_phase = Phase::None;
    QPoint m_from;
    int m_elapsed = 0;
    int m_legStart = 0;
    int m_legMs = kLiftMs;
};

} // namespace gk
