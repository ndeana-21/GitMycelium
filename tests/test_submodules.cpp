#include "core/types.h"
#include "git/cli.h"
#include "repo/repo_session.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

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
    argv.reserve(static_cast<std::size_t>(args.size()));
    for (const QString& a : args) {
        argv.push_back(a.toStdString());
    }
    return QString::fromStdString(runGit(argv, dir.toStdString()).out).trimmed();
}

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

const SubmoduleEntry* findSubmodule(const std::vector<SubmoduleEntry>& list, const QString& path) {
    for (const SubmoduleEntry& entry : list) {
        if (QString::fromStdString(entry.path) == path) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

/// Submodules, against real repositories. Every remote here is a local repository
/// path, so nothing touches the network; the transport allowance a local clone
/// needs is set for this process only, because the product must not weaken git's
/// own `transport 'file' not allowed` guard.
class SubmoduleTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void addingRecordsTheSubmodule();
    void noSubmodulesIsAnEmptyList();
    void aDottedNameParses();
    void deinitializingAndInitializingRoundTrips();
    void deinitRefusesADirtySubmoduleWithoutForce();
    void updateToRemoteMovesTheCheckoutAndLeavesThePointerStale();
    void syncCopiesTheUrlIntoTheConfig();
    void aCloneStartsWithNothingInitialized();
    void aNestedSubmoduleIsListedAndRecursivelyInitialized();
    void anOutOfSyncSubmoduleShowsAsAPointerChangeAndStagingRecordsIt();
    void aSubmodulesOwnDirtIsNotAPointerChange();
    void theAutoUpdateHookRestoresTheRecordedPointer();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_parent;
    QString m_lib;
    QString m_inner;

    void makeRepo(const QString& path);
};

void SubmoduleTest::makeRepo(const QString& path) {
    QVERIFY(git(path, {"init", "-q", "-b", "main"}));
    QVERIFY(git(path, {"config", "user.name", "Test"}));
    QVERIFY(git(path, {"config", "user.email", "test@localhost"}));
    QVERIFY(git(path, {"config", "protocol.file.allow", "always"}));
    QVERIFY(writeFile(path, "a.txt", "base\n"));
    QVERIFY(git(path, {"add", "-A"}));
    QVERIFY(git(path, {"commit", "-q", "-m", "base"}));
}

void SubmoduleTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    // A local path is the `file` transport, which git refuses for submodules
    // unless it is allowed. Allow it for this process only — the product does not
    // pass the flag, so this is the one thing a test has to arrange.
    qputenv("GIT_ALLOW_PROTOCOL", "file");

    m_parent = QDir(m_dir->path()).filePath(QStringLiteral("parent"));
    m_lib = QDir(m_dir->path()).filePath(QStringLiteral("libsrc"));
    m_inner = QDir(m_dir->path()).filePath(QStringLiteral("innersrc"));

    QVERIFY(QDir().mkpath(m_parent));
    QVERIFY(QDir().mkpath(m_lib));
    QVERIFY(QDir().mkpath(m_inner));
    makeRepo(m_parent);
    makeRepo(m_lib);
    makeRepo(m_inner);

    QVERIFY(git(m_lib, {"submodule", "add", "-q", m_inner, "inner"}));
    QVERIFY(git(m_lib, {"commit", "-q", "-m", "add inner"}));
}

void SubmoduleTest::cleanup() {
    qunsetenv("GIT_ALLOW_PROTOCOL");
    m_dir.reset();
}

void SubmoduleTest::addingRecordsTheSubmodule() {
    RepoSession session;
    QVERIFY(session.open(m_parent));
    QString error;
    QVERIFY2(session.addSubmodule(m_lib, QStringLiteral("lib"), QStringLiteral("main"), false,
                                  &error),
             qPrintable(error));

    const std::vector<SubmoduleEntry> list = session.submodules();
    QCOMPARE(list.size(), std::size_t(2));
    const SubmoduleEntry* found = findSubmodule(list, QStringLiteral("lib"));
    QVERIFY(found);
    const SubmoduleEntry& entry = *found;
    QCOMPARE(QString::fromStdString(entry.name), QStringLiteral("lib"));
    QCOMPARE(QString::fromStdString(entry.path), QStringLiteral("lib"));
    QCOMPARE(QString::fromStdString(entry.url), m_lib);
    QCOMPARE(QString::fromStdString(entry.branch), QStringLiteral("main"));
    QVERIFY(entry.initialized);
    QCOMPARE(entry.state, SubmoduleState::InSync);
    QCOMPARE(entry.checkedOutOid, entry.recordedOid);
    QCOMPARE(QString::fromStdString(entry.recordedOid), gitOut(m_lib, {"rev-parse", "HEAD"}));
}

