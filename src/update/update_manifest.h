#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QUrl>

#include <optional>

namespace gk::update {

struct UpdateAsset {
    QUrl url;
    QString sha256;
    qint64 size = 0;
};

/// What the release workflow publishes beside every release: the version, the CI
/// build that produced it, and each platform's package with its checksum.
///
/// The build number is what tells two builds of the same version apart (a release
/// re-run after a fix keeps its version); the version alone cannot.
struct UpdateManifest {
    QString version;
    int build = 0;
    QString tag;
    QUrl notesUrl;
    QHash<QString, UpdateAsset> assets;
};

std::optional<UpdateManifest> parseManifest(const QByteArray& json, QString* error = nullptr);

/// Numeric, component by component: "1.10.0" is newer than "1.9.3". A missing
/// component counts as 0.
int compareVersions(const QString& a, const QString& b);

bool isNewer(const UpdateManifest& manifest, const QString& version, int build);

/// The `assets` key this binary installs from: "linux-x86_64", "macos-arm64", or
/// empty where no package is published.
QString platformAssetKey();

QString currentVersion();
int currentBuild();
QString describeVersion(const QString& version, int build);

/// Where the newest release's manifest lives. `GMC_UPDATE_MANIFEST` replaces it
/// (a test serves a `file://` manifest; a pre-release can be tried the same way).
QUrl manifestUrl();

} // namespace gk::update
