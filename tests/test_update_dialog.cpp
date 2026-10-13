/// The update dialog says what is installed and what is published, and offers the
/// install only to a copy that can replace itself.

#include "ui/update_dialog.h"
#include "update/updater.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

using namespace gk;

class UpdateDialogTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void aCopyBuiltFromSourceIsToldWhereToDownload();
    void anAppImageIsOfferedTheInstall();

private:
    QLabel* status(UpdateDialog& dialog) { return dialog.findChild<QLabel*>(QStringLiteral("updateStatus")); }
    QPushButton* install(UpdateDialog& dialog) {
        return dialog.findChild<QPushButton*>(QStringLiteral("updateInstall"));
    }

    std::unique_ptr<QTemporaryDir> m_dir;
};

void UpdateDialogTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    const QByteArray payload("new build");
    QFile package(m_dir->filePath(QStringLiteral("package.bin")));
    QVERIFY(package.open(QIODevice::WriteOnly));
    package.write(payload);
    package.close();
    QJsonObject asset{{QStringLiteral("url"), QUrl::fromLocalFile(package.fileName()).toString()},
                      {QStringLiteral("sha256"),
                       QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex())}};
    QJsonObject root{{QStringLiteral("version"), QStringLiteral("99.0.0")},
                     {QStringLiteral("build"), 7},
                     {QStringLiteral("assets"), QJsonObject{{update::platformAssetKey(), asset}}}};
    QFile manifest(m_dir->filePath(QStringLiteral("update.json")));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(root).toJson());
    manifest.close();
    qputenv("GMC_UPDATE_MANIFEST", QUrl::fromLocalFile(manifest.fileName()).toString().toUtf8());
}

void UpdateDialogTest::cleanup() {
    qunsetenv("GMC_UPDATE_MANIFEST");
    m_dir.reset();
}

void UpdateDialogTest::aCopyBuiltFromSourceIsToldWhereToDownload() {
    update::Updater updater;
    updater.setInstallTarget({});
    UpdateDialog dialog(&updater);
    QTRY_VERIFY(status(dialog)->text().contains(QStringLiteral("99.0.0 (build 7)")));
    QVERIFY(status(dialog)->text().contains(QStringLiteral("release page")));
    QVERIFY(!install(dialog)->isEnabled());
}

void UpdateDialogTest::anAppImageIsOfferedTheInstall() {
    if (update::platformAssetKey().isEmpty()) {
        QSKIP("no release package is published for this platform");
    }
    QFile current(m_dir->filePath(QStringLiteral("GitMycelium.AppImage")));
    QVERIFY(current.open(QIODevice::WriteOnly));
    current.close();
    update::Updater updater;
    updater.setInstallTarget({update::InstallKind::AppImage, current.fileName()});
    UpdateDialog dialog(&updater);
    QTRY_VERIFY(status(dialog)->text().contains(QStringLiteral("99.0.0 (build 7)")));
    QVERIFY(install(dialog)->isEnabled());
}

QTEST_MAIN(UpdateDialogTest)
#include "test_update_dialog.moc"