void SubmoduleTest::noSubmodulesIsAnEmptyList() {
    RepoSession session;
    QVERIFY(session.open(m_parent));
    QVERIFY(session.submodules().empty());
}

void SubmoduleTest::aDottedNameParses() {
    // A submodule name may contain a dot; the `.gitmodules` key is
    // `submodule.sub.lib.path`, so the field is the *last* dotted component.
    QVERIFY(git(m_parent, {"submodule", "add", "--name", "sub.lib", m_lib, "subdir"}));
    QVERIFY(git(m_parent, {"commit", "-q", "-m", "dotted"}));

    RepoSession session;
    QVERIFY(session.open(m_parent));
    const std::vector<SubmoduleEntry> list = session.submodules();
    const SubmoduleEntry* entry = findSubmodule(list, QStringLiteral("subdir"));
    QVERIFY(entry);
    QCOMPARE(QString::fromStdString(entry->name), QStringLiteral("sub.lib"));
    QCOMPARE(QString::fromStdString(entry->path), QStringLiteral("subdir"));
    QCOMPARE(QString::fromStdString(entry->url), m_lib);
}

void SubmoduleTest::deinitializingAndInitializingRoundTrips() {
    RepoSession session;
    QVERIFY(session.open(m_parent));
    QString error;
    QVERIFY(session.addSubmodule(m_lib, QStringLiteral("lib"), QString(), false, &error));
    QVERIFY(git(m_parent, {"commit", "-q", "-m", "add lib"}));

    QVERIFY2(session.deinitSubmodule(QStringLiteral("lib"), false, &error), qPrintable(error));
    const std::vector<SubmoduleEntry> afterDeinitList = session.submodules();
    const SubmoduleEntry* afterDeinit = findSubmodule(afterDeinitList, QStringLiteral("lib"));
    QVERIFY(afterDeinit);
    QCOMPARE(afterDeinit->state, SubmoduleState::NotInitialized);
    QVERIFY(!QDir(QDir(m_parent).filePath(QStringLiteral("lib/a.txt"))).exists());

    // `update --init` is what a fresh clone needs; `init` alone only registers.
    QVERIFY2(session.updateSubmodules({QStringLiteral("lib")}, true, false, false, &error),
             qPrintable(error));
    const std::vector<SubmoduleEntry> afterInitList = session.submodules();
    const SubmoduleEntry* afterInit = findSubmodule(afterInitList, QStringLiteral("lib"));
    QVERIFY(afterInit);
    QCOMPARE(afterInit->state, SubmoduleState::InSync);
}

void SubmoduleTest::deinitRefusesADirtySubmoduleWithoutForce() {
    RepoSession session;
    QVERIFY(session.open(m_parent));
    QString error;
    QVERIFY(session.addSubmodule(m_lib, QStringLiteral("lib"), QString(), false, &error));
    QVERIFY(git(m_parent, {"commit", "-q", "-m", "add lib"}));

    QVERIFY(writeFile(QDir(m_parent).filePath(QStringLiteral("lib")), QStringLiteral("a.txt"),
                      QStringLiteral("dirty\n")));
    QVERIFY(!session.deinitSubmodule(QStringLiteral("lib"), false, &error));
    const std::vector<SubmoduleEntry> stillThere = session.submodules();
    QVERIFY(findSubmodule(stillThere, QStringLiteral("lib"))->initialized);

    QVERIFY2(session.deinitSubmodule(QStringLiteral("lib"), true, &error), qPrintable(error));
    const std::vector<SubmoduleEntry> afterForce = session.submodules();
    QCOMPARE(findSubmodule(afterForce, QStringLiteral("lib"))->state,
             SubmoduleState::NotInitialized);
}

void SubmoduleTest::updateToRemoteMovesTheCheckoutAndLeavesThePointerStale() {
    RepoSession session;
    QVERIFY(session.open(m_parent));
    QString error;
    QVERIFY(session.addSubmodule(m_lib, QStringLiteral("lib"), QStringLiteral("main"), false,
                                 &error));
    QVERIFY(git(m_parent, {"commit", "-q", "-m", "add lib"}));
    const QString recorded = gitOut(m_parent, {"rev-parse", "HEAD:lib"});

    // Move libsrc's branch on. `update --remote` fetches it and checks it out; the
    // parent's gitlink still records the old commit until it is committed.
    QVERIFY(writeFile(m_lib, QStringLiteral("b.txt"), QStringLiteral("new\n")));
    QVERIFY(git(m_lib, {"add", "-A"}));
    QVERIFY(git(m_lib, {"commit", "-q", "-m", "advance"}));
    const QString moved = gitOut(m_lib, {"rev-parse", "HEAD"});
    QVERIFY(moved != recorded);

    QVERIFY2(session.updateSubmodules({QStringLiteral("lib")}, true, true, false, &error),
             qPrintable(error));
    const std::vector<SubmoduleEntry> updated = session.submodules();
    const SubmoduleEntry* entry = findSubmodule(updated, QStringLiteral("lib"));
    QVERIFY(entry);
    QCOMPARE(QString::fromStdString(entry->checkedOutOid), moved);
    QCOMPARE(QString::fromStdString(entry->recordedOid), recorded);
    QCOMPARE(entry->state, SubmoduleState::OutOfSync);

    QVERIFY(git(m_parent, {"add", "lib"}));
    QVERIFY(git(m_parent, {"commit", "-q", "-m", "update pointer"}));
    const std::vector<SubmoduleEntry> committed = session.submodules();
    QCOMPARE(findSubmodule(committed, QStringLiteral("lib"))->state, SubmoduleState::InSync);
}

