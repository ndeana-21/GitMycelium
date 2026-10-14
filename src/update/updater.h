#pragma once

#include "update/update_manifest.h"

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>

#include <atomic>
#include <memory>
#include <optional>

class QNetworkReply;

namespace gk::update {

enum class InstallKind {
    None,      ///< built from source or unpacked from a tarball: download it by hand
    AppImage,  ///< `$APPIMAGE` names the file; it is replaced in place
    MacBundle, ///< running from a `.app`; the bundle is swapped once the app has quit
};

struct InstallTarget {
    InstallKind kind = InstallKind::None;
    QString path;
};

InstallTarget detectInstallTarget();
QString describeInstallKind(InstallKind kind);

/// Checks the newest release, downloads this platform's package, verifies its
/// sha256 against the manifest and puts it in place of the running copy.
///
/// Everything here is asynchronous: the network through `QNetworkAccessManager`,
/// the macOS disk-image work on the thread pool. Nothing is replaced until the
/// download matches its checksum.
class Updater : public QObject {
    Q_OBJECT

public:
    explicit Updater(QObject* parent = nullptr);
    ~Updater() override;

    /// Test seam: install into this target instead of the detected one.
    void setInstallTarget(const InstallTarget& target) { m_target = target; }
    const InstallTarget& installTarget() const { return m_target; }

    void check();
    void install();
    bool restart();

    bool busy() const { return m_busy; }
    const std::optional<UpdateManifest>& manifest() const { return m_manifest; }
    bool updateAvailable() const;
    QString installRefusal() const;
    bool installed() const { return m_installed; }
    const QString& stagedBundle() const { return m_stagedBundle; }

signals:
    void checked(bool ok, const QString& error);
    void progress(qint64 received, qint64 total);
    void installFinished(bool ok, const QString& error);

private:
    void onAssetData();
    void onAssetFinished();
    void finishInstall(bool ok, const QString& error);
    void stageMacBundle(const QString& diskImage);
    QString downloadPathFor(const UpdateAsset& asset) const;

    QNetworkAccessManager m_network;
    InstallTarget m_target;
    std::optional<UpdateManifest> m_manifest;
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QFile> m_download;
    std::unique_ptr<QCryptographicHash> m_hash;
    UpdateAsset m_asset;
    QString m_stagedBundle;
    bool m_busy = false;
    bool m_installed = false;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
};

} // namespace gk::update
