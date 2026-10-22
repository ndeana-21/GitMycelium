#include "ui/avatar_store.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QStandardPaths>

namespace gk {
namespace {

constexpr int kFetchPixels = 96;
constexpr int kMaxInFlight = 4;
constexpr qint64 kFreshSeconds = 7 * 24 * 3600;

QString normalized(const QString& email) {
    return email.trimmed().toLower();
}

bool fresh(const QFileInfo& file) {
    return file.lastModified().secsTo(QDateTime::currentDateTime()) < kFreshSeconds;
}

} // namespace

AvatarStore& AvatarStore::instance() {
    static AvatarStore* store = new AvatarStore;
    return *store;
}

AvatarStore::AvatarStore(QObject* parent) : QObject(parent) {
    m_cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                 QStringLiteral("/avatars");
}

void AvatarStore::setEnabled(bool on) {
    m_enabled = on;
    if (on) {
        startNext();
    }
}

void AvatarStore::setSourceForTests(std::function<QUrl(const QString&)> source) {
    m_source = std::move(source);
}

void AvatarStore::setCacheDirectory(const QString& directory) {
    m_cacheDir = directory;
}

void AvatarStore::clearForTests() {
    m_images.clear();
    m_missing.clear();
    m_pending.clear();
    m_queue.clear();
    m_rounded.clear();
}

QUrl AvatarStore::sourceFor(const QString& email, int pixels) {
    const QString address = normalized(email);
    if (!address.contains(QLatin1Char('@'))) {
        return QUrl();
    }
    // GitHub's no-reply addresses name the account: `<id>+<login>@…` since 2017, and
    // `<login>@…` before that.
    static const QRegularExpression withId(
        QStringLiteral("^(\\d+)\\+[^@]+@users\\.noreply\\.github\\.com$"));
    static const QRegularExpression loginOnly(
        QStringLiteral("^([^@+]+)@users\\.noreply\\.github\\.com$"));
    if (const auto match = withId.match(address); match.hasMatch()) {
        return QUrl(QStringLiteral("https://avatars.githubusercontent.com/u/%1?s=%2&v=4")
                        .arg(match.captured(1))
                        .arg(pixels));
    }
    if (const auto match = loginOnly.match(address); match.hasMatch()) {
        return QUrl(QStringLiteral("https://github.com/%1.png?size=%2")
                        .arg(match.captured(1))
                        .arg(pixels));
    }
    const QByteArray hash =
        QCryptographicHash::hash(address.toUtf8(), QCryptographicHash::Sha256).toHex();
    // `d=404`: an address Gravatar does not know is a miss, not a generated pattern.
    return QUrl(QStringLiteral("https://www.gravatar.com/avatar/%1?s=%2&d=404")
                    .arg(QString::fromLatin1(hash))
                    .arg(pixels));
}

QString AvatarStore::cachePath(const QString& email, const QString& suffix) const {
    const QByteArray key =
        QCryptographicHash::hash(email.toUtf8(), QCryptographicHash::Sha256).toHex();
    return m_cacheDir + QLatin1Char('/') + QString::fromLatin1(key) + suffix;
}

bool AvatarStore::readCache(const QString& email) {
    const QFileInfo picture(cachePath(email, QStringLiteral(".png")));
    if (picture.exists()) {
        QImage image(picture.filePath());
        if (!image.isNull()) {
            m_images.insert(email, image);
            return fresh(picture);
        }
    }
    const QFileInfo miss(cachePath(email, QStringLiteral(".none")));
    if (miss.exists() && fresh(miss)) {
        m_missing.insert(email);
        return true;
    }
    return false;
}

QPixmap AvatarStore::avatar(const QString& email, int side, qreal ratio) {
    const QString address = normalized(email);
    if (!m_enabled || address.isEmpty() || side <= 0) {
        return QPixmap();
    }
    if (!m_images.contains(address) && !m_missing.contains(address) &&
        !m_pending.contains(address)) {
        if (!readCache(address)) {
            request(address);
        }
    }
    const auto image = m_images.constFind(address);
    if (image == m_images.cend()) {
        return QPixmap();
    }
    const QString key = QStringLiteral("%1|%2|%3").arg(address).arg(side).arg(ratio);
    if (const auto rounded = m_rounded.constFind(key); rounded != m_rounded.cend()) {
        return *rounded;
    }
    const int pixels = qMax(1, qRound(side * ratio));
    QPixmap pixmap(pixels, pixels);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QPainterPath circle;
        circle.addEllipse(QRectF(0, 0, pixels, pixels));
        painter.setClipPath(circle);
        painter.drawImage(QRectF(0, 0, pixels, pixels), *image);
    }
    pixmap.setDevicePixelRatio(ratio);
    m_rounded.insert(key, pixmap);
    return pixmap;
}

void AvatarStore::request(const QString& email) {
    if (m_pending.contains(email)) {
        return;
    }
    m_pending.insert(email);
    m_queue.push_back(email);
    startNext();
}

void AvatarStore::startNext() {
    if (!m_enabled) {
        return;
    }
    while (m_inFlight < kMaxInFlight && !m_queue.empty()) {
        const QString email = m_queue.front();
        m_queue.pop_front();
        const QUrl url = m_source ? m_source(email) : sourceFor(email, kFetchPixels);
        if (!url.isValid() || url.isEmpty()) {
            m_pending.remove(email);
            m_missing.insert(email);
            continue;
        }
        if (!m_network) {
            m_network = new QNetworkAccessManager(this);
        }
        QNetworkRequest ask(url);
        ask.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        ask.setTransferTimeout(15000);
        QNetworkReply* reply = m_network->get(ask);
        ++m_inFlight;
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, email] { onFinished(reply, email); });
    }
}

void AvatarStore::onFinished(QNetworkReply* reply, const QString& email) {
    reply->deleteLater();
    --m_inFlight;
    m_pending.remove(email);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool missing = status == 404 || status == 410;
    QImage image;
    if (reply->error() == QNetworkReply::NoError) {
        image.loadFromData(reply->readAll());
    }
    QDir().mkpath(m_cacheDir);
    if (!image.isNull()) {
        image.save(cachePath(email, QStringLiteral(".png")), "PNG");
        QFile::remove(cachePath(email, QStringLiteral(".none")));
        m_images.insert(email, image);
        m_missing.remove(email);
        for (auto it = m_rounded.begin(); it != m_rounded.end();) {
            it = it.key().startsWith(email + QLatin1Char('|')) ? m_rounded.erase(it) : ++it;
        }
        emit avatarReady(email);
    } else if (missing) {
        QFile mark(cachePath(email, QStringLiteral(".none")));
        if (mark.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            mark.close();
        }
        m_missing.insert(email);
    } else {
        // Offline or refused: asked again in a later session, not remembered as a miss.
        m_missing.insert(email);
    }
    startNext();
}

} // namespace gk
