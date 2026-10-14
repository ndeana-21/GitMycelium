#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

namespace gk {

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

    void load();

    const QVector<RecentRepository>& entries() const { return m_entries; }

    void touch(const QString& path, const QString& name,
               const QDateTime& when = QDateTime::currentDateTime());

    void clear();

private:
    void save() const;

    QVector<RecentRepository> m_entries;
};

} // namespace gk
