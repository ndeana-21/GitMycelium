#include "model/date_format.h"

namespace gk {
namespace {

QString relativeLabel(const QDateTime& when, const QDateTime& now) {
    const qint64 seconds = when.secsTo(now);
    if (seconds < 60) {
        return QStringLiteral("just now");
    }
    const qint64 minutes = seconds / 60;
    if (minutes < 60) {
        return QStringLiteral("%1 min ago").arg(minutes);
    }
    const qint64 hours = minutes / 60;
    if (hours < 24) {
        return QStringLiteral("%1 h ago").arg(hours);
    }
    const qint64 days = hours / 24;
    if (days < 30) {
        return QStringLiteral("%1 d ago").arg(days);
    }
    const qint64 months = days / 30;
    if (months < 12) {
        return QStringLiteral("%1 mo ago").arg(months);
    }
    return QStringLiteral("%1 y ago").arg(months / 12);
}

} // namespace

CommitDateFormat commitDateFormatFromKey(const QString& key, CommitDateFormat fallback) {
    if (key == QStringLiteral("iso")) {
        return CommitDateFormat::Iso;
    }
    if (key == QStringLiteral("european")) {
        return CommitDateFormat::European;
    }
    if (key == QStringLiteral("us")) {
        return CommitDateFormat::Us;
    }
    if (key == QStringLiteral("relative")) {
        return CommitDateFormat::Relative;
    }
    return fallback;
}

QString commitDateFormatKey(CommitDateFormat format) {
    switch (format) {
    case CommitDateFormat::European: return QStringLiteral("european");
    case CommitDateFormat::Us: return QStringLiteral("us");
    case CommitDateFormat::Relative: return QStringLiteral("relative");
    case CommitDateFormat::Iso:
        break;
    }
    return QStringLiteral("iso");
}

QString formatCommitDate(std::int64_t unixSeconds, CommitDateFormat format) {
    return formatCommitDate(QDateTime::fromSecsSinceEpoch(unixSeconds), format);
}

QString formatCommitDate(const QDateTime& when, CommitDateFormat format) {
    switch (format) {
    case CommitDateFormat::European:
        return when.toString(QStringLiteral("dd.MM.yyyy HH:mm"));
    case CommitDateFormat::Us:
        return when.toString(QStringLiteral("MM/dd/yyyy h:mm AP"));
    case CommitDateFormat::Relative:
        return relativeLabel(when, QDateTime::currentDateTime());
    case CommitDateFormat::Iso:
        break;
    }
    return when.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

} // namespace gk