void SubmoduleTest::syncCopiesTheUrlIntoTheConfig() {
    RepoSession session;
    QVERIFY(session.open(m_parent));
    QString error;
    QVERIFY(session.addSubmodule(m_lib, QStringLiteral("lib"), QString(), false, &error));
    QVERIFY(git(m_parent, {"commit", "-q", "-m", "add lib"}));

    const QString other = QStringLiteral("https://example.com/elsewhere.git");
    QVERIFY(git(m_parent, {"config", "-f", ".gitmodules", "submodule.lib.url", other}));
    QVERIFY2(session.syncSubmodules({QStringLiteral("lib")}, false, &error), qPrintable(error));
    QCOMPARE(gitOut(m_parent, {"config", "-f", ".git/config", "submodule.lib.url"}), other);
}

void SubmoduleTest::aCloneStartsWithNothingInitialized() {
    RepoSession session;
    QVERIFY(session.open(m_parent));
    QString error;
    QVERIFY(session.addSubmodule(m_lib, QStringLiteral("lib"), QString(), false, &error));
    QVERIFY(git(m_parent, {"commit", "-q", "-m", "add lib"}));

    const QString clone = QDir(m_dir->path()).filePath(QStringLiteral("clone"));
    QVERIFY(git(m_dir->path(), {"clone", "-q", m_parent, clone}));

    RepoSession cloned;
    QVERIFY(cloned.open(clone));
    const std::vector<SubmoduleEntry> before = cloned.submodules();
    const SubmoduleEntry* entry = findSubmodule(before, QStringLiteral("lib"));
    QVERIFY(entry);
    QCOMPARE(entry->state, SubmoduleState::NotInitialized);
    QVERIFY(!entry->initialized);

    QVERIFY2(cloned.updateSubmodules({QStringLiteral("lib")}, true, false, true, &error),
             qPrintable(error));
    const std::vector<SubmoduleEntry> after = cloned.submodules();
    QCOMPARE(findSubmodule(after, QStringLiteral("lib"))->state, SubmoduleState::InSync);
}

void SubmoduleTest::aNestedSubmoduleIsListedAndRecursivelyInitialized() {
    RepoSession session;
    QVERIFY(session.open(m_parent));
    QString error;
    QVERIFY(session.addSubmodule(m_lib, QStringLiteral("lib"), QString(), false, &error));
    QVERIFY(git(m_parent, {"commit", "-q", "-m", "add lib"}));

    const std::vector<SubmoduleEntry> beforeList = session.submodules();
    const SubmoduleEntry* before = findSubmodule(beforeList, QStringLiteral("lib/inner"));
    QVERIFY(before);
    QVERIFY(before->nested);
    QCOMPARE(QString::fromStdString(before->parentPath), QStringLiteral("lib"));
    QCOMPARE(QString::fromStdString(before->url), m_inner);
    QCOMPARE(before->state, SubmoduleState::NotInitialized);

    QVERIFY2(session.updateSubmodules({}, true, false, true, &error), qPrintable(error));
    const std::vector<SubmoduleEntry> list = session.submodules();
    QCOMPARE(list.size(), std::size_t(2));
    const SubmoduleEntry* inner = findSubmodule(list, QStringLiteral("lib/inner"));
    QVERIFY(inner);
    QVERIFY(inner->nested);
    QVERIFY(inner->initialized);
    QCOMPARE(inner->state, SubmoduleState::InSync);
    // A submodule's `.git` is a file pointing at the superproject's modules dir.
    QVERIFY(QFileInfo(QDir(m_parent).filePath(QStringLiteral("lib/inner/.git"))).exists());
}

