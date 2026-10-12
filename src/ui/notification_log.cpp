#include "ui/notification_log.h"

namespace gk {

NotificationLog::NotificationLog(QObject* parent) : QObject(parent) {}

void NotificationLog::add(Level level, const QString& text) {
    if (text.trimmed().isEmpty()) {
        return;
    }
    Notification entry;
    entry.when = QDateTime::currentDateTime();
    entry.level = static_cast<int>(level);
    entry.text = text;
    m_entries.prepend(entry);
    while (m_entries.size() > kCapacity) {
        m_entries.removeLast();
    }
    emit changed();
}

void NotificationLog::clear() {
    if (m_entries.isEmpty()) {
        return;
    }
    m_entries.clear();
    emit changed();
}

int NotificationLog::problemCount() const {
    int count = 0;
    for (const Notification& entry : m_entries) {
        if (entry.level >= Warning) {
            ++count;
        }
    }
    return count;
}

} // namespace gk
