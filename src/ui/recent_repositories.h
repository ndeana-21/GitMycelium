#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

namespace gk {

/// One repository the reader used recently, and when.
struct RecentRepository {
    QString path;       ///< canonical checkout path
    QString name;       ///< directory name, for display
    QDateTime lastUsed; ///< last open or tab activation
};

/// The repositories opened or switched to recently, most recent first.
///
/// Stored in `QSettings` under `ui/recent`; capped at ten; keyed and de-duplicated
/// by the checkout path, so using the same repository again moves its entry to the
/// front rather than adding a second row. The "last used" time is open/activation,
/// not every git call: a fetch by another client is not the reader's action.
class RecentRepositories {
public:
    static constexpr int kMaxEntries = 10;

    RecentRepositories();

    /// Reads the stored list. A missing or unreadable value is not an error: it
    /// means nothing has been opened yet.
    void load();

    const QVector<RecentRepository>& entries() const { return m_entries; }

    /// Records that `path` was used at `when` (now by default): moves it to the
    /// front and drops the oldest beyond the cap.
    void touch(const QString& path, const QString& name,
               const QDateTime& when = QDateTime::currentDateTime());

    void clear();

private:
    void save() const;

    QVector<RecentRepository> m_entries;
};

} // namespace gk
