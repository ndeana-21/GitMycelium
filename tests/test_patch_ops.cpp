/// "Apply a patch" is two operations: a mailbox carries commits and goes through
/// `git am`, a plain diff belongs in the working tree.

#include "core/types.h"
#include "git/cli.h"
#include "git/maintenance.h"
#include "git/patch_ops.h"
#include "repo/repo_session.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

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

QString gitOut(const QString& dir, const QStringList& args) {
    std::vector<std::string> argv;
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, dir.toStdString()).out);
}

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

const QStringList kIdentity{"-c", "user.name=Test", "-c", "user.email=test@localhost"};

} // namespace

class PatchOpsTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aMailboxIsRecognisedAndMakesItsCommit();
    void aPlainDiffLandsInTheWorkingTreeUnstaged();
    void anEmptyOrBrokenPatchIsRefused();
    void garbageCollectionSucceedsAndFsckIsClean();

private:
    bool commitFile(const QString& name, const QString& content, const QString& message);
    QString treeOf(const QString& rev) const;

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<QTemporaryDir> m_scratch;
    QString m_path;
};

void PatchOpsTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    // Patch files live outside the repository: an untracked *.patch inside it would
    // show up in the status assertions below and hide the thing being tested.
    m_scratch = std::make_unique<QTemporaryDir>();
    QVERIFY(m_scratch->isValid());
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(commitFile(QStringLiteral("a.txt"), QStringLiteral("base\n"),
                       QStringLiteral("base")));
}

void PatchOpsTest::cleanup() {
    m_scratch.reset();
    m_dir.reset();
}

bool PatchOpsTest::commitFile(const QString& name, const QString& content,
                              const QString& message) {
    if (!writeFile(m_path, name, content)) {
        return false;
    }
    if (!git(m_path, {"add", "-A"})) {
        return false;
    }
    QStringList args = kIdentity;
    args << "commit" << "-q" << "-m" << message;
    return git(m_path, args);
}

QString PatchOpsTest::treeOf(const QString& rev) const {
    return gitOut(m_path, {"rev-parse", rev + QStringLiteral("^{tree}")}).trimmed();
}

void PatchOpsTest::aMailboxIsRecognisedAndMakesItsCommit() {
    QVERIFY(commitFile(QStringLiteral("b.txt"), QStringLiteral("second\n"),
                       QStringLiteral("second")));
    const QString tree = treeOf(QStringLiteral("HEAD"));
    const QString author =
        gitOut(m_path, {"log", "-1", "--format=%an <%ae>"}).trimmed();

    // The patch, then the history it came from wound back, so applying it is the
    // only thing that can put the commit back.
    const QString patch = m_scratch->filePath(QStringLiteral("p.patch"));
    QFile out(patch);
    QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
    out.write(gitOut(m_path, {"format-patch", "-1", "--stdout"}).toUtf8());
    out.close();
    QVERIFY(git(m_path, {"reset", "--hard", "-q", "HEAD~1"}));

    QVERIFY(PatchOps::isMailbox(patch.toStdString()));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    bool mailbox = false;
    QVERIFY2(session.applyPatch(patch, &mailbox, &error), qPrintable(error));
    QVERIFY(mailbox);

    // The author, message, tree and parent survive; the committer date is git am's
    // own, so the id is not asserted — the content is what the patch carries.
    QCOMPARE(treeOf(QStringLiteral("HEAD")), tree);
    QCOMPARE(gitOut(m_path, {"log", "-1", "--format=%s"}).trimmed(), QStringLiteral("second"));
    QCOMPARE(gitOut(m_path, {"log", "-1", "--format=%an <%ae>"}).trimmed(), author);
    QCOMPARE(gitOut(m_path, {"rev-list", "--count", "HEAD"}).trimmed(), QStringLiteral("2"));
}

void PatchOpsTest::aPlainDiffLandsInTheWorkingTreeUnstaged() {
    QVERIFY(writeFile(m_path, QStringLiteral("a.txt"), QStringLiteral("changed\n")));
    const QString patch = m_scratch->filePath(QStringLiteral("d.patch"));
    QFile out(patch);
    QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
    out.write(gitOut(m_path, {"diff"}).toUtf8());
    out.close();
    // Back to the committed text; the working tree is clean when the patch is applied.
    QVERIFY(git(m_path, {"checkout", "--", "a.txt"}));
    QVERIFY(gitOut(m_path, {"status", "--porcelain"}).trimmed().isEmpty());

    QVERIFY(!PatchOps::isMailbox(patch.toStdString()));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    bool mailbox = true;
    QVERIFY2(session.applyPatch(patch, &mailbox, &error), qPrintable(error));
    QVERIFY(!mailbox);

    QFile applied(QDir(m_path).filePath(QStringLiteral("a.txt")));
    QVERIFY(applied.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(applied.readAll()), QStringLiteral("changed\n"));

    // A diff is applied to the working tree, not committed and not staged: the
    // reader reviews it and decides. ` M` is the unstaged-modification code.
    QVERIFY(git(m_path, {"diff", "--cached", "--quiet"}));
    QVERIFY(gitOut(m_path, {"status", "--porcelain"}).startsWith(QStringLiteral(" M a.txt")));
}

void PatchOpsTest::anEmptyOrBrokenPatchIsRefused() {
    const QString empty = m_scratch->filePath(QStringLiteral("empty.patch"));
    QFile e(empty);
    QVERIFY(e.open(QIODevice::WriteOnly | QIODevice::Truncate));
    e.close();

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));
    bool mailbox = true;
    QVERIFY(!session.applyPatch(empty, &mailbox, &error));
    QVERIFY(!error.isEmpty());

    const QString broken = m_scratch->filePath(QStringLiteral("broken.patch"));
    QVERIFY(writeFile(m_scratch->path(), QStringLiteral("broken.patch"),
                      QStringLiteral("This is not a patch at all.\n")));
    error.clear();
    QVERIFY(!session.applyPatch(broken, &mailbox, &error));
    QVERIFY(!error.isEmpty());
}

void PatchOpsTest::garbageCollectionSucceedsAndFsckIsClean() {
    QVERIFY(commitFile(QStringLiteral("c.txt"), QStringLiteral("c\n"), QStringLiteral("c")));
    QVERIFY(commitFile(QStringLiteral("d.txt"), QStringLiteral("d\n"), QStringLiteral("d")));

    RepoSession session;
    QString error;
    QVERIFY(session.open(m_path, &error));

    const SyncResult gc = session.maintenanceBlocking(MaintenanceTask::Gc);
    QVERIFY2(gc.ok, gc.output.c_str());

    const SyncResult fsck = session.maintenanceBlocking(MaintenanceTask::Fsck);
    QVERIFY2(fsck.ok, fsck.output.c_str());
}

QTEST_MAIN(PatchOpsTest)
#include "test_patch_ops.moc"
