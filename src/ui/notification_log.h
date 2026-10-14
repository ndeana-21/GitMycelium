#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVector>

namespace gk {

struct Notification {
    QDateTime when;
    /// 0 info, 1 success, 2 warning, 3 error — the order maps onto the theme's
    /// text/success/warning/danger colours.
    int level = 0;
    QString text;
};

class NotificationLog : public QObject {
    Q_OBJECT

public:
    enum Level { Info = 0, Success = 1, Warning = 2, Error = 3 };

    explicit NotificationLog(QObject* parent = nullptr);

    void add(Level level, const QString& text);
    const QVector<Notification>& entries() const { return m_entries; }
    void clear();

    int problemCount() const;

    static constexpr int kCapacity = 200;

signals:
    void changed();

private:
    QVector<Notification> m_entries;
};

} // namespace gk
