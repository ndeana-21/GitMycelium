#include "update/updater.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>

#include <cstdio>

namespace gk::update {
namespace {

constexpr int kNetworkTimeoutMs = 30000;
constexpr int kToolTimeoutMs = 120000;

QNetworkRequest requestFor(const QUrl& url) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("GitMycelium/%1").arg(currentVersion()));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kNetworkTimeoutMs);
    return request;
}

bool directoryWritable(const QString& dir) {
    return QFileInfo(dir).isDir() && QFileInfo(dir).isWritable();
}

/// Runs a tool to completion; on failure returns its own words. `input` goes to its
/// stdin, which is closed after it, so a tool that asks a question gets an answer
/// (or end of input) instead of waiting forever.
QString runTool(const QString& program, const QStringList& args, const QByteArray& input = {}) {
    QProcess process;
    process.start(program, args);
    if (process.waitForStarted()) {
        process.write(input);
        process.closeWriteChannel();
    }
    if (!process.waitForFinished(kToolTimeoutMs)) {
        process.kill();
        process.waitForFinished();
        return QStringLiteral("%1 did not finish").arg(program);
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString err = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        return QStringLiteral("%1 failed: %2").arg(program, err.isEmpty() ? QStringLiteral("exit %1").arg(process.exitCode()) : err);
    }
    return {};
}

/// Waits for the old process to go, swaps the staged copy in (the bundle case) and
/// starts the new one. A shell outlives the application that started it.
const char* const kRelaunchScript = R"SH(
pid=$1; target=$2; staged=$3
while kill -0 "$pid" 2>/dev/null; do sleep 0.2; done
if [ -n "$staged" ]; then
    old="$target.replaced"
    rm -rf "$old"
    if mv "$target" "$old"; then
        if mv "$staged" "$target"; then rm -rf "$old"; else mv "$old" "$target"; fi
    fi
    touch "$target"
    /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$target" >/dev/null 2>&1
    exec open "$target"
fi
exec "$target"
)SH";

/// The Windows counterpart: the folder cannot be renamed while the executable and its
/// DLLs inside it are loaded, so the swap waits for our process, and retries while
/// the handles close.
const char* const kWindowsRelaunchScript = R"PS(
param([int]$ProcessId, [string]$Target, [string]$Staged)
Wait-Process -Id $ProcessId -ErrorAction SilentlyContinue
$old = "$Target.replaced"
if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Recurse -Force }
for ($i = 0; $i -lt 50; $i++) {
    try { Move-Item -LiteralPath $Target -Destination $old -ErrorAction Stop; break }
    catch { Start-Sleep -Milliseconds 200 }
}
if (Test-Path -LiteralPath $old) {
    try {
        Move-Item -LiteralPath $Staged -Destination $Target -ErrorAction Stop
        Remove-Item -LiteralPath $old -Recurse -Force -ErrorAction SilentlyContinue
    } catch {
        Move-Item -LiteralPath $old -Destination $Target
    }
}
Start-Process -FilePath (Join-Path $Target 'GitMycelium.exe')
Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue
)PS";

/// The folder of a release archive that holds the executable: the archive's root or
/// the one folder inside it.
QString packagedAppFolder(const QString& extracted) {
    const QString exe = QStringLiteral("GitMycelium.exe");
    if (QFileInfo::exists(QDir(extracted).filePath(exe))) {
        return extracted;
    }
    const QStringList children = QDir(extracted).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (children.size() == 1 && QFileInfo::exists(QDir(extracted).filePath(children.front() +
                                                                              QLatin1Char('/') + exe))) {
        return QDir(extracted).filePath(children.front());
    }
    return {};
}

} // namespace

