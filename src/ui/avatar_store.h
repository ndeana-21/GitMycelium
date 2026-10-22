#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QUrl>

#include <deque>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace gk {

/// Authors' pictures: GitHub's for a GitHub no-reply address, Gravatar's for any
/// other (its SHA-256 of the address, nothing else leaves the machine). Fetched a few
/// at a time and kept on disk for a week, a miss included, so a repository reopened
/// asks nobody. Until a picture is here, or when there is none, callers draw the
/// initials they drew before.
class AvatarStore : public QObject {
    Q_OBJECT

public:
    static AvatarStore& instance();

    /// Off, no picture is fetched or shown: the initials are drawn. Only the
    /// interactive window turns it on, so a test or a snapshot never depends on the
    /// network or on what an earlier run cached.
    void setEnabled(bool on);
    bool enabled() const { return m_enabled; }

    /// The picture for `email`, round, `side` logical pixels across at `ratio`; null
    /// while it is being fetched or when the author has none.
    QPixmap avatar(const QString& email, int side, qreal ratio);

    /// Where the picture for `email` is asked for, `pixels` across; empty for an
    /// address with no picture service to ask.
    static QUrl sourceFor(const QString& email, int pixels);

    /// Tests: where pictures come from and where they are kept.
    void setSourceForTests(std::function<QUrl(const QString& email)> source);
    void setCacheDirectory(const QString& directory);
    void clearForTests();

signals:
    void avatarReady(QString email);

private:
    explicit AvatarStore(QObject* parent = nullptr);

    QString cachePath(const QString& email, const QString& suffix) const;
    /// What the disk holds: a picture, a remembered miss, or nothing fresh.
    bool readCache(const QString& email);
    void request(const QString& email);
    void startNext();
    void onFinished(QNetworkReply* reply, const QString& email);

    bool m_enabled = false;
    QString m_cacheDir;
    std::function<QUrl(const QString&)> m_source;
    QNetworkAccessManager* m_network = nullptr;
    QHash<QString, QImage> m_images;
    QSet<QString> m_missing;
    QSet<QString> m_pending;
    std::deque<QString> m_queue;
    int m_inFlight = 0;
    QHash<QString, QPixmap> m_rounded;
};

} // namespace gk
