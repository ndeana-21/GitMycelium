/// The updater replaces the running copy only with a download that matches the
/// release's checksum. Served over file:// so no test touches the network.

#include "update/update_manifest.h"
#include "update/updater.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace gk::update;

class UpdaterTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void versionsCompareNumerically();
    void aLaterBuildOfTheSameVersionIsNewer();
    void aManifestWithABadChecksumOrPlainHttpIsRefused();
    void anAppImageIsReplacedByAVerifiedDownload();
    void aMismatchedDownloadReplacesNothing();
    void aCopyBuiltFromSourceDoesNotInstall();

private:
    QString writeFile(const QString& path, const QByteArray& bytes);
    void serveManifest(const QByteArray& payload, const QString& sha256);
    bool runCheck(Updater& updater);
    QString runInstall(Updater& updater);

    std::unique_ptr<QTemporaryDir> m_install;
    std::unique_ptr<QTemporaryDir> m_release;
    QString m_appImage;
};

void UpdaterTest::init() {
    m_install = std::make_unique<QTemporaryDir>();
    m_release = std::make_unique<QTemporaryDir>();
    QVERIFY(m_install->isValid() && m_release->isValid());
    m_appImage = writeFile(m_install->filePath(QStringLiteral("GitMycelium.AppImage")), "old build");
    QFile::setPermissions(m_appImage, QFile::permissions(m_appImage) | QFileDevice::ExeOwner);
}

void UpdaterTest::cleanup() {
    qunsetenv("GMC_UPDATE_MANIFEST");
    m_install.reset();
    m_release.reset();
}

QString UpdaterTest::writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {};
    }
    file.write(bytes);
    return path;
}

void UpdaterTest::serveManifest(const QByteArray& payload, const QString& sha256) {
    const QString package = writeFile(m_release->filePath(QStringLiteral("package.bin")), payload);
    QJsonObject asset{{QStringLiteral("url"), QUrl::fromLocalFile(package).toString()},
                      {QStringLiteral("sha256"), sha256},
                      {QStringLiteral("size"), payload.size()}};
    QJsonObject assets{{platformAssetKey(), asset}};
    QJsonObject root{{QStringLiteral("version"), QStringLiteral("99.0.0")},
                     {QStringLiteral("build"), 7},
                     {QStringLiteral("tag"), QStringLiteral("v99.0.0")},
                     {QStringLiteral("assets"), assets}};
    const QString manifest = writeFile(m_release->filePath(QStringLiteral("update.json")),
                                       QJsonDocument(root).toJson());
    qputenv("GMC_UPDATE_MANIFEST", QUrl::fromLocalFile(manifest).toString().toUtf8());
}

bool UpdaterTest::runCheck(Updater& updater) {
    QSignalSpy spy(&updater, &Updater::checked);
    updater.check();
    if (!spy.wait(5000)) {
        return false;
    }
    return spy.first().at(0).toBool();
}

QString UpdaterTest::runInstall(Updater& updater) {
    QSignalSpy spy(&updater, &Updater::installFinished);
    updater.install();
    if (spy.isEmpty() && !spy.wait(5000)) {
        return QStringLiteral("no answer");
    }
    return spy.first().at(0).toBool() ? QString() : spy.first().at(1).toString();
}

void UpdaterTest::versionsCompareNumerically() {
    QCOMPARE(compareVersions(QStringLiteral("1.10.0"), QStringLiteral("1.9.3")), 1);
    QCOMPARE(compareVersions(QStringLiteral("1.0"), QStringLiteral("1.0.0")), 0);
    QCOMPARE(compareVersions(QStringLiteral("1.0.0"), QStringLiteral("1.0.1")), -1);
}

void UpdaterTest::aLaterBuildOfTheSameVersionIsNewer() {
    UpdateManifest manifest;
    manifest.version = QStringLiteral("1.1.0");
    manifest.build = 12;
    QVERIFY(isNewer(manifest, QStringLiteral("1.1.0"), 11));
    QVERIFY(!isNewer(manifest, QStringLiteral("1.1.0"), 12));
    QVERIFY(!isNewer(manifest, QStringLiteral("1.2.0"), 1));
    QVERIFY(isNewer(manifest, QStringLiteral("1.0.0"), 40));
}

void UpdaterTest::aManifestWithABadChecksumOrPlainHttpIsRefused() {
    const QString sha(64, QLatin1Char('a'));
    auto manifestWith = [](const QString& url, const QString& checksum) {
        QJsonObject asset{{QStringLiteral("url"), url}, {QStringLiteral("sha256"), checksum}};
        QJsonObject root{{QStringLiteral("version"), QStringLiteral("1.2.0")},
                         {QStringLiteral("assets"), QJsonObject{{QStringLiteral("x"), asset}}}};
        return QJsonDocument(root).toJson();
    };
    QVERIFY(parseManifest(manifestWith(QStringLiteral("https://example.org/a"), sha)));
    QVERIFY(!parseManifest(manifestWith(QStringLiteral("http://example.org/a"), sha)));
    QVERIFY(!parseManifest(manifestWith(QStringLiteral("https://example.org/a"), sha.left(10))));
    QVERIFY(!parseManifest("{\"version\":\"latest\"}"));
    QVERIFY(!parseManifest("not json"));
}

void UpdaterTest::anAppImageIsReplacedByAVerifiedDownload() {
    if (platformAssetKey().isEmpty()) {
        QSKIP("no release package is published for this platform");
    }
    const QByteArray payload("new build");
    serveManifest(payload,
                  QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()));

    Updater updater;
    updater.setInstallTarget({InstallKind::AppImage, m_appImage});
    QVERIFY(runCheck(updater));
    QVERIFY(updater.updateAvailable());
    QCOMPARE(updater.installRefusal(), QString());
    QCOMPARE(runInstall(updater), QString());

    QFile installed(m_appImage);
    QVERIFY(installed.open(QIODevice::ReadOnly));
    QCOMPARE(installed.readAll(), payload);
    QVERIFY(installed.permissions() & QFileDevice::ExeOwner);
    QCOMPARE(QDir(m_install->path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
}

void UpdaterTest::aMismatchedDownloadReplacesNothing() {
    if (platformAssetKey().isEmpty()) {
        QSKIP("no release package is published for this platform");
    }
    serveManifest("tampered build", QString(64, QLatin1Char('0')));

    Updater updater;
    updater.setInstallTarget({InstallKind::AppImage, m_appImage});
    QVERIFY(runCheck(updater));
    QVERIFY(runInstall(updater).contains(QStringLiteral("sha256")));
    QVERIFY(!updater.installed());

    QFile kept(m_appImage);
    QVERIFY(kept.open(QIODevice::ReadOnly));
    QCOMPARE(kept.readAll(), QByteArray("old build"));
    QCOMPARE(QDir(m_install->path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
}

void UpdaterTest::aCopyBuiltFromSourceDoesNotInstall() {
    serveManifest("new build", QString(64, QLatin1Char('0')));
    Updater updater;
    updater.setInstallTarget({});
    QVERIFY(runCheck(updater));
    QVERIFY(updater.updateAvailable());
    QVERIFY(updater.installRefusal().contains(QStringLiteral("release package")));
    QVERIFY(!runInstall(updater).isEmpty());
}

QTEST_GUILESS_MAIN(UpdaterTest)
#include "test_updater.moc"