InstallTarget detectInstallTarget() {
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (!appImage.isEmpty() && QFileInfo(appImage).isFile()) {
        return {InstallKind::AppImage, QFileInfo(appImage).absoluteFilePath()};
    }
#if defined(Q_OS_MACOS)
    // <bundle>.app/Contents/MacOS/<binary>
    QDir dir(QCoreApplication::applicationDirPath());
    if (dir.cdUp() && dir.cdUp() && dir.dirName().endsWith(QLatin1String(".app"))) {
        return {InstallKind::MacBundle, dir.absolutePath()};
    }
#elif defined(Q_OS_WIN)
    // A release folder carries Qt beside the executable (windeployqt); a build tree
    // loads it from the Qt installation and is not something to replace.
    const QString dir = QCoreApplication::applicationDirPath();
    if (QFileInfo::exists(QDir(dir).filePath(QStringLiteral("Qt6Core.dll")))) {
        return {InstallKind::WindowsFolder, QDir(dir).absolutePath()};
    }
#endif
    return {};
}

QString describeInstallKind(InstallKind kind) {
    switch (kind) {
    case InstallKind::AppImage:
        return QStringLiteral("appimage");
    case InstallKind::MacBundle:
        return QStringLiteral("app-bundle");
    case InstallKind::WindowsFolder:
        return QStringLiteral("windows-folder");
    case InstallKind::None:
        break;
    }
    return QStringLiteral("none");
}

Updater::Updater(QObject* parent) : QObject(parent), m_target(detectInstallTarget()) {}

Updater::~Updater() {
    m_alive->store(false);
    if (m_reply) {
        m_reply->abort();
    }
}

bool Updater::updateAvailable() const {
    return m_manifest && isNewer(*m_manifest, currentVersion(), currentBuild());
}

QString Updater::installRefusal() const {
    if (!updateAvailable()) {
        return QStringLiteral("No newer build is published.");
    }
    if (m_target.kind == InstallKind::None) {
        return QStringLiteral("This copy was not installed from a release package (AppImage, "
                              "macOS app or Windows archive), so it cannot replace itself. Download the new "
                              "version from the release page.");
    }
    if (!m_manifest->assets.contains(platformAssetKey())) {
        return QStringLiteral("The release has no package for this platform.");
    }
    const QString dir = QFileInfo(m_target.path).absolutePath();
    if (!directoryWritable(dir)) {
        return QStringLiteral("%1 is not writable, so the update cannot be put in place. "
                              "Download it from the release page.")
            .arg(QDir::toNativeSeparators(dir));
    }
    return {};
}

void Updater::check() {
    if (m_busy) {
        return;
    }
    m_busy = true;
    QNetworkReply* reply = m_network.get(requestFor(manifestUrl()));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_busy = false;
        if (reply->error() == QNetworkReply::ContentNotFoundError) {
            emit checked(false, QStringLiteral("The newest release publishes no update "
                                               "manifest; check the release page by hand."));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            emit checked(false, QStringLiteral("Could not reach the release page: %1")
                                    .arg(reply->errorString()));
            return;
        }
        QString error;
        std::optional<UpdateManifest> manifest = parseManifest(reply->readAll(), &error);
        if (!manifest) {
            emit checked(false, error);
            return;
        }
        m_manifest = std::move(manifest);
        emit checked(true, QString());
    });
}

QString Updater::downloadPathFor(const UpdateAsset& asset) const {
    const QString name = QFileInfo(asset.url.path()).fileName();
    if (m_target.kind == InstallKind::AppImage) {
        // Beside the AppImage, so the final rename stays on one filesystem.
        const QFileInfo target(m_target.path);
        return target.absoluteDir().filePath(QStringLiteral(".%1.download").arg(target.fileName()));
    }
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("gitmycelium-update-%1").arg(name));
}

