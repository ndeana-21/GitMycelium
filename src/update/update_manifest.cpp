#include "update/update_manifest.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QSysInfo>

namespace gk::update {
namespace {

const QUrl kDefaultManifest(
    QStringLiteral("https://github.com/ndeana-21/GitMycelium/releases/latest/download/update.json"));

bool isSha256(const QString& text) {
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    return hex.match(text).hasMatch();
}

bool isAllowedDownload(const QUrl& url) {
    return url.isValid() && (url.scheme() == QLatin1String("https") || url.isLocalFile());
}

} // namespace

std::optional<UpdateManifest> parseManifest(const QByteArray& json, QString* error) {
    auto fail = [error](const QString& why) -> std::optional<UpdateManifest> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return fail(QStringLiteral("the update manifest is not JSON: %1").arg(parseError.errorString()));
    }
    const QJsonObject root = doc.object();

    UpdateManifest manifest;
    manifest.version = root.value(QStringLiteral("version")).toString();
    manifest.build = root.value(QStringLiteral("build")).toInt();
    manifest.tag = root.value(QStringLiteral("tag")).toString();
    manifest.notesUrl = QUrl(root.value(QStringLiteral("notes")).toString());
    static const QRegularExpression versionShape(QStringLiteral("^\\d+(\\.\\d+)*$"));
    if (!versionShape.match(manifest.version).hasMatch()) {
        return fail(QStringLiteral("the update manifest names no version"));
    }

    const QJsonObject assets = root.value(QStringLiteral("assets")).toObject();
    for (auto it = assets.begin(); it != assets.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        UpdateAsset asset;
        asset.url = QUrl(entry.value(QStringLiteral("url")).toString());
        asset.sha256 = entry.value(QStringLiteral("sha256")).toString().toLower();
        asset.size = entry.value(QStringLiteral("size")).toInteger();
        if (!isAllowedDownload(asset.url) || !isSha256(asset.sha256)) {
            return fail(QStringLiteral("the update manifest's %1 entry is malformed").arg(it.key()));
        }
        manifest.assets.insert(it.key(), asset);
    }
    return manifest;
}

int compareVersions(const QString& a, const QString& b) {
    const QStringList left = a.split(QLatin1Char('.'));
    const QStringList right = b.split(QLatin1Char('.'));
    const qsizetype count = std::max(left.size(), right.size());
    for (qsizetype i = 0; i < count; ++i) {
        const int l = i < left.size() ? left[i].toInt() : 0;
        const int r = i < right.size() ? right[i].toInt() : 0;
        if (l != r) {
            return l < r ? -1 : 1;
        }
    }
    return 0;
}

bool isNewer(const UpdateManifest& manifest, const QString& version, int build) {
    const int order = compareVersions(manifest.version, version);
    return order > 0 || (order == 0 && manifest.build > build);
}

QString platformAssetKey() {
#if defined(Q_OS_MACOS)
    return QSysInfo::currentCpuArchitecture() == QLatin1String("arm64") ? QStringLiteral("macos-arm64")
                                                                          : QString();
#elif defined(Q_OS_LINUX)
    return QSysInfo::currentCpuArchitecture() == QLatin1String("x86_64")
               ? QStringLiteral("linux-x86_64")
               : QString();
#else
    return {};
#endif
}

QString currentVersion() {
    return QStringLiteral(GMC_VERSION);
}

int currentBuild() {
    return GMC_BUILD_NUMBER;
}

QString describeVersion(const QString& version, int build) {
    return build > 0 ? QStringLiteral("%1 (build %2)").arg(version).arg(build) : version;
}

QUrl manifestUrl() {
    const QString overridden = qEnvironmentVariable("GMC_UPDATE_MANIFEST");
    if (overridden.isEmpty()) {
        return kDefaultManifest;
    }
    return QUrl::fromUserInput(overridden);
}

} // namespace gk::update
