#pragma once

#include <QDateTime>
#include <QString>

#include <cstdint>

namespace gk {

/// How a commit's date reads in the list and the details panel. One choice for the
/// whole window, so a date cannot read one way in the header and another in a row.
enum class CommitDateFormat {
    /// `yyyy-MM-dd HH:mm` — the default.
    Iso = 0,
    /// `dd.MM.yyyy HH:mm`.
    European,
    /// `MM/dd/yyyy h:mm AP`.
    Us,
    /// "3 h ago", "5 d ago" — measured against now.
    Relative,
};

/// The stored spelling of a format, and its inverse. An unknown key keeps `fallback`,
/// so a profile written by a newer build still reads.
CommitDateFormat commitDateFormatFromKey(const QString& key,
                                         CommitDateFormat fallback = CommitDateFormat::Iso);
QString commitDateFormatKey(CommitDateFormat format);

/// Formats a commit's timestamp. `Relative` is measured against the current clock;
/// the others are absolute. Two overloads so the absolute forms are testable without
/// depending on now.
QString formatCommitDate(std::int64_t unixSeconds, CommitDateFormat format);
QString formatCommitDate(const QDateTime& when, CommitDateFormat format);

} // namespace gk
