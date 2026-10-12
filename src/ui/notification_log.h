#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVector>

namespace gk {

/// One entry in the notification centre: what happened, when, and how serious.
struct Notification {
    QDateTime when;
    /// 0 info, 1 success, 2 warning, 3 error — the order maps onto the theme's
    /// text/success/warning/danger colours.
    int level = 0;
    QString text;
};

/// The background results a reader may not have been looking at: a fetch that
/// finished, a maintenance run, an auto-resolve report, a clone. The status bar
/// shows the last one and then it is gone; this keeps the recent ones so "what did
/// that say" has an answer.
///
/// It holds strings, not operations: nothing here can move a branch.
class NotificationLog : public QObject {
    Q_OBJECT

public:
    enum Level { Info = 0, Success = 1, Warning = 2, Error = 3 };

    explicit NotificationLog(QObject* parent = nullptr);

    void add(Level level, const QString& text);
    /// The entries, newest first.
    const QVector<Notification>& entries() const { return m_entries; }
    void clear();

    int problemCount() const;

    /// Entries older than this are dropped: a notification centre that grows
    /// without bound is a log file, not a noticeboard.
    static constexpr int kCapacity = 200;

signals:
    void changed();

private:
    QVector<Notification> m_entries;
};

} // namespace gk
