#include "git/cli.h"
#include "model/commit_model.h"
#include "model/date_format.h"
#include "repo/repo_session.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace gk;

namespace {

bool git(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return runGit(argv, dir.toStdString()).exitCode == 0;
}

bool writeFile(const QString& path, const QString& content) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content.toUtf8()) == content.toUtf8().size();
}

} // namespace

class DateFormatTest : public QObject {
    Q_OBJECT

private slots:
    void theAbsoluteFormatsSpellTheSameInstant();
    void relativeIsMeasuredAgainstNow();
    void theKeyRoundTripsAndFallsBack();
    void theModelDateColumnFollowsTheFormat();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void DateFormatTest::theAbsoluteFormatsSpellTheSameInstant() {
    const QDateTime when(QDate(2026, 1, 31), QTime(14, 5));
    QCOMPARE(formatCommitDate(when, CommitDateFormat::Iso), QStringLiteral("2026-01-31 14:05"));
    QCOMPARE(formatCommitDate(when, CommitDateFormat::European),
             QStringLiteral("31.01.2026 14:05"));
    QCOMPARE(formatCommitDate(when, CommitDateFormat::Us), QStringLiteral("01/31/2026 2:05 PM"));
}

void DateFormatTest::relativeIsMeasuredAgainstNow() {
    const QDateTime now = QDateTime::currentDateTime();
    QCOMPARE(formatCommitDate(now.addSecs(-10), CommitDateFormat::Relative),
             QStringLiteral("just now"));
    QCOMPARE(formatCommitDate(now.addSecs(-5 * 60), CommitDateFormat::Relative),
             QStringLiteral("5 min ago"));
    QCOMPARE(formatCommitDate(now.addSecs(-3 * 3600), CommitDateFormat::Relative),
             QStringLiteral("3 h ago"));
    QCOMPARE(formatCommitDate(now.addDays(-5), CommitDateFormat::Relative),
             QStringLiteral("5 d ago"));
}

void DateFormatTest::theKeyRoundTripsAndFallsBack() {
    for (const CommitDateFormat format :
         {CommitDateFormat::Iso, CommitDateFormat::European, CommitDateFormat::Us,
          CommitDateFormat::Relative}) {
        QCOMPARE(commitDateFormatFromKey(commitDateFormatKey(format)), format);
    }
    QCOMPARE(commitDateFormatFromKey(QStringLiteral("martian"), CommitDateFormat::Us),
             CommitDateFormat::Us);
}

void DateFormatTest::theModelDateColumnFollowsTheFormat() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(m_path, {"config", "user.name", "Test"}));
    QVERIFY(git(m_path, {"config", "user.email", "test@localhost"}));
    QVERIFY(writeFile(m_path + "/a.txt", QStringLiteral("a\n")));
    QVERIFY(git(m_path, {"add", "a.txt"}));
    QVERIFY(git(m_path, {"commit", "-qm", "base"}));

    RepoSession session;
    QVERIFY(session.open(m_path));
    CommitModel model(&session);
    model.reset();
    QTRY_COMPARE(model.rowCount(), 1);

    const QModelIndex date = model.index(0, CommitModel::Date);
    const QString iso = model.data(date, Qt::DisplayRole).toString();
    QVERIFY2(QRegularExpression(QStringLiteral(R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}$)"))
                 .match(iso)
                 .hasMatch(),
             qPrintable(iso));

    model.setCommitDateFormat(CommitDateFormat::Us);
    const QString us = model.data(date, Qt::DisplayRole).toString();
    QVERIFY2(QRegularExpression(QStringLiteral(R"(^\d{2}/\d{2}/\d{4} \d{1,2}:\d{2} [AP]M$)"))
                 .match(us)
                 .hasMatch(),
             qPrintable(us));
    QVERIFY(iso != us);

    m_dir.reset();
}

QTEST_MAIN(DateFormatTest)
#include "test_date_format.moc"
