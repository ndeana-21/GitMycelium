/// Authors' pictures: where each address is asked for, a picture fetched once and kept
/// on disk, a miss that is not asked again, nothing at all while turned off, and the
/// picture drawn inside a commit's node.

#include "git/cli.h"
#include "model/commit_model.h"
#include "repo/repo_session.h"
#include "theme/theme.h"
#include "ui/avatar_store.h"
#include "ui/graph_delegate.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QPainter>
#include <QSignalSpy>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QtTest>

using namespace gk;

namespace {

bool git(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, dir.toStdString()).exitCode == 0;
}

} // namespace

class AvatarsTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void eachAddressIsAskedWhereItsPictureLives();
    void aPictureIsFetchedOnceAndKept();
    void aMissIsRememberedAndOffDrawsNothing();
    void theNodeShowsThePicture();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_picture;
    QStringList m_asked;
};

void AvatarsTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_picture = m_dir->filePath(QStringLiteral("red.png"));
    QImage red(64, 64, QImage::Format_ARGB32);
    red.fill(QColor(220, 20, 20));
    QVERIFY(red.save(m_picture));
    m_asked.clear();
    AvatarStore& store = AvatarStore::instance();
    store.clearForTests();
    store.setCacheDirectory(m_dir->filePath(QStringLiteral("cache")));
    store.setSourceForTests([this](const QString& email) {
        m_asked << email;
        return email == QStringLiteral("dev@example.com")
                   ? QUrl::fromLocalFile(m_picture)
                   : QUrl::fromLocalFile(m_dir->filePath(QStringLiteral("absent.png")));
    });
    store.setEnabled(true);
}

void AvatarsTest::cleanup() {
    AvatarStore::instance().setEnabled(false);
    AvatarStore::instance().clearForTests();
    m_dir.reset();
}

void AvatarsTest::eachAddressIsAskedWhereItsPictureLives() {
    QCOMPARE(AvatarStore::sourceFor(QStringLiteral("64661846+ndeana-21@users.noreply.github.com"), 96),
             QUrl(QStringLiteral("https://avatars.githubusercontent.com/u/64661846?s=96&v=4")));
    QCOMPARE(AvatarStore::sourceFor(QStringLiteral("octocat@users.noreply.github.com"), 96),
             QUrl(QStringLiteral("https://github.com/octocat.png?size=96")));
    // Gravatar is sent a hash of the trimmed, lower-cased address, nothing else.
    const QByteArray hash =
        QCryptographicHash::hash("dev@example.com", QCryptographicHash::Sha256).toHex();
    QCOMPARE(AvatarStore::sourceFor(QStringLiteral(" Dev@Example.com "), 80),
             QUrl(QStringLiteral("https://www.gravatar.com/avatar/%1?s=80&d=404")
                      .arg(QString::fromLatin1(hash))));
    QVERIFY(AvatarStore::sourceFor(QStringLiteral("no address"), 80).isEmpty());
}

void AvatarsTest::aPictureIsFetchedOnceAndKept() {
    AvatarStore& store = AvatarStore::instance();
    QSignalSpy ready(&store, &AvatarStore::avatarReady);
    QVERIFY(store.avatar(QStringLiteral("Dev@example.com"), 20, 2.0).isNull());
    QTRY_COMPARE(ready.count(), 1);
    const QPixmap picture = store.avatar(QStringLiteral("dev@example.com"), 20, 2.0);
    QVERIFY(!picture.isNull());
    QCOMPARE(picture.size(), QSize(40, 40));
    QCOMPARE(picture.devicePixelRatio(), 2.0);
    // Round: the corner is clear, the middle is the picture.
    const QImage image = picture.toImage();
    QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(image.pixelColor(20, 20).red(), 220);

    // Another session reads it from disk and asks nobody.
    store.clearForTests();
    m_asked.clear();
    QVERIFY(!store.avatar(QStringLiteral("dev@example.com"), 20, 1.0).isNull());
    QVERIFY(m_asked.isEmpty());
}

void AvatarsTest::aMissIsRememberedAndOffDrawsNothing() {
    AvatarStore& store = AvatarStore::instance();
    QVERIFY(store.avatar(QStringLiteral("nobody@example.com"), 20, 1.0).isNull());
    QTRY_COMPARE(m_asked.size(), 1);
    QTest::qWait(50);
    QVERIFY(store.avatar(QStringLiteral("nobody@example.com"), 20, 1.0).isNull());
    QCOMPARE(m_asked.size(), 1);

    store.setEnabled(false);
    QVERIFY(store.avatar(QStringLiteral("dev@example.com"), 20, 1.0).isNull());
    QCOMPARE(m_asked.size(), 1);
}

void AvatarsTest::theNodeShowsThePicture() {
    const QString repo = m_dir->filePath(QStringLiteral("repo"));
    QVERIFY(QDir().mkpath(repo));
    QVERIFY(git(repo, {"init", "-q", "-b", "main"}));
    QVERIFY(git(repo, {"-c", "user.name=Dev", "-c", "user.email=dev@example.com", "commit",
                       "--allow-empty", "-qm", "first"}));
    RepoSession session;
    QVERIFY(session.open(repo));
    CommitModel model(&session);
    model.reset();
    QTRY_VERIFY(model.realRowCount() == 1 && !model.isLoading());

    const auto paintNode = [&model] {
        const QRect cell(0, 0, 60, 30);
        QImage image(cell.size(), QImage::Format_ARGB32);
        image.fill(theme::background());
        QPainter painter(&image);
        GraphDelegate delegate;
        QStyleOptionViewItem option;
        option.rect = cell;
        option.font = QApplication::font();
        delegate.paint(&painter, option, model.index(0, CommitModel::Graph));
        painter.end();
        return image.pixelColor(GraphDelegate::lanePadding(), cell.center().y());
    };
    AvatarStore::instance().avatar(QStringLiteral("dev@example.com"), 10, 1.0);
    QTRY_VERIFY(!AvatarStore::instance().avatar(QStringLiteral("dev@example.com"), 10, 1.0).isNull());
    const QColor centre = paintNode();
    QVERIFY2(centre.red() > 180 && centre.green() < 80, qPrintable(centre.name()));

    AvatarStore::instance().setEnabled(false);
    const QColor initials = paintNode();
    QVERIFY2(initials != centre, qPrintable(initials.name()));
}

QTEST_MAIN(AvatarsTest)
#include "test_avatars.moc"