void SubmoduleTest::anOutOfSyncSubmoduleShowsAsAPointerChangeAndStagingRecordsIt() {
    RepoSession session;
    QVERIFY(session.open(m_parent));
    QString error;
    QVERIFY(session.addSubmodule(m_lib, QStringLiteral("lib"), QStringLiteral("main"), false,
                                 &error));
    QVERIFY(git(m_parent, {"commit", "-q", "-m", "add lib"}));
    const QString recorded = gitOut(m_parent, {"rev-parse", "HEAD:lib"});

    QVERIFY(writeFile(m_lib, QStringLiteral("b.txt"), QStringLiteral("new\n")));
    QVERIFY(git(m_lib, {"add", "-A"}));
    QVERIFY(git(m_lib, {"commit", "-q", "-m", "advance"}));
    const QString moved = gitOut(m_lib, {"rev-parse", "HEAD"});
    QVERIFY2(session.updateSubmodules({QStringLiteral("lib")}, true, true, false, &error),
             qPrintable(error));

    const auto findLib = [](const std::vector<FileChange>& changes) -> const FileChange* {
        for (const FileChange& change : changes) {
            if (change.path == "lib") {
                return &change;
            }
        }
        return nullptr;
    };
    // A pointer into a by-value `status()` temporary would dangle; bind the vector.
    std::vector<FileChange> status = session.status();
    const FileChange* unstaged = findLib(status);
    QVERIFY(unstaged);
    QVERIFY(unstaged->submodule);
    QVERIFY(!unstaged->staged);
    QCOMPARE(QString::fromStdString(unstaged->recordedOid), recorded);
    QCOMPARE(QString::fromStdString(unstaged->checkedOutOid), moved);

    // `.gitmodules` is an ordinary file: a URL change is a normal modification, not
    // a gitlink, and the two must not be confused.
    const QString elsewhere = QStringLiteral("https://example.com/elsewhere.git");
    QVERIFY(git(m_parent, {"config", "-f", ".gitmodules", "submodule.lib.url", elsewhere}));
    bool sawGitmodules = false;
    for (const FileChange& change : session.status()) {
        if (change.path == ".gitmodules") {
            sawGitmodules = true;
            QVERIFY(!change.submodule);
        }
    }
    QVERIFY(sawGitmodules);

    QVERIFY(git(m_parent, {"config", "-f", ".gitmodules", "submodule.lib.url", m_lib}));

    QVERIFY2(session.stageFile(QStringLiteral("lib"), &error), qPrintable(error));
    status = session.status();
    const FileChange* staged = findLib(status);
    QVERIFY(staged);
    QVERIFY(staged->submodule);
    QVERIFY(staged->staged);

    QVERIFY(session.createCommit(QStringLiteral("update pointer"), nullptr, &error));
    QCOMPARE(findSubmodule(session.submodules(), QStringLiteral("lib"))->state,
             SubmoduleState::InSync);
}

void SubmoduleTest::aSubmodulesOwnDirtIsNotAPointerChange() {
    RepoSession session;
    QVERIFY(session.open(m_parent));
    QString error;
    QVERIFY(session.addSubmodule(m_lib, QStringLiteral("lib"), QString(), false, &error));
    QVERIFY(git(m_parent, {"commit", "-q", "-m", "add lib"}));

    // An untracked file inside a submodule whose checkout matches the recorded
    // commit. The panel is about the pointer, so this must not appear as a
    // submodule pointer change.
    QVERIFY(writeFile(m_lib, QStringLiteral("untracked.txt"), QStringLiteral("x\n")));
    for (const FileChange& change : session.status()) {
        QVERIFY(!change.submodule);
    }
}

void SubmoduleTest::theAutoUpdateHookRestoresTheRecordedPointer() {
    RepoSession session;
    QVERIFY(session.open(m_parent));
    QString error;
    QVERIFY(session.addSubmodule(m_lib, QStringLiteral("lib"), QStringLiteral("main"), false,
                                 &error));
    QVERIFY(git(m_parent, {"commit", "-q", "-m", "add lib"}));

    QVERIFY(writeFile(m_lib, QStringLiteral("b.txt"), QStringLiteral("new\n")));
    QVERIFY(git(m_lib, {"add", "-A"}));
    QVERIFY(git(m_lib, {"commit", "-q", "-m", "advance"}));
    QVERIFY2(session.updateSubmodules({QStringLiteral("lib")}, true, true, false, &error),
             qPrintable(error));
    QCOMPARE(findSubmodule(session.submodules(), QStringLiteral("lib"))->state,
             SubmoduleState::OutOfSync);

    QSignalSpy spy(&session, &RepoSession::submodulesUpdated);
    session.requestAutoUpdateSubmodules(/*remote=*/false);
    QTRY_COMPARE(spy.count(), 1);
    QVERIFY(spy.at(0).at(0).toBool());

    QCOMPARE(findSubmodule(session.submodules(), QStringLiteral("lib"))->state,
             SubmoduleState::InSync);
}

QTEST_MAIN(SubmoduleTest)
#include "test_submodules.moc"
