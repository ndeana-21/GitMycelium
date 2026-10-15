#include "core/types.h"
#include "git/cli.h"
#include "model/commit_model.h"
#include "model/ref_model.h"
#include "model/ref_visibility.h"
#include "repo/repo_session.h"
#include "theme/theme.h"
#include "ui/main_window.h"
#include "ui/ref_tree_view.h"

#include <QDir>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

using namespace gk;

namespace {

// Each commit is a second later than the last. Which of two tips the graph
// draws first follows their dates, and commits made within one second leave that
// to a tie-break — the fixtures describe an order, so they spell it out.
void stampNextCommit(const QStringList& args) {
    static qint64 date = 1700000000;
    if (args.contains(QStringLiteral("commit"))) {
        const QByteArray stamp = QByteArray::number(++date) + " +0000";
        qputenv("GIT_AUTHOR_DATE", stamp);
        qputenv("GIT_COMMITTER_DATE", stamp);
    }
}

bool git(const QString& dir, const QStringList& args) {
    stampNextCommit(args);
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

QStringList identity() {
    return {QStringLiteral("-c"), QStringLiteral("user.name=Test"), QStringLiteral("-c"),
            QStringLiteral("user.email=test@localhost")};
}

bool writeFile(const QString& dir, const QString& name, const QString& content) {
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

/// The same rule the window applies: every ref is visible except the hidden
/// branches. Tags are a name for a commit, not a branch, so they are never hidden.
HistoryFilter hiding(const RepoSession& session, const QSet<QString>& hidden) {
    HistoryFilter filter;
    filter.scope = HistoryScope::AllRefs;
    for (const RefInfo& ref : session.refs()) {
        const bool branch =
            ref.kind == RefKind::LocalBranch || ref.kind == RefKind::RemoteBranch;
        if (branch && hidden.contains(QString::fromStdString(ref.shortName))) {
            continue;
        }
        filter.visibleRefs.push_back(ref.fullName);
    }
    return filter;
}

HistoryFilter soloing(const RepoSession& session, const QStringList& soloed) {
    HistoryFilter filter;
    filter.scope = HistoryScope::AllRefs;
    const QString head = QString::fromStdString(session.info().headBranch);
    for (const RefInfo& ref : session.refs()) {
        const bool branch =
            ref.kind == RefKind::LocalBranch || ref.kind == RefKind::RemoteBranch;
        const QString name = QString::fromStdString(ref.shortName);
        if (branch && !soloed.contains(name) && name != head) {
            continue;
        }
        filter.visibleRefs.push_back(ref.fullName);
    }
    return filter;
}

QSet<QString> labelNames(const std::unordered_map<std::string, std::vector<RefInfo>>& byCommit) {
    QSet<QString> names;
    for (const auto& [oid, bucket] : byCommit) {
        for (const RefInfo& ref : bucket) {
            names.insert(QString::fromStdString(ref.shortName));
        }
    }
    return names;
}

QStandardItem* refItem(QStandardItem* parent, const QString& shortName) {
    for (int row = 0; row < parent->rowCount(); ++row) {
        QStandardItem* item = parent->child(row);
        if (!item) {
            continue;
        }
        if (item->data(RefModel::ShortNameRole).toString() == shortName) {
            return item;
        }
        if (QStandardItem* found = refItem(item, shortName)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

/// Hiding and soloing a branch is a filter on the history walk, not a label
/// hiding trick: an exclusive commit must leave the graph, while shared history
/// and the other branches stay.
class GraphVisibilityTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void hidingABranchDropsItsExclusiveCommits();
    void sharedHistorySurvivesHiding();
    void soloKeepsTheBranchAndHead();
    void labelsFollowTheFilter();
    void theUnfilteredWalkSeesEverything();

    void theModelWalksAndLabelsTheFilteredSet();
    void aPinnedBranchGetsTheLeftmostLane();

    void theStoreRemembersPerRepository();
    void theStoreAdoptsTheOldCheckoutPathKey();
    void theTreeMarksHiddenBranches();
    void theWindowPathReportsTheFilteredGraph();
    void theEyeHidesASectionAndShowsItAgain();
    void aGoneMergedBranchCanFoldIntoItsMerge();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

void GraphVisibilityTest::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->path();
    QVERIFY(git(m_path, {"init", "-q", "-b", "main"}));
    QVERIFY(writeFile(m_path, "base.txt", "base\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "base"}));
    QVERIFY(git(m_path, {"tag", "v1.0"}));

    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature-a"}));
    QVERIFY(writeFile(m_path, "a1.txt", "a1\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "a1"}));
    QVERIFY(writeFile(m_path, "a2.txt", "a2\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "a2"}));

    QVERIFY(git(m_path, {"checkout", "-q", "-b", "feature-b", "main"}));
    QVERIFY(writeFile(m_path, "b1.txt", "b1\n"));
    QVERIFY(git(m_path, {"add", "-A"}));
    QVERIFY(git(m_path, identity() + QStringList{"commit", "-q", "-m", "b1"}));

    QVERIFY(git(m_path, {"checkout", "-q", "main"}));
}

void GraphVisibilityTest::cleanup() {
    m_dir.reset();
}

void GraphVisibilityTest::hidingABranchDropsItsExclusiveCommits() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    bool hasMore = false;
    const auto all = session.commitsBlocking(0, 1000, &hasMore,
                                             HistoryFilter{HistoryScope::AllRefs, {}});
    QCOMPARE(all.size(), std::size_t(4));

    const auto filtered = session.commitsBlocking(0, 1000, &hasMore,
                                                  hiding(session, {QStringLiteral("feature-a")}));
    QCOMPARE(filtered.size(), std::size_t(2));

    QSet<QString> ids;
    for (const CommitInfo& c : filtered) {
        ids.insert(QString::fromStdString(c.id));
    }
    QVERIFY(ids.contains(gitOut(m_path, {"rev-parse", "main"})));
    QVERIFY(ids.contains(gitOut(m_path, {"rev-parse", "feature-b"})));
    QVERIFY(!ids.contains(gitOut(m_path, {"rev-parse", "feature-a"})));
}

void GraphVisibilityTest::sharedHistorySurvivesHiding() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    bool hasMore = false;
    const auto filtered = session.commitsBlocking(0, 1000, &hasMore,
                                                  hiding(session, {QStringLiteral("feature-a")}));
    // The merge base is what both branches descend from; hiding one must not take
    // the common ancestor with it. This is exactly what `git_revwalk_hide` would
    // have done, and why the filter refuses it.
    QSet<QString> ids;
    for (const CommitInfo& c : filtered) {
        ids.insert(QString::fromStdString(c.id));
    }
    QVERIFY(ids.contains(gitOut(m_path, {"rev-parse", "v1.0^{commit}"})));
}

void GraphVisibilityTest::soloKeepsTheBranchAndHead() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    bool hasMore = false;
    const auto filtered =
        session.commitsBlocking(0, 1000, &hasMore, soloing(session, {QStringLiteral("feature-b")}));
    QCOMPARE(filtered.size(), std::size_t(2));

    QSet<QString> ids;
    for (const CommitInfo& c : filtered) {
        ids.insert(QString::fromStdString(c.id));
    }
    QVERIFY(ids.contains(gitOut(m_path, {"rev-parse", "feature-b"})));
    QVERIFY(!ids.contains(gitOut(m_path, {"rev-parse", "feature-a"})));
}

void GraphVisibilityTest::labelsFollowTheFilter() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    const QSet<QString> hidden = labelNames(session.refsByCommit(hiding(
        session, {QStringLiteral("feature-a")})));
    QVERIFY(hidden.contains(QStringLiteral("main")));
    QVERIFY(hidden.contains(QStringLiteral("feature-b")));
    QVERIFY(hidden.contains(QStringLiteral("v1.0")));
    QVERIFY(!hidden.contains(QStringLiteral("feature-a")));
}

void GraphVisibilityTest::theUnfilteredWalkSeesEverything() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    bool hasMore = false;
    const auto all = session.commitsBlocking(0, 1000, &hasMore,
                                             HistoryFilter{HistoryScope::AllRefs, {}});
    QCOMPARE(all.size(), std::size_t(4));

    const QSet<QString> names = labelNames(session.refsByCommit({}));
    QVERIFY(names.contains(QStringLiteral("feature-a")));
    QVERIFY(names.contains(QStringLiteral("feature-b")));
    QVERIFY(names.contains(QStringLiteral("main")));
    QVERIFY(names.contains(QStringLiteral("v1.0")));
}

void GraphVisibilityTest::theModelWalksAndLabelsTheFilteredSet() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    CommitModel model(&session);
    model.setHistoryFilter(hiding(session, {QStringLiteral("feature-a")}));
    model.reset();
    QTRY_COMPARE(model.rowCount(), 2);

    QVERIFY(model.rowForCommit(gitOut(m_path, {"rev-parse", "main"})) >= 0);
    QVERIFY(model.rowForCommit(gitOut(m_path, {"rev-parse", "feature-a"})) < 0);

    const int bRow = model.rowForCommit(gitOut(m_path, {"rev-parse", "feature-b"}));
    QVERIFY(bRow >= 0);
    QSet<QString> labels;
    for (const RefInfo& ref : model.refsAt(bRow)) {
        labels.insert(QString::fromStdString(ref.shortName));
    }
    QVERIFY(labels.contains(QStringLiteral("feature-b")));
    QVERIFY(!labels.contains(QStringLiteral("feature-a")));
}

void GraphVisibilityTest::aPinnedBranchGetsTheLeftmostLane() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    CommitModel model(&session);
    model.setPinnedBranches({QStringLiteral("feature-b")});
    model.reset();
    QTRY_COMPARE(model.rowCount(), 4);

    const QString baseId = gitOut(m_path, {"rev-parse", "main"});
    const QString aId = gitOut(m_path, {"rev-parse", "feature-a"});
    const QString bId = gitOut(m_path, {"rev-parse", "feature-b"});

    const GraphNode* base = model.graphNodeAt(model.rowForCommit(baseId));
    const GraphNode* a = model.graphNodeAt(model.rowForCommit(aId));
    const GraphNode* b = model.graphNodeAt(model.rowForCommit(bId));
    QVERIFY(base && a && b);
    // HEAD keeps lane 0; the pinned branch takes the next one, ahead of the rest.
    QCOMPARE(base->lane, 0);
    QVERIFY(b->lane < a->lane);
}

void GraphVisibilityTest::theStoreRemembersPerRepository() {
    const QString store = QDir(m_path).filePath(QStringLiteral("visibility.json"));
    const QString repoA = QStringLiteral("/repos/a");
    const QString repoB = QStringLiteral("/repos/b");

    {
        RefVisibility visibility(store);
        visibility.load(repoA);
        visibility.setHidden(QStringLiteral("noisy"), true);
        visibility.setSolo(QStringLiteral("focus"), true);
    }
    {
        RefVisibility visibility(store);
        visibility.load(repoA);
        QVERIFY(visibility.isHidden(QStringLiteral("noisy")));
        QVERIFY(visibility.isSolo(QStringLiteral("focus")));
        QVERIFY(!visibility.isHidden(QStringLiteral("focus")));
        visibility.load(repoB);
        QVERIFY(!visibility.isHidden(QStringLiteral("noisy")));
        QVERIFY(visibility.empty());
    }
    {
        RefVisibility visibility(store);
        visibility.load(repoA);
        visibility.setHidden(QStringLiteral("noisy"), false);
        visibility.setSolo(QStringLiteral("focus"), false);
        QVERIFY(visibility.empty());
    }
}

void GraphVisibilityTest::theStoreAdoptsTheOldCheckoutPathKey() {
    const QString store = QDir(m_path).filePath(QStringLiteral("visibility.json"));
    const QString oldKey = QStringLiteral("/checkouts/project");
    const QString newKey = QStringLiteral("/checkouts/project/.git");

    // Settings used to be keyed by the checkout path. Opening through the new
    // common-git-dir key has to adopt them, or every hidden branch silently comes
    // back the first time a worktree is opened.
    {
        RefVisibility old(store);
        old.load(oldKey);
        old.setHidden(QStringLiteral("noisy"), true);
        old.setSmart(true);
    }

    RefVisibility migrated(store);
    migrated.load(newKey, oldKey);
    QVERIFY(migrated.isHidden(QStringLiteral("noisy")));
    QVERIFY(migrated.smart());

    RefVisibility fresh(store);
    fresh.load(newKey);
    QVERIFY(fresh.isHidden(QStringLiteral("noisy")));
    QVERIFY(fresh.smart());

    RefVisibility stale(store);
    stale.load(oldKey);
    QVERIFY(stale.empty());
}

void GraphVisibilityTest::theTreeMarksHiddenBranches() {
    RepoSession session;
    QVERIFY(session.open(m_path));

    RefModel model;
    model.setHeadBranch(QString::fromStdString(session.info().headBranch));
    model.setVisibility({QStringLiteral("feature-a")}, {});
    model.rebuild(session.refs());

    QStandardItem* hidden = refItem(model.invisibleRootItem(), QStringLiteral("feature-a"));
    QStandardItem* shown = refItem(model.invisibleRootItem(), QStringLiteral("feature-b"));
    QStandardItem* head = refItem(model.invisibleRootItem(), QStringLiteral("main"));
    QVERIFY(hidden && shown && head);
    // The tree and the graph must not disagree: a hidden branch is dimmed, a
    // visible one is not, and HEAD never is.
    QCOMPARE(hidden->foreground().color(), theme::textDim());
    QVERIFY(shown->foreground().color() != theme::textDim());
    QVERIFY(head->foreground().color() != theme::textDim());

    model.setVisibility({}, {QStringLiteral("feature-b")});
    QCOMPARE(refItem(model.invisibleRootItem(), QStringLiteral("feature-a"))->foreground().color(),
             theme::textDim());
    QVERIFY(refItem(model.invisibleRootItem(), QStringLiteral("feature-b"))->foreground().color() !=
            theme::textDim());
}

void GraphVisibilityTest::theWindowPathReportsTheFilteredGraph() {
    MainWindow window;
    QVERIFY(window.openRepository(m_path));

    QStringList report = window.visibilityForDiagnostics(QStringLiteral("hide"),
                                                         QStringLiteral("feature-a"));
    QString text = report.join(QLatin1Char('\n'));
    QVERIFY(text.contains(QStringLiteral("rows: 2")));
    QVERIFY(text.contains(QStringLiteral("label: feature-b")));
    QVERIFY(!text.contains(QStringLiteral("label: feature-a")));
    QVERIFY(text.contains(QStringLiteral("hidden: 1")));

    report = window.visibilityForDiagnostics(QStringLiteral("solo"), QStringLiteral("feature-b"));
    // Solo wins over the earlier hide: feature-a stays hidden, and feature-b shows.
    text = report.join(QLatin1Char('\n'));
    QVERIFY(text.contains(QStringLiteral("solo: yes")));
    QVERIFY(text.contains(QStringLiteral("rows: 2")));

    window.visibilityForDiagnostics(QStringLiteral("show"), QString());
    // Leaves the user's store with no entry for this temporary repository.
    report = window.visibilityForDiagnostics(QStringLiteral("show"), QString());
    text = report.join(QLatin1Char('\n'));
    QVERIFY(text.contains(QStringLiteral("rows: 4")));
    QVERIFY(text.contains(QStringLiteral("hidden: 0")));
}

void GraphVisibilityTest::theEyeHidesASectionAndShowsItAgain() {
    MainWindow window;
    window.setHeadless(true);
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openRepository(m_path));
    auto* tree = window.findChild<RefTreeView*>();
    QVERIFY(tree);
    tree->expandAll();

    const auto find = [tree](const QString& text) {
        const QModelIndexList found = tree->model()->match(
            tree->model()->index(0, 0), Qt::DisplayRole, text, 1,
            Qt::MatchExactly | Qt::MatchRecursive);
        return found.isEmpty() ? QModelIndex() : found.front();
    };
    const auto hover = [tree](const QPoint& at) {
        QMouseEvent move(QEvent::MouseMove, at, tree->viewport()->mapToGlobal(at), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(tree->viewport(), &move);
    };
    const auto click = [tree, &hover](const QModelIndex& index) {
        const QPoint at = tree->eyeRect(index).center();
        hover(at);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, at);
    };

    const QModelIndex local = find(QStringLiteral("Local"));
    QVERIFY(local.isValid());
    QCOMPARE(local.data(RefModel::EyeRole).toInt(), int(RefModel::EyeShown));
    QVERIFY(!tree->eyeVisible(local));
    hover(tree->visualRect(local).center());
    QVERIFY(tree->eyeVisible(local));

    // The current branch stays: the eye hides the two others under the section.
    click(local);
    QString text = window.visibilityForDiagnostics(QStringLiteral("report"), {}).join('\n');
    QVERIFY2(text.contains(QStringLiteral("hidden: 2")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("rows: 1")));
    const QModelIndex featureA = find(QStringLiteral("feature-a"));
    QCOMPARE(featureA.data(RefModel::EyeRole).toInt(), int(RefModel::EyeHidden));
    QVERIFY(tree->eyeVisible(featureA));
    QCOMPARE(find(QStringLiteral("Local")).data(RefModel::EyeRole).toInt(),
             int(RefModel::EyeHidden));

    click(featureA);
    text = window.visibilityForDiagnostics(QStringLiteral("report"), {}).join('\n');
    QVERIFY(text.contains(QStringLiteral("hidden: 1")));
    QVERIFY(text.contains(QStringLiteral("label: feature-a")));

    click(find(QStringLiteral("Local")));
    text = window.visibilityForDiagnostics(QStringLiteral("report"), {}).join('\n');
    QVERIFY(text.contains(QStringLiteral("hidden: 2")));

    click(find(QStringLiteral("Local")));
    text = window.visibilityForDiagnostics(QStringLiteral("show"), {}).join('\n');
    QVERIFY(text.contains(QStringLiteral("rows: 4")));
}

void GraphVisibilityTest::aGoneMergedBranchCanFoldIntoItsMerge() {
    QVERIFY(git(m_path, identity() + QStringList{"merge", "-q", "--no-ff", "-m", "merge a",
                                                 "feature-a"}));
    QVERIFY(git(m_path, {"branch", "-D", "feature-a"}));
    MainWindow window;
    QVERIFY(window.openRepository(m_path));
    QString text =
        window.visibilityForDiagnostics(QStringLiteral("merged-commits"), {}).join('\n');
    QVERIFY2(text.contains(QStringLiteral("rows: 5")), qPrintable(text));
    text = window.visibilityForDiagnostics(QStringLiteral("merged-commits-off"), {}).join('\n');
    QVERIFY2(text.contains(QStringLiteral("rows: 3")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("merged commits: merge only")));
    window.visibilityForDiagnostics(QStringLiteral("merged-commits"), {});
}

QTEST_MAIN(GraphVisibilityTest)
#include "test_graph_visibility.moc"
