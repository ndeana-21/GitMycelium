#include "ui/recent_repositories.h"

#include <QSettings>

#include <algorithm>

namespace gk {
namespace {

/// Fields of one stored entry. A unit separator, not a NUL: a NUL truncates a
/// string read back through the C string APIs the rest of the tree learned about.
const QChar kFieldSep = QChar(0x1f);

const QString kEntriesKey = QStringLiteral("ui/recent/entries");

} // namespace

RecentRepositories::RecentRepositories() = default;

void RecentRepositories::load() {
    QSettings settings;
    const QStringList raw = settings.value(kEntriesKey).toStringList();
    m_entries.clear();
    for (const QString& line : raw) {
        const QStringList parts = line.split(kFieldSep);
        if (parts.size() < 3) {
            continue;
        }
        RecentRepository entry;
        entry.path = parts.at(0);
        entry.name = parts.at(1);
        entry.lastUsed = QDateTime::fromString(parts.at(2), Qt::ISODate);
        if (!entry.path.isEmpty()) {
            m_entries.push_back(entry);
        }
    }
    if (m_entries.size() > kMaxEntries) {
        m_entries.resize(kMaxEntries);
    }
}

void RecentRepositories::touch(const QString& path, const QString& name, const QDateTime& when) {
    if (path.isEmpty()) {
        return;
    }
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [&](const RecentRepository& entry) {
                                       return entry.path == path;
                                   }),
                    m_entries.end());
    RecentRepository entry;
    entry.path = path;
    entry.name = name;
    entry.lastUsed = when;
    m_entries.prepend(entry);
    while (m_entries.size() > kMaxEntries) {
        m_entries.removeLast();
    }
    save();
}

void RecentRepositories::clear() {
    m_entries.clear();
    QSettings settings;
    settings.remove(kEntriesKey);
}

void RecentRepositories::save() const {
    QSettings settings;
    QStringList raw;
    raw.reserve(m_entries.size());
    for (const RecentRepository& entry : m_entries) {
        raw << entry.path + kFieldSep + entry.name + kFieldSep +
                   entry.lastUsed.toString(Qt::ISODate);
    }
    settings.setValue(kEntriesKey, raw);
}

} // namespace gk