void Updater::install() {
    if (m_busy) {
        return;
    }
    const QString refusal = installRefusal();
    if (!refusal.isEmpty()) {
        emit installFinished(false, refusal);
        return;
    }
    m_busy = true;
    m_installed = false;
    m_asset = m_manifest->assets.value(platformAssetKey());

    m_download = std::make_unique<QFile>(downloadPathFor(m_asset));
    if (!m_download->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        finishInstall(false, QStringLiteral("Cannot write %1: %2")
                                 .arg(m_download->fileName(), m_download->errorString()));
        return;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);

    m_reply = m_network.get(requestFor(m_asset.url));
    connect(m_reply, &QNetworkReply::readyRead, this, &Updater::onAssetData);
    connect(m_reply, &QNetworkReply::downloadProgress, this, &Updater::progress);
    connect(m_reply, &QNetworkReply::finished, this, &Updater::onAssetFinished);
}

void Updater::onAssetData() {
    const QByteArray chunk = m_reply->readAll();
    m_hash->addData(chunk);
    if (m_download->write(chunk) != chunk.size()) {
        m_reply->abort();
    }
}

void Updater::onAssetFinished() {
    QNetworkReply* reply = m_reply;
    reply->deleteLater();
    if (reply->error() == QNetworkReply::NoError) {
        onAssetData();
    }
    const QString path = m_download->fileName();
    const qint64 written = m_download->size();
    const bool flushed = m_download->flush();
    m_download->close();

    QString error;
    if (reply->error() != QNetworkReply::NoError) {
        error = QStringLiteral("The download failed: %1").arg(reply->errorString());
    } else if (!flushed) {
        error = QStringLiteral("Cannot write %1").arg(path);
    } else if (m_asset.size > 0 && written != m_asset.size) {
        error = QStringLiteral("The download is %1 bytes, the release says %2.")
                    .arg(written)
                    .arg(m_asset.size);
    } else if (QString::fromLatin1(m_hash->result().toHex()) != m_asset.sha256) {
        error = QStringLiteral("The download does not match the release's sha256; nothing was "
                               "replaced.");
    }
    if (!error.isEmpty()) {
        QFile::remove(path);
        finishInstall(false, error);
        return;
    }

    if (m_target.kind == InstallKind::AppImage) {
        QFile::setPermissions(path, QFile::permissions(m_target.path) | QFileDevice::ExeOwner |
                                        QFileDevice::ExeGroup | QFileDevice::ExeOther);
        // rename(2) replaces the file in one step; the running AppImage keeps its
        // mounted copy of the old one.
        if (std::rename(QFile::encodeName(path).constData(),
                        QFile::encodeName(m_target.path).constData()) != 0) {
            QFile::remove(path);
            finishInstall(false, QStringLiteral("Cannot replace %1").arg(m_target.path));
            return;
        }
        finishInstall(true, QString());
        return;
    }
    if (m_target.kind == InstallKind::WindowsFolder) {
        stageWindowsFolder(path);
        return;
    }
    stageMacBundle(path);
}

void Updater::stageWindowsFolder(const QString& archive) {
    const QString folder = m_target.path;
    const QString staged = QFileInfo(folder).absoluteDir().filePath(
        QStringLiteral(".%1.update").arg(QFileInfo(folder).fileName()));
    auto alive = m_alive;
    QThreadPool::globalInstance()->start([this, alive, archive, staged] {
        QString error;
        const QString unpacked = staged + QStringLiteral(".unpack");
        QDir(unpacked).removeRecursively();
        QDir(staged).removeRecursively();
        if (!QDir().mkpath(unpacked)) {
            error = QStringLiteral("Cannot create %1").arg(QDir::toNativeSeparators(unpacked));
        } else {
            // bsdtar, part of Windows since 10 1803, reads zip archives.
            error = runTool(QStringLiteral("tar"), {QStringLiteral("-xf"), archive,
                                                    QStringLiteral("-C"), unpacked});
        }
        if (error.isEmpty()) {
            const QString app = packagedAppFolder(unpacked);
            if (app.isEmpty()) {
                error = QStringLiteral("The archive holds no GitMycelium.exe.");
            } else if (!QDir().rename(app, staged)) {
                error = QStringLiteral("Cannot stage the update at %1")
                            .arg(QDir::toNativeSeparators(staged));
            }
        }
        QDir(unpacked).removeRecursively();
        if (!error.isEmpty()) {
            QDir(staged).removeRecursively();
        }
        QFile::remove(archive);
        if (!alive->load()) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, error, staged] {
                if (error.isEmpty()) {
                    m_stagedBundle = staged;
                }
                finishInstall(error.isEmpty(), error);
            },
            Qt::QueuedConnection);
    });
}

