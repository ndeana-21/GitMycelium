#include "ui/badge_flight.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFontMetrics>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>

namespace gk {

QString badgeFlowLogPath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/badge-flow.log");
}

void logBadgeFlow(const QString& line) {
    QFile file(badgeFlowLogPath());
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")) << ' ' << line
           << '\n';
}

QPoint parkedBadgeCentre(const QRect& targetPill, int badgeWidth) {
    return QPoint(targetPill.right() + kBadgeGap + badgeWidth / 2, targetPill.center().y());
}

QPoint listTopLeft(const QPoint& badgeCentre, int badgeWidth) {
    return QPoint(badgeCentre.x() + badgeWidth / 2 + kBadgeGap, badgeCentre.y() - 12);
}

int badgeWidthFor(const QString& branch, const QFontMetrics& metrics) {
    return metrics.horizontalAdvance(branch) + 30;
}
namespace {

/// Fast at the start, settling at the end: a badge that moved at a constant speed
/// reads as machinery, and one that barely moves at the start reads as stuck.
qreal easeOut(qreal t) {
    const qreal inverse = 1.0 - std::clamp(t, 0.0, 1.0);
    return 1.0 - inverse * inverse * inverse;
}

QPoint lerp(const QPoint& from, const QPoint& to, qreal t) {
    return QPoint(qRound(from.x() + (to.x() - from.x()) * t),
                  qRound(from.y() + (to.y() - from.y()) * t));
}

} // namespace

void BadgeFlight::lift(const QString& branch, const QPoint& home, const QPoint& anchor) {
    m_branch = branch;
    m_home = home;
    m_anchor = anchor;
    m_from = home;
    m_phase = Phase::Out;
    m_elapsed = 0;
    m_legStart = 0;
    m_legMs = kLiftMs;
}

void BadgeFlight::land() {
    if (!lifted()) {
        return;   // nothing in the air; landing twice is not an error
    }
    if (m_phase == Phase::Back) {
        return;   // already on the way; do not restart the leg
    }
    m_from = position();
    m_phase = Phase::Back;
    m_legStart = m_elapsed;
    m_legMs = kLandMs;
}

void BadgeFlight::landImmediately() {
    m_branch.clear();
    m_phase = Phase::None;
    m_elapsed = 0;
}

void BadgeFlight::advance(int elapsedMs) {
    if (!lifted() || m_phase == Phase::Parked) {
        return;
    }
    if (elapsedMs > 0) {
        m_elapsed += elapsedMs;
    }

    const int since = m_elapsed - m_legStart;
    const qreal t = m_legMs <= 0 ? 1.0 : static_cast<qreal>(since) / m_legMs;

    if (m_phase == Phase::Out) {
        if (t >= 1.0) {
            m_phase = Phase::Parked;
        }
        return;
    }

    if (m_phase == Phase::Back && t >= 1.0) {
        // Home. The badge stops being lifted at exactly this point, which is what
        // makes `grounded()` a truthful answer rather than a hopeful one.
        landImmediately();
    }
}

bool BadgeFlight::flying() const {
    return lifted() && m_phase != Phase::Parked;
}

QPoint BadgeFlight::position() const {
    if (!lifted()) {
        return m_home;
    }
    if (m_phase == Phase::Parked) {
        return m_anchor;
    }
    const int since = m_elapsed - m_legStart;
    const qreal t = m_legMs <= 0 ? 1.0 : static_cast<qreal>(since) / m_legMs;
    const QPoint target = m_phase == Phase::Out ? m_anchor : m_home;
    return lerp(m_from, target, easeOut(t));
}

} // namespace gk