void Updater::stageMacBundle(const QString& diskImage) {
    const QString bundle = m_target.path;
    const QString staged = QFileInfo(bundle).absoluteDir().filePath(
        QStringLiteral(".%1.update").arg(QFileInfo(bundle).fileName()));
    const QString bundleName = QFileInfo(bundle).fileName();
    auto alive = m_alive;
    QThreadPool::globalInstance()->start([this, alive, diskImage, staged, bundleName] {
        QString error;
        QTemporaryDir mount;
        if (!mount.isValid()) {
            error = QStringLiteral("Cannot create a mount point for the update.");
        } else {
            // A disk image with a license agreement asks to accept it on stdin.
            error = runTool(QStringLiteral("hdiutil"),
                            {QStringLiteral("attach"), QStringLiteral("-nobrowse"),
                             QStringLiteral("-readonly"), QStringLiteral("-noautoopen"),
                             QStringLiteral("-mountpoint"), mount.path(), diskImage},
                            "Y\n");
            if (error.isEmpty()) {
                QDir(staged).removeRecursively();
                const QString source = QDir(mount.path()).filePath(bundleName);
                if (!QFileInfo(source).isDir()) {
                    error = QStringLiteral("The disk image holds no %1.").arg(bundleName);
                } else {
                    error = runTool(QStringLiteral("ditto"), {source, staged});
                }
                runTool(QStringLiteral("hdiutil"),
                        {QStringLiteral("detach"), QStringLiteral("-force"), mount.path()});
            }
            if (error.isEmpty()) {
                error = runTool(QStringLiteral("codesign"),
                                {QStringLiteral("--verify"), QStringLiteral("--deep"),
                                 QStringLiteral("--strict"), staged});
            }
            if (!error.isEmpty()) {
                QDir(staged).removeRecursively();
            }
        }
        QFile::remove(diskImage);
        if (!alive->load()) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, error, staged] {
                if (error.isEmpty()) {
                    m_stagedBundle = staged;
                }
                finishInstall(error.isEmpty(), error);
            },
            Qt::QueuedConnection);
    });
}

void Updater::finishInstall(bool ok, const QString& error) {
    m_busy = false;
    m_installed = ok;
    m_download.reset();
    m_hash.reset();
    emit installFinished(ok, error);
}

bool Updater::restart() {
    if (!m_installed) {
        return false;
    }
    if (m_target.kind == InstallKind::WindowsFolder) {
        const QString script = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                   .filePath(QStringLiteral("gitmycelium-relaunch.ps1"));
        QFile file(script);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(kWindowsRelaunchScript) < 0) {
            return false;
        }
        file.close();
        return QProcess::startDetached(
            QStringLiteral("powershell.exe"),
            {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
             QStringLiteral("Bypass"), QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
             QStringLiteral("-File"), script, QStringLiteral("-ProcessId"),
             QString::number(QCoreApplication::applicationPid()), QStringLiteral("-Target"),
             QDir::toNativeSeparators(m_target.path), QStringLiteral("-Staged"),
             QDir::toNativeSeparators(m_stagedBundle)});
    }
    const QStringList args{QStringLiteral("-c"), QString::fromLatin1(kRelaunchScript),
                           QStringLiteral("gitmycelium-relaunch"),
                           QString::number(QCoreApplication::applicationPid()), m_target.path,
                           m_stagedBundle};
    return QProcess::startDetached(QStringLiteral("/bin/sh"), args);
}

} // namespace gk::update
