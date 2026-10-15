#include "ui/main_window.h"
#include "ui/local_ignore_dialog.h"
#include "ui/panel_splitter.h"

#include "ui/badge_flight.h"
#include "ui/drop_point.h"

#include "model/commit_model.h"
#include "model/date_format.h"
#include "model/ref_model.h"
#include "repo/repo_session.h"
#include "ui/commit_graph_view.h"
#include "ui/file_history_dialog.h"
#include "ui/gitflow_dialog.h"
#include "ui/lfs_dialog.h"
#include "ui/profile_dialog.h"
#include "ui/profiles.h"
#include "ui/reflog_dialog.h"
#include "ui/sparse_checkout_dialog.h"
#include "ui/rebase_editor.h"
#include "ui/repo_watcher.h"
#include "ui/worktree_dialog.h"
#include "ui/submodule_dialog.h"
#include "ui/conflict_view.h"
#include "ui/clone_dialog.h"
#include "ui/coauthor_dialog.h"
#include "ui/preferences_dialog.h"
#include "ui/settings_store.h"
#include "ui/theme_apply.h"
#include "ui/themed_icons.h"
#include "ui/start_page.h"
#include "ui/create_repository_dialog.h"
#include "ui/hooks_dialog.h"
#include "git/clone.h"
#include "core/external_terminal.h"
#include "ui/graph_delegate.h"
#include "ui/ref_tree_view.h"
#include "theme/glyphs.h"
#include "theme/icons.h"
#include "ui/refs_delegate.h"
#include "ui/skeleton.h"
#include "ui/update_dialog.h"
#include "update/updater.h"
#include "theme/theme.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QThreadPool>

#include <algorithm>
#include <QCloseEvent>
#include <QFileDialog>
#include <QHash>
#include <QProcess>
#include <QStandardPaths>
#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QEventLoop>
#include <QFile>
#include <QInputDialog>
#include <QLineEdit>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QPushButton>
#include <QToolButton>
#include <QMenu>
#include <QFont>
#include <QFontDatabase>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>

#include <cstdio>
#include <QTimer>
#include <QSplitter>
#include <QShortcut>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QStatusBar>

#include <QTextEdit>
#include <QToolBar>
#include <QTreeView>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

namespace gk {
namespace {

void showAboutBox(QWidget* parent) {
    QMessageBox::about(
        parent, QStringLiteral("About GitMycelium"),
        QStringLiteral("<b>GitMycelium %1</b><br>"
                       "A fast native Git client for large repositories, "
                       "built with C++20, Qt 6 and libgit2.<br><br>"
                       "Licensed under the Apache License, Version 2.0.<br>"
                       "Uses Qt (LGPLv3), libgit2 (GPLv2 with linking exception) "
                       "and Lucide icons (ISC).")
            .arg(update::describeVersion(update::currentVersion(), update::currentBuild())));
}

struct DocumentRequest {
    QString path;
    bool workingTree = false;
    bool staged = false;
    QString commitId;
    QString compareBase;
    QString compareTarget;
    QString compareLabel;
    bool compareWorkingTree = false;
};

DiffView::Document readDocument(RepoSession& session, const DocumentRequest& request) {
    const QString& path = request.path;
    DiffView::Document document;
    document.path = path;
    std::string bytes;
    if (request.workingTree && request.staged) {
        document.diff = session.stagedFileDiff(path);
        document.staging = DiffView::Staging::Unstage;
        document.revisionContent = session.indexFileContent(path);
        document.hasRevision = true;
        document.contentLabel = QStringLiteral("Index");
    } else if (request.workingTree) {
        document.diff = session.workdirFileDiff(path);
        document.staging = DiffView::Staging::Stage;
        // The unstaged side can also be thrown away; the staged side cannot (that is
        // an unstage, and the committed version is what is left).
        document.offerDiscard = true;
        if (session.workingFileContent(path, &bytes, nullptr)) {
            document.workingContent = std::move(bytes);
            document.hasWorking = true;
        }
        document.contentLabel = QStringLiteral("Working tree");
    } else if (!request.compareBase.isEmpty()) {
        if (request.compareWorkingTree) {
            document.diff = session.workdirFileDiffAgainst(request.compareBase, path);
            if (session.workingFileContent(path, &bytes, nullptr)) {
                document.workingContent = std::move(bytes);
                document.hasWorking = true;
            }
        } else {
            document.diff = session.fileDiffBetween(request.compareBase, request.compareTarget, path);
            if (session.fileAtRevision(request.compareTarget, path, &bytes, nullptr)) {
                document.revisionContent = std::move(bytes);
                document.hasRevision = true;
            }
        }
        document.contentLabel = request.compareLabel;
    } else {
        document.diff = session.commitFileDiff(request.commitId, path);
        if (session.fileAtRevision(request.commitId, path, &bytes, nullptr)) {
            document.revisionContent = std::move(bytes);
            document.hasRevision = true;
        }
        document.contentLabel = request.commitId.left(7);
    }
    return document;
}

CommitDetails readCommitDetails(RepoSession& session, const QString& commitId) {
    CommitDetails details;
    details.files = session.changedFiles(commitId);
    // Only the commit HEAD points at is amendable: rewording an older one rebuilds
    // everything after it, which is a rebase and not a text field.
    details.amendable = commitId == QString::fromStdString(session.info().headId);
    // Asked for one commit, on selection: reading `%G?` runs the signature check,
    // which is not something to do for every row of the graph.
    details.signature = session.commitSignature(commitId);
    return details;
}

/// A stacked widget asks for the largest minimum of all its pages, shown or not, so
/// a hidden page could hold the whole window wide. Only the page on screen counts.
void sizeToCurrentPage(QStackedWidget* stack) {
    const auto apply = [stack] {
        for (int i = 0; i < stack->count(); ++i) {
            QWidget* page = stack->widget(i);
            const QVariant own = page->property("gkOwnSizePolicy");
            if (!own.isValid()) {
                page->setProperty("gkOwnSizePolicy", QVariant::fromValue(page->sizePolicy()));
            }
            page->setSizePolicy(i == stack->currentIndex()
                                    ? page->property("gkOwnSizePolicy").value<QSizePolicy>()
                                    : QSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored));
        }
        stack->updateGeometry();
    };
    QObject::connect(stack, &QStackedWidget::currentChanged, stack, apply);
    apply();
}

constexpr int kMaxDiffLines = 8000;
constexpr int kDefaultGraphLanes = 5;
constexpr int kKindRole = Qt::UserRole + 1;
constexpr int kHistoryPage = 0;
constexpr int kConflictsPage = 1;
constexpr int kStartPage = 2;

QString describe(const RepoInfo& info) {
    if (info.headBranch.empty()) {
        return info.detached ? QStringLiteral("DETACHED HEAD") : QStringLiteral("(no branch)");
    }
    return QString::fromStdString(info.headBranch);
}

/// "a.txt: lines 3–5, 12" — the file a prediction names and where in our side
/// of it the conflicts are; just the path when there is nothing to point at.
QString describePredictedConflict(const MergePreview& preview, const std::string& path) {
    const QString name = QString::fromStdString(path);
    const auto it = preview.conflictedLines.find(path);
    if (it == preview.conflictedLines.end() || it->second.empty()) {
        return name;
    }
    QStringList spans;
    for (const LineSpan& span : it->second) {
        spans << (span.count > 1
                      ? QStringLiteral("%1–%2").arg(span.first).arg(span.first + span.count - 1)
                      : QString::number(span.first));
    }
    const bool several = it->second.size() > 1 || it->second.front().count > 1;
    return QStringLiteral("%1: %2 %3")
        .arg(name, several ? QStringLiteral("lines") : QStringLiteral("line"),
             spans.join(QStringLiteral(", ")));
}

QString changesSummary(const std::vector<FileChange>& changes) {
    int staged = 0;
    int unstaged = 0;
    for (const auto& c : changes) {
        (c.staged ? staged : unstaged)++;
    }
    return QStringLiteral("%1 staged · %2 changed").arg(staged).arg(unstaged);
}

/// The last `count` non-empty lines of git's output. A clone failure prints a
/// paragraph of hints; the line that says what went wrong is at the end, and a
/// modal with twenty lines makes it the reader's job to find.
QString lastLines(const QString& output, int count) {
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QStringList tail;
    const qsizetype from = std::max<qsizetype>(0, lines.size() - count);
    for (qsizetype i = from; i < lines.size(); ++i) {
        tail << lines.at(i).trimmed();
    }
    return tail.join(QLatin1Char('\n'));
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    m_contexts.push_back(makeContext());
    m_active = 0;
    adoptContext(m_contexts.front().get());
    m_autoFetchTimer = new QTimer(this);
    m_autoFetchTimer->setInterval(5 * 60 * 1000);
    connect(m_autoFetchTimer, &QTimer::timeout, this, &MainWindow::onAutoFetchTick);
    m_watcher = new RepoWatcher(this);
    m_watcher->setIgnoredLister(&RepoSession::ignoredPaths);
    connect(m_watcher, &RepoWatcher::changed, this, &MainWindow::onRepositoryFilesChanged);
    buildUi();
    bindViews();
    showEmptyTab();
    // The empty starting context is a tab like any other and has to be on the strip:
    // a window whose only context is invisible looks like a window with no tabs, and
    // the "+" beside it makes no sense in that state.
    rebuildTabBar();
    loadGraphAnimation();
    setWindowTitle(QStringLiteral("GitMycelium"));
    resize(1500, 900);
}

void MainWindow::setUpdateChecksEnabled(bool on) {
    if (!on || !SettingsStore().boolValue(SettingsStore::globalKey(QStringLiteral("updates/check")),
                                          true)) {
        return;
    }
    SettingsStore store;
    const QString lastKey = SettingsStore::globalKey(QStringLiteral("updates/last-check"));
    const QDateTime last = QDateTime::fromString(store.stringValue(lastKey), Qt::ISODate);
    if (last.isValid() && last.secsTo(QDateTime::currentDateTimeUtc()) < 20 * 3600) {
        return;
    }
    QTimer::singleShot(5000, this, [this, lastKey] {
        SettingsStore().setString(lastKey, QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        m_updater->check();
    });
}

void MainWindow::showUpdateDialog() {
    UpdateDialog dialog(m_updater, this);
    connect(&dialog, &UpdateDialog::restartRequested, this, [this] {
        close();
        QCoreApplication::quit();
    });
    dialog.exec();
}

void MainWindow::setBackgroundIndexingEnabled(bool on) {
    m_backgroundIndexing = on;
    for (const auto& context : m_contexts) {
        if (context->session) {
            context->session->setAutoCommitGraph(on);
        }
    }
}

std::unique_ptr<MainWindow::RepoContext> MainWindow::makeContext() {
    auto context = std::make_unique<RepoContext>();
    // The context owns its session and models, so they are not QObject children of
    // the window: a parented object would be deleted twice. The views that point at
    // them are detached in the destructor before these go away.
    context->session.reset(new RepoSession(nullptr));
    context->session->setAutoCommitGraph(m_backgroundIndexing);
    context->commitModel = std::make_unique<CommitModel>(context->session.get(), nullptr);
    context->refModel = std::make_unique<RefModel>(nullptr);
    wireContext(context.get());
    return context;
}

void MainWindow::wireContext(RepoContext* context) {
    RepoSession* session = context->session.get();
    CommitModel* model = context->commitModel.get();
    RefModel* refs = context->refModel.get();

    // Every signal is filtered by "is this the context on screen": a page loaded or
    // a fetch finished in a background tab must not redraw the repository in front
    // of the reader. Switching to that tab runs a full refresh and catches up.
    //
    // The context is looked up from the session on every signal rather than captured:
    // a tab closed while its fetch ran has no context any more, but its session lives
    // on until the fetch ends (`RepoSession::retire`) and still reports — and the
    // window's running flag, which is global, must clear all the same.
    connect(session, &RepoSession::syncFinished, this,
            [this, session](bool ok, QString summary, QString output) {
                RepoContext* context = contextOf(session);
                if (context && active() == context) {
                    onSyncFinished(ok, summary, output);
                    return;
                }
                m_syncRunning = false;
                m_silentSync = false;
                m_commitView->setBusy(false);
                updateSyncActions();
                if (context) {
                    context->commitModel->reset();
                }
            });
    connect(session, &RepoSession::snapshotReady, this, [this, session] { onSnapshotReady(session); });
    connect(model, &CommitModel::loadingChanged, this, [this, context] {
        if (active() == context) {
            updateGraphSkeleton();
        }
    });
    connect(session, &RepoSession::readFailed, this, [this, session](QString message) {
        if (RepoContext* context = contextOf(session); context && active() == context) {
            notify(NotificationLog::Error, QStringLiteral("Cannot read the repository: %1").arg(message));
        }
    });
    connect(session, &RepoSession::maintenanceFinished, this,
            [this, session](bool ok, QString task, QString output) {
                if (RepoContext* context = contextOf(session); context && active() == context) {
                    onMaintenanceFinished(ok, task, output);
                    return;
                }
                m_maintenanceRunning = false;
                m_commitView->setBusy(false);
            });
    connect(session, &RepoSession::submodulesUpdated, this,
            [this, session](bool ok, QString output) {
                if (RepoContext* context = contextOf(session); context && active() == context) {
                    onSubmodulesUpdated(ok, output);
                    return;
                }
                m_submoduleUpdateRunning = false;
                m_commitView->setBusy(false);
            });
    connect(session, &RepoSession::lfsFinished, this,
            [this, session](bool ok, QString task, QString output) {
                m_lfsRunning = false;
                RepoContext* context = contextOf(session);
                if (!context || active() != context) {
                    m_commitView->setBusy(false);
                    return;
                }
                m_commitView->setBusy(false);
                const QString last =
                    output.trimmed().isEmpty()
                        ? QStringLiteral("done")
                        : output.trimmed().section(QLatin1Char('\n'), -1).trimmed();
                if (ok) {
                    notify(NotificationLog::Success,
                           QStringLiteral("git lfs %1: %2").arg(task, last));
                } else {
                    notify(NotificationLog::Warning,
                           QStringLiteral("git lfs %1 failed: %2").arg(task, last));
                }
            });
    // A reveal pages history until the commit it wants is loaded; each page arrives
    // as an insertion, which is the moment to look again. Only the visible model's
    // pages are worth reacting to.
    connect(model, &QAbstractItemModel::rowsInserted, this,
            [this, context](const QModelIndex&, int, int) {
                if (active() == context) {
                    restoreSelectionAfterReload();
                    tryRevealCommit();
                }
            });
    connect(model, &CommitModel::graphChanged, this, [this, context] {
        if (active() == context) {
            updateGraphColumnWidth();
        }
    });
    connect(model, &CommitModel::highlightChanged, this, [this, context] {
        if (active() == context && m_commitView) {
            m_commitView->viewport()->update();
        }
    });
    // Drops are user-driven, so they always arrive from the active context; the
    // handlers read `m_session`/`m_commitModel`, which point at it.
    connect(model, &CommitModel::refDroppedOnCommit, this, &MainWindow::onRefDroppedOnCommit);
    connect(refs, &RefModel::refDroppedOnRef, this, &MainWindow::onRefDroppedOnRef);
}

MainWindow::RepoContext* MainWindow::contextOf(const RepoSession* session) {
    for (const std::unique_ptr<RepoContext>& context : m_contexts) {
        if (context->session.get() == session) {
            return context.get();
        }
    }
    return nullptr;
}

MainWindow::RepoContext* MainWindow::active() {
    if (m_active < 0 || m_active >= static_cast<int>(m_contexts.size())) {
        return nullptr;
    }
    return m_contexts[static_cast<std::size_t>(m_active)].get();
}

const MainWindow::RepoContext* MainWindow::active() const {
    if (m_active < 0 || m_active >= static_cast<int>(m_contexts.size())) {
        return nullptr;
    }
    return m_contexts[static_cast<std::size_t>(m_active)].get();
}

void MainWindow::bindViews() {
    if (!m_commitView || !m_refView) {
        return;
    }
    m_refView->setModel(m_refModel);
    m_commitView->setModel(m_commitModel);
    updateGraphSkeleton();
    if (m_conflictView) {
        m_conflictView->setSession(m_session);
    }
    // The selection model is replaced when the *model* changes, but it survives a
    // rebind to the same model — and bindViews runs on every activation. The
    // connection is therefore made unique: a second bind must not run onCommitSelected
    // twice for one click.
    if (m_commitModel && m_commitView->selectionModel()) {
        connect(m_commitView->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
                &MainWindow::onCommitSelected, Qt::UniqueConnection);
    }
}

void MainWindow::saveActiveState() {
    RepoContext* context = active();
    if (!context) {
        return;
    }
    context->pins = m_pins;
    context->visibility = m_visibility;
    context->selectedCommitId = m_selectedCommitId;
    context->renderedPath = m_renderedPath;
    context->renderedContext = m_renderedContext;
    context->renderedStaged = m_renderedStaged;
    context->lastFingerprint = m_lastFingerprint;
    context->revealTarget = m_revealTarget;
    context->revealPages = m_revealPages;
    context->diffCodec = m_diffCodec;
    context->compareBase = m_compareBase;
    context->compareTarget = m_compareTarget;
    context->compareMode = m_compareMode;
    context->compareLabel = m_compareLabel;
    context->refFilter = m_refFilter ? m_refFilter->text() : QString();
    context->refFilterRegex = m_refFilterRegex && m_refFilterRegex->isChecked();
    context->graphColumnUserSized = m_graphColumnUserSized;
    context->columnMode = m_columnMode;
    context->timelinePeriod = m_timelinePeriod;
    context->conflictPageDismissed = m_conflictPageDismissed;
    context->diffPageShown = diffPageVisible();
    // Flush a column drag the debounce timer has not written yet. Only for a
    // context that was already on screen: opening a repository into the empty
    // starting context also lands here, and flushing then would save the default
    // layout over the reader's saved one before it was ever loaded.
    if (m_commitView && m_session && m_session->isOpen() && active() && active()->primed) {
        saveCommitColumns();
    }
}

void MainWindow::adoptContext(RepoContext* context) {
    m_session = context->session.get();
    m_commitModel = context->commitModel.get();
    if (m_mergedRemotesAction) {
        m_commitModel->setShowMergedRemotes(m_mergedRemotesAction->isChecked());
    }
    m_refModel = context->refModel.get();
    m_pins = context->pins;
    m_visibility = context->visibility;
    m_selectedCommitId = context->selectedCommitId;
    m_renderedPath = context->renderedPath;
    m_renderedContext = context->renderedContext;
    m_renderedStaged = context->renderedStaged;
    m_lastFingerprint = context->lastFingerprint;
    m_revealTarget = context->revealTarget;
    m_revealPages = context->revealPages;
    m_diffCodec = context->diffCodec;
    m_compareBase = context->compareBase;
    m_compareTarget = context->compareTarget;
    m_compareMode = context->compareMode;
    m_compareLabel = context->compareLabel;
    m_graphColumnUserSized = context->graphColumnUserSized;
    m_columnMode = context->columnMode;
    m_timelinePeriod = context->timelinePeriod;
    m_conflictPageDismissed = context->conflictPageDismissed;
    if (m_refFilter) {
        const QSignalBlocker block(m_refFilter);
        m_refFilter->setText(context->refFilter);
    }
    if (m_refFilterRegex) {
        const QSignalBlocker block(m_refFilterRegex);
        m_refFilterRegex->setChecked(context->refFilterRegex);
    }
}

void MainWindow::activateContext(int index) {
    if (index < 0 || index >= static_cast<int>(m_contexts.size())) {
        return;
    }
    // No "already active" shortcut: opening a repository into the empty starting
    // context activates the index it already has, and that still has to refresh.
    saveActiveState();
    m_active = index;
    RepoContext* context = active();
    adoptContext(context);
    bindViews();
    ++m_documentToken;
    ++m_commitFilesToken;
    m_documentPending = false;
    m_restoreCommit.clear();
    m_restoreWip = false;
    // The diff on screen is the tab being left's. The one coming back shows its own
    // file again (read anew below) and its own page, or nothing.
    const QString shownFile = m_renderedPath;
    const QString shownContext = m_renderedContext;
    const bool shownStaged = m_renderedStaged;
    m_renderedPath.clear();
    m_renderedContext.clear();
    if (m_diffView) {
        m_diffView->clear();
        m_diffView->setLoading(false);
    }
    if (context->diffPageShown && !shownFile.isEmpty()) {
        showDiffPage();
    } else {
        showGraphPage();
    }
    if (m_details) {
        m_details->setFilesLoading(false);
    }
    rebuildTabBar();
    if (m_commitView) {
        m_commitView->setBusy(false);
    }

    // A tab restored from the last session carries a path but has no session yet;
    // opening it here is what keeps startup from reading every monorepo at once.
    if (!context->opened && !context->path.isEmpty()) {
        openIntoContext(context, context->path);
    }
    if (!context->opened) {
        showEmptyTab();
        saveTabs();
        return;
    }

    if (context->alias.isEmpty()) {
        const QString alias =
            loadTabAlias(QString::fromStdString(m_session->paths().commonDir));
        if (!alias.isEmpty()) {
            context->alias = alias;
        }
    }

    // Both of the steps above change what the tab is called: opening a restored tab
    // turns "New Tab" into the repository's name, and the alias can turn that into
    // whatever the reader renamed it to. They happen after the bar was rebuilt for
    // the switch, so the one tab on screen is relabelled here — otherwise it wore its
    // old name until the reader switched away and back, because a switch was the only
    // thing that recomputed the labels.
    if (m_tabBar) {
        m_tabBar->setTabText(m_active, contextLabel(*context));
    }

    setWindowTitle(QStringLiteral("GitMycelium — %1")
                       .arg(contextLabel(*context)));
    const QString settingsPath = repositorySettingsPath();
    migrateRepositorySettings(repositoryLegacySettingsPath(), settingsPath);
    m_pins.load(settingsPath, repositoryLegacyStoreKey());
    m_visibility.load(settingsPath, repositoryLegacyStoreKey());
    // The first time a context is shown the model has no rows; afterwards it keeps
    // the page it was scrolled to, which is the whole point of tabs.
    if (!context->primed) {
        context->primed = true;
        const bool animateOnOpen = m_commitView->animationsEnabled();
        m_commitView->setAnimationsEnabled(false);
        m_commitModel->reset();
        m_commitView->setAnimationsEnabled(animateOnOpen);
    }
    loadCommitColumns();
    loadCommitDateFormat();
    loadTimeline();
    // The panels describe the tab's selected commit only if it can be selected again;
    // otherwise they would go on describing the tab that was left.
    if (!m_selectedCommitId.isEmpty() && m_commitModel->rowForCommit(m_selectedCommitId) < 0) {
        m_selectedCommitId.clear();
    }
    // A tab shown before comes back at once with what it last read; the fresh read
    // replaces it when it lands. A tab never shown has nothing to show yet.
    if (m_asyncReads) {
        if (m_session->snapshot()) {
            applyPanels();
        } else {
            setPanelsLoading(!context->panelsShown);
        }
    }
    refreshPanels();
    const RepoPaths paths = m_session->paths();
    m_watcher->setRoots(QString::fromStdString(paths.workDir), QString::fromStdString(paths.gitDir),
                        QString::fromStdString(paths.commonDir));
    loadAutoFetch();
    loadPreflight();
    loadPrevention();
    loadSubmoduleAutoupdate();
    if (!m_selectedCommitId.isEmpty()) {
        selectCommit(m_selectedCommitId);
    }
    const bool workingTreeFile =
        shownContext == QStringLiteral("staged") || shownContext == QStringLiteral("unstaged");
    if (!shownFile.isEmpty() && (workingTreeFile || shownContext == m_selectedCommitId)) {
        renderFile(shownFile, workingTreeFile, shownStaged);
    }
    noteRecentRepository(*context);
    saveTabs();
}

void MainWindow::showEmptyTab() {
    m_watcher->setRoots(QString(), QString());
    m_autoFetchTimer->stop();
    m_commitModel->reset();
    m_refModel->rebuild({});
    if (m_conflictView && m_session->isOpen()) {
        m_conflictView->reload();
    }
    if (m_branchLabel) {
        m_branchLabel->setText(QStringLiteral("No repository open"));
    }
    if (m_visibilityButton) {
        m_visibilityButton->setVisible(false);
    }
    setWindowTitle(QStringLiteral("GitMycelium"));
    m_stack->setCurrentIndex(kStartPage);
    refreshStartPage();
    setStatus(QStringLiteral("Open a repository (Ctrl+O) or clone one (Ctrl+Shift+O)."));
    refreshCommands();
}

void MainWindow::refreshStartPage() {
    if (!m_startPage) {
        return;
    }
    RecentRepositories recent;
    recent.load();
    m_startPage->setRecentRepositories(recent.entries());
}

void MainWindow::noteRecentRepository(const RepoContext& context) {
    if (!context.opened || context.path.isEmpty()) {
        return;
    }
    // Persistence is a profile write, so a headless check must not touch it — the
    // same rule the tab set follows.
    if (!m_tabsPersistence || m_headless) {
        return;
    }
    RecentRepositories recent;
    recent.load();
    recent.touch(context.path, QFileInfo(context.path).fileName());
}

void MainWindow::createRepositoryFromDialog() {
    CreateRepositoryDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString path = dialog.path();
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!RepoSession::createRepository(path, dialog.initialBranch(), &error)) {
        reportFailure(QStringLiteral("Cannot create repository"), error);
        return;
    }
    if (openRepository(path)) {
        setStatus(QStringLiteral("Created repository %1").arg(path));
    }
}

int MainWindow::contextForPath(const QString& key) const {
    for (std::size_t i = 0; i < m_contexts.size(); ++i) {
        if (m_contexts[i]->path == key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool MainWindow::openIntoContext(RepoContext* context, const QString& path) {
    QString error;
    if (!context->session->open(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("Cannot open repository"), error);
        return false;
    }
    const RepoInfo info = context->session->info();
    const QString checkout = QString::fromStdString(info.workDir.empty() ? info.gitDir : info.workDir);
    const QString canonical = QFileInfo(checkout).canonicalFilePath();
    context->path = canonical.isEmpty() ? QDir::cleanPath(checkout) : canonical;
    context->opened = true;
    context->primed = false;
    return true;
}

QString MainWindow::contextLabel(const RepoContext& context) const {
    if (!context.alias.isEmpty()) {
        return context.alias;
    }
    // A restored tab is a path with no session until it is activated, and it already
    // has a name: naming it by what it will open is the difference between a strip
    // that reads "alpha, beta, gamma" and one that reads "New Tab, New Tab, alpha".
    // Only a tab that holds no path at all is a new tab.
    if (context.path.isEmpty()) {
        return QStringLiteral("New Tab");
    }
    const QString name = QFileInfo(context.path).fileName();
    return name.isEmpty() ? context.path : name;
}

void MainWindow::rebuildTabBar() {
    if (!m_tabBar) {
        return;
    }
    const QSignalBlocker block(m_tabBar);
    while (m_tabBar->count() > 0) {
        m_tabBar->removeTab(0);
    }
    for (const auto& context : m_contexts) {
        const int index = m_tabBar->addTab(contextLabel(*context));
        // The close button is ours rather than the style's: the built-in one is a red
        // glyph from whatever icon theme the desktop has, which belongs to no palette
        // in this window. A themed icon also has to be redrawn when the theme changes,
        // and only the registry can do that — a stylesheet's `close-button image`
        // cannot, because the SVGs carry `currentColor`, which Qt does not resolve.
        auto* close = new QToolButton(m_tabBar);
        close->setObjectName(QStringLiteral("tabClose"));
        close->setAutoRaise(true);
        close->setCursor(Qt::ArrowCursor);
        close->setIconSize(QSize(theme::iconSize() - 4, theme::iconSize() - 4));
        close->setToolTip(QStringLiteral("Close Tab"));
        setThemedIcon(close, Icon::Close);
        connect(close, &QToolButton::clicked, this, [this, close] {
            // The strip is movable, so the tab is found by its button rather than by an
            // index captured when the button was built.
            for (int i = 0; i < m_tabBar->count(); ++i) {
                if (m_tabBar->tabButton(i, QTabBar::RightSide) == close) {
                    closeContext(i);
                    return;
                }
            }
        });
        m_tabBar->setTabButton(index, QTabBar::RightSide, close);
    }
    if (m_active >= 0 && m_active < m_tabBar->count()) {
        m_tabBar->setCurrentIndex(m_active);
    }
}

void MainWindow::onTabChanged(int index) {
    if (index < 0 || index == m_active) {
        return;
    }
    activateContext(index);
}

void MainWindow::onTabCloseRequested(int index) {
    closeContext(index);
}

void MainWindow::onNewTabRequested() {
    // The "+" opens a start-page tab, not a file dialog: the reader picks a way in
    // from there — open, clone, create, or a repository from the recent list. An
    // empty tab that is already up is brought forward instead of stacking a second —
    // but "empty" is a tab with no path, not merely one that has not been opened yet.
    // A restored tab holds a repository that simply has not been read yet, and
    // bringing it forward turns "+" into a switch to somebody else's tab.
    for (int i = 0; i < static_cast<int>(m_contexts.size()); ++i) {
        if (m_contexts[static_cast<std::size_t>(i)]->path.isEmpty()) {
            activateContext(i);
            return;
        }
    }
    m_contexts.push_back(makeContext());
    activateContext(static_cast<int>(m_contexts.size()) - 1);
}

void MainWindow::closeContext(int index) {
    if (index < 0 || index >= static_cast<int>(m_contexts.size())) {
        return;
    }
    if (m_contexts.size() == 1) {
        closeAllContexts();
        return;
    }
    const bool wasActive = index == m_active;
    // Detach the views when the tab being closed is the one on screen: the context
    // owns the models they point at, and a view left holding a freed model is the
    // shape of the crash this window already learned once.
    if (wasActive) {
        if (m_commitView) {
            m_commitView->setModel(nullptr);
        }
        if (m_refView) {
            m_refView->setModel(nullptr);
        }
    }
    m_contexts.erase(m_contexts.begin() + index);
    if (wasActive) {
        m_active = -1;
        const int next = std::min(index, static_cast<int>(m_contexts.size()) - 1);
        activateContext(next);
    } else {
        if (index < m_active) {
            --m_active;
        }
        rebuildTabBar();
        saveTabs();
    }
}

void MainWindow::onTabContextMenu(const QPoint& pos) {
    const int index = m_tabBar->tabAt(pos);
    if (index < 0) {
        return;
    }
    QMenu menu(this);
    QAction* rename = menu.addAction(QStringLiteral("Rename Tab…"));
    QAction* close = menu.addAction(QStringLiteral("Close Tab"));
    QAction* closeOthers = menu.addAction(QStringLiteral("Close Other Tabs"));
    QAction* closeAll = menu.addAction(QStringLiteral("Close All Tabs"));
    QAction* chosen = menu.exec(m_tabBar->mapToGlobal(pos));
    if (chosen == rename) {
        renameContext(index);
    } else if (chosen == close) {
        closeContext(index);
    } else if (chosen == closeOthers) {
        keepOnlyContext(index);
    } else if (chosen == closeAll) {
        closeAllContexts();
    }
}

void MainWindow::keepOnlyContext(int keep) {
    if (keep < 0 || keep >= static_cast<int>(m_contexts.size()) || m_contexts.size() == 1) {
        return;
    }
    if (keep == m_active) {
        saveActiveState();
    }
    if (m_commitView) {
        m_commitView->setModel(nullptr);
    }
    if (m_refView) {
        m_refView->setModel(nullptr);
    }
    std::unique_ptr<RepoContext> kept = std::move(m_contexts[static_cast<std::size_t>(keep)]);
    m_contexts.clear();
    m_contexts.push_back(std::move(kept));
    m_active = -1;
    activateContext(0);
}

void MainWindow::closeAllContexts() {
    if (m_commitView) {
        m_commitView->setModel(nullptr);
    }
    if (m_refView) {
        m_refView->setModel(nullptr);
    }
    m_contexts.clear();
    m_contexts.push_back(makeContext());
    m_active = -1;
    activateContext(0);
}

void MainWindow::renameContext(int index) {
    if (index < 0 || index >= static_cast<int>(m_contexts.size())) {
        return;
    }
    RepoContext* context = m_contexts[static_cast<std::size_t>(index)].get();
    if (!context->opened || m_headless) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename Tab"),
                                               QStringLiteral("Tab name:"), QLineEdit::Normal,
                                               contextLabel(*context), &ok);
    if (!ok) {
        return;
    }
    applyTabAlias(index, name.trimmed());
}

void MainWindow::applyTabAlias(int index, const QString& alias) {
    if (index < 0 || index >= static_cast<int>(m_contexts.size())) {
        return;
    }
    RepoContext* context = m_contexts[static_cast<std::size_t>(index)].get();
    context->alias = alias;
    saveTabAlias(QString::fromStdString(context->session->info().commonDir), alias);
    rebuildTabBar();
    if (index == m_active) {
        setWindowTitle(QStringLiteral("GitMycelium — %1").arg(contextLabel(*context)));
    }
    saveTabs();
}

QString MainWindow::loadTabAlias(const QString& commonDir) const {
    if (commonDir.isEmpty()) {
        return QString();
    }
    return QSettings().value(QStringLiteral("ui/tabalias/%1").arg(commonDir)).toString();
}

void MainWindow::saveTabAlias(const QString& commonDir, const QString& alias) {
    if (commonDir.isEmpty()) {
        return;
    }
    QSettings settings;
    const QString key = QStringLiteral("ui/tabalias/%1").arg(commonDir);
    if (alias.isEmpty()) {
        settings.remove(key);
    } else {
        settings.setValue(key, alias);
    }
}

void MainWindow::saveTabs() {
    if (!m_tabsPersistence || m_headless || m_restoringTabs || !isVisible()) {
        return;
    }
    QSettings settings;
    QStringList paths;
    QStringList aliases;
    int active = 0;
    int savedIndex = 0;
    for (int i = 0; i < static_cast<int>(m_contexts.size()); ++i) {
        const RepoContext& context = *m_contexts[static_cast<std::size_t>(i)];
        if (context.path.isEmpty()) {
            continue; // the empty starting tab is not a session
        }
        if (i == m_active) {
            active = savedIndex;
        }
        paths << context.path;
        aliases << context.alias;
        ++savedIndex;
    }
    settings.setValue(QStringLiteral("ui/tabs/paths"), paths);
    settings.setValue(QStringLiteral("ui/tabs/aliases"), aliases);
    settings.setValue(QStringLiteral("ui/tabs/active"), active);
}

void MainWindow::restoreTabs() {
    QSettings settings;
    const QStringList paths = settings.value(QStringLiteral("ui/tabs/paths")).toStringList();
    if (paths.isEmpty()) {
        return;
    }
    const QStringList aliases = settings.value(QStringLiteral("ui/tabs/aliases")).toStringList();
    const int savedActive = settings.value(QStringLiteral("ui/tabs/active"), 0).toInt();

    m_restoringTabs = true;
    if (m_contexts.size() == 1 && !m_contexts.front()->opened) {
        if (m_commitView) {
            m_commitView->setModel(nullptr);
        }
        if (m_refView) {
            m_refView->setModel(nullptr);
        }
        m_contexts.clear();
        m_active = -1;
    }
    for (int i = 0; i < paths.size(); ++i) {
        const QString path = paths.at(i);
        if (path.isEmpty() || contextForPath(path) >= 0) {
            continue;
        }
        auto context = makeContext();
        context->path = path;
        context->alias = i < aliases.size() ? aliases.at(i) : QString();
        context->opened = false;
        m_contexts.push_back(std::move(context));
    }
    m_restoringTabs = false;
    if (m_contexts.empty()) {
        m_contexts.push_back(makeContext());
        m_active = -1;
    }
    const int index = std::max(0, std::min(savedActive, static_cast<int>(m_contexts.size()) - 1));
    m_active = -1; // force activation, even for index 0
    activateContext(index);
}

MainWindow::~MainWindow() {
    if (m_workerAlive) {
        *m_workerAlive = false;
    }
    // The context owns the models; the views merely point at them. Detaching first
    // keeps the destruction of one from reaching for the other: a view destructor
    // disconnects from its model, and a model destructor notifies its views.
    if (m_commitView) {
        m_commitView->setModel(nullptr);
    }
    if (m_refView) {
        m_refView->setModel(nullptr);
    }
    m_contexts.clear();
}

void MainWindow::buildUi() {
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    auto* openAction = fileMenu->addAction(QStringLiteral("&Open Repository…"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::onOpenRepositoryDialog);
    auto* cloneAction = addThemedAction(fileMenu, Icon::Cloud, QStringLiteral("Clone Repository…"));
    cloneAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    connect(cloneAction, &QAction::triggered, this, &MainWindow::cloneFromDialog);
    m_commandActions.insert(QStringLiteral("app.clone"), cloneAction);
    auto* newRepoAction = addThemedAction(fileMenu, Icon::Folder,
                                          QStringLiteral("New Repository…"));
    connect(newRepoAction, &QAction::triggered, this, &MainWindow::createRepositoryFromDialog);
    m_commandActions.insert(QStringLiteral("app.new-repository"), newRepoAction);
    fileMenu->addSeparator();
    auto* newTabAction = addThemedAction(fileMenu, Icon::Plus, QStringLiteral("New &Tab"));
    newTabAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
    connect(newTabAction, &QAction::triggered, this, &MainWindow::onNewTabRequested);
    m_commandActions.insert(QStringLiteral("tab.new"), newTabAction);
    auto* closeTabAction = addThemedAction(fileMenu, Icon::Close, QStringLiteral("&Close Tab"));
    closeTabAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(closeTabAction, &QAction::triggered, this, [this] { closeContext(m_active); });
    m_commandActions.insert(QStringLiteral("tab.close"), closeTabAction);
    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction(QStringLiteral("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    m_undoAction = addThemedAction(editMenu, Icon::Undo, QStringLiteral("&Undo"));
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_undoAction->setEnabled(false);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::undoLastOperation);
    m_redoAction = addThemedAction(editMenu, Icon::Redo, QStringLiteral("&Redo"));
    m_redoAction->setShortcut(QKeySequence::Redo);
    m_redoAction->setEnabled(false);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::redoLastOperation);

    editMenu->addSeparator();
    auto* preferencesAction =
        addThemedAction(editMenu, Icon::Gear, QStringLiteral("&Preferences…"));
    preferencesAction->setShortcut(QKeySequence::Preferences);
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::openPreferences);

    // Commands that already exist as a menu action bind through it; a keymap
    // override must move that action's shortcut, not add a second one beside it.
    m_commandActions.insert(QStringLiteral("app.open"), openAction);
    m_commandActions.insert(QStringLiteral("app.quit"), quitAction);
    m_commandActions.insert(QStringLiteral("edit.undo"), m_undoAction);
    m_commandActions.insert(QStringLiteral("edit.redo"), m_redoAction);
    m_commandActions.insert(QStringLiteral("view.branches"), m_refsAction);

    auto* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    m_refsAction = addThemedAction(viewMenu, Icon::Branch, QStringLiteral("&Branches panel"));
    m_refsAction->setCheckable(true);
    m_refsAction->setChecked(true);
    m_refsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+B")));
    connect(m_refsAction, &QAction::toggled, this, &MainWindow::setRefsPanelVisible);
    m_mergedRemotesAction = viewMenu->addAction(QStringLiteral("Show Merged Remote Branches"));
    m_mergedRemotesAction->setCheckable(true);
    m_mergedRemotesAction->setChecked(
        QSettings().value(QStringLiteral("ui/showMergedRemotes"), false).toBool());
    m_mergedRemotesAction->setToolTip(
        QStringLiteral("Label remote branches whose tip is a commit on the main branch's line"));
    connect(m_mergedRemotesAction, &QAction::toggled, this, [this](bool show) {
        if (!m_headless) {
            QSettings().setValue(QStringLiteral("ui/showMergedRemotes"), show);
        }
        if (m_commitModel) {
            m_commitModel->setShowMergedRemotes(show);
        }
    });
    m_mergedCommitsAction = viewMenu->addAction(QStringLiteral("Show Commits of Merged Branches"));
    m_mergedCommitsAction->setCheckable(true);
    m_mergedCommitsAction->setChecked(
        QSettings().value(QStringLiteral("ui/showMergedCommits"), true).toBool());
    m_mergedCommitsAction->setToolTip(QStringLiteral(
        "Off: a merged branch that no longer exists shows as its merge commit alone"));
    connect(m_mergedCommitsAction, &QAction::toggled, this, [this](bool show) {
        if (!m_headless) {
            QSettings().setValue(QStringLiteral("ui/showMergedCommits"), show);
        }
        if (m_commitModel) {
            applyVisibility();
        }
    });
    m_detailsAction = viewMenu->addAction(QStringLiteral("&Details panel"));
    m_detailsAction->setCheckable(true);
    m_detailsAction->setChecked(true);
    connect(m_detailsAction, &QAction::toggled, this, [this](bool visible) {
        if (m_history) {
            m_history->setPanelCollapsed(m_history->count() - 1, !visible);
        }
    });

    m_animateAction = viewMenu->addAction(QStringLiteral("Animate Graph"));
    m_animateAction->setCheckable(true);
    m_animateAction->setChecked(true);
    connect(m_animateAction, &QAction::toggled, this, &MainWindow::setGraphAnimationsEnabled);
    viewMenu->addSeparator();
    m_searchAction = addThemedAction(viewMenu, Icon::Search, QStringLiteral("Find in History…"));
    m_searchAction->setShortcut(QKeySequence::Find);
    connect(m_searchAction, &QAction::triggered, this, [this] { showCommandPalette(true); });
    m_commandActions.insert(QStringLiteral("view.search"), m_searchAction);
    auto* reflogAction = addThemedAction(viewMenu, Icon::Command, QStringLiteral("&Reflog"));
    connect(reflogAction, &QAction::triggered, this, &MainWindow::showReflog);
    m_commandActions.insert(QStringLiteral("history.reflog"), reflogAction);

    auto* branchMenu = menuBar()->addMenu(QStringLiteral("&Branch"));
    addThemedAction(branchMenu, Icon::Branch, QStringLiteral("New Branch…"), this,
                    &MainWindow::createBranchFromSelection);
    addThemedAction(branchMenu, Icon::Checkout, QStringLiteral("Checkout"), this,
                    &MainWindow::checkoutSelectedBranch);
    addThemedAction(branchMenu, Icon::Merge, QStringLiteral("Merge…"), this,
                    &MainWindow::mergeBranchIntoCurrent);
    addThemedAction(branchMenu, Icon::Merge, QStringLiteral("Squash Merge…"), this,
                    &MainWindow::squashMergeIntoCurrent);
    QMenu* visibilityMenu = branchMenu->addMenu(QStringLiteral("Visibility"));
    m_smartAction = visibilityMenu->addAction(QStringLiteral("Focus on Current Branch"));
    m_smartAction->setCheckable(true);
    connect(m_smartAction, &QAction::toggled, this, [this](bool on) {
        if (m_visibility.smart() != on) {
            toggleSmart();
        }
    });
    visibilityMenu->addSeparator();
    visibilityMenu->addAction(QStringLiteral("My branches"), this,
                              [this] { applyPreset(VisibilityPreset::Mine); });
    visibilityMenu->addAction(QStringLiteral("Active in 30 days"), this,
                              [this] { applyPreset(VisibilityPreset::Recent30); });
    visibilityMenu->addAction(QStringLiteral("Branches by prefix…"), this,
                              &MainWindow::applyPrefixPreset);
    visibilityMenu->addAction(QStringLiteral("Branches without upstream"), this,
                              [this] { applyPreset(VisibilityPreset::NoUpstream); });
    visibilityMenu->addAction(QStringLiteral("Branches ahead of upstream"), this,
                              [this] { applyPreset(VisibilityPreset::Ahead); });
    visibilityMenu->addAction(QStringLiteral("Branches behind upstream"), this,
                              [this] { applyPreset(VisibilityPreset::Behind); });
    visibilityMenu->addSeparator();
    visibilityMenu->addAction(QStringLiteral("Show all branches"), this,
                              &MainWindow::clearVisibility);
    branchMenu->addSeparator();
    addThemedAction(branchMenu, Icon::Plus, QStringLiteral("New Worktree…"), this,
                    &MainWindow::createWorktree);
    addThemedAction(branchMenu, Icon::Close, QStringLiteral("Prune Worktrees"), this,
                    &MainWindow::pruneWorktrees);
    QMenu* submoduleMenu = branchMenu->addMenu(QStringLiteral("Submodules"));
    setThemedIcon(submoduleMenu, Icon::Submodule);
    submoduleMenu->addAction(QStringLiteral("Add Submodule…"), this, &MainWindow::addSubmodule);
    submoduleMenu->addAction(QStringLiteral("Initialize All"), this,
                             &MainWindow::initializeAllSubmodules);
    submoduleMenu->addAction(QStringLiteral("Update All"), this,
                             &MainWindow::updateAllSubmodules);
    submoduleMenu->addAction(QStringLiteral("Sync All"), this, &MainWindow::syncAllSubmodules);
    submoduleMenu->addSeparator();
    m_submoduleAutoupdateAction =
        submoduleMenu->addAction(QStringLiteral("Update after fetch/checkout/merge"));
    m_submoduleAutoupdateAction->setCheckable(true);
    connect(m_submoduleAutoupdateAction, &QAction::toggled, this,
            &MainWindow::setSubmoduleAutoupdate);
    m_submoduleAutoupdateRemoteAction =
        submoduleMenu->addAction(QStringLiteral("  …tracking the submodule's remote branch"));
    m_submoduleAutoupdateRemoteAction->setCheckable(true);
    connect(m_submoduleAutoupdateRemoteAction, &QAction::toggled, this,
            &MainWindow::setSubmoduleAutoupdateRemote);
    branchMenu->addSeparator();
    addThemedAction(branchMenu, Icon::Close, QStringLiteral("Delete Branch"), this,
                    &MainWindow::deleteSelectedBranch);
    addThemedAction(branchMenu, Icon::Close, QStringLiteral("Delete Merged Branches…"), this,
                    &MainWindow::deleteMergedBranches);
    branchMenu->addSeparator();
    m_preflightAction =
        branchMenu->addAction(QStringLiteral("Check for conflicts before merging"));
    m_preflightAction->setCheckable(true);
    m_preflightAction->setChecked(m_preflightEnabled);
    connect(m_preflightAction, &QAction::toggled, this, &MainWindow::setPreflightEnabled);
    m_rerereAction = branchMenu->addAction(QStringLiteral("Remember conflict resolutions (rerere)"));
    m_rerereAction->setCheckable(true);
    connect(m_rerereAction, &QAction::toggled, this, &MainWindow::setRerereEnabled);
    m_rerereAutoupdateAction = branchMenu->addAction(QStringLiteral("  …and stage them automatically"));
    m_rerereAutoupdateAction->setCheckable(true);
    connect(m_rerereAutoupdateAction, &QAction::toggled, this, &MainWindow::setRerereAutoupdate);
    branchMenu->addSeparator();
    m_preventionAction =
        branchMenu->addAction(QStringLiteral("Watch for conflicts with the base branch"));
    m_preventionAction->setCheckable(true);
    m_preventionAction->setChecked(m_preventionEnabled);
    connect(m_preventionAction, &QAction::toggled, this, &MainWindow::setPreventionEnabled);
    addThemedAction(branchMenu, Icon::Alert,
                    QStringLiteral("Check conflicts with the base branch now"), this,
                    [this] { checkConflictsWithBase(false); });

    m_remoteMenu = menuBar()->addMenu(QStringLiteral("&Remote"));
    addThemedAction(m_remoteMenu, Icon::Fetch, QStringLiteral("Fetch"), this,
                    [this] { startSync(SyncKind::Fetch); });
    addThemedAction(m_remoteMenu, Icon::Pull, QStringLiteral("Pull"), this,
                    [this] { startSync(SyncKind::Pull); });
    addThemedAction(m_remoteMenu, Icon::Push, QStringLiteral("Push"), this,
                    [this] { startSync(SyncKind::Push); });
    addThemedAction(m_remoteMenu, Icon::Fetch, QStringLiteral("Fetch All"), this,
                    [this] { startSync(SyncKind::FetchAll); });
    m_remoteMenu->addSeparator();
    m_remoteMenu->addAction(QStringLiteral("Push Tags"), this,
                            [this] { startSync(SyncKind::PushTags); });
    m_remoteMenu->addSeparator();
    m_forcePushAction = m_remoteMenu->addAction(QStringLiteral("Force Push (with lease)…"), this,
                                                &MainWindow::forcePushInteractive);
    m_autoFetchMenu = m_remoteMenu->addMenu(QStringLiteral("Auto-fetch"));
    m_autoFetchAction = m_autoFetchMenu->addAction(QStringLiteral("Enabled (every 5 min)"));
    m_autoFetchAction->setCheckable(true);
    m_autoFetchAction->setChecked(m_autoFetchEnabled);
    connect(m_autoFetchAction, &QAction::toggled, this, &MainWindow::setAutoFetch);
    m_autoFetchSeparator = m_autoFetchMenu->addSeparator();
    // The per-remote entries are added by updateAutoFetchMenu(): the remote list
    // changes on every refresh, and a menu built once would go stale.

    auto* repoMenu = menuBar()->addMenu(QStringLiteral("&Repository"));
    addThemedAction(repoMenu, Icon::FileCode, QStringLiteral("Apply Patch…"), this,
                    &MainWindow::applyPatchInteractive);
    addThemedAction(repoMenu, Icon::Terminal, QStringLiteral("Open in Terminal"), this,
                    &MainWindow::openTerminal);
    addThemedAction(repoMenu, Icon::Gear, QStringLiteral("Hooks…"), this, &MainWindow::openHooks);
    repoMenu->addAction(QStringLiteral("Local Ignore Rules…"), this, &MainWindow::openLocalIgnore);
    addThemedAction(repoMenu, Icon::Folder, QStringLiteral("Sparse Checkout…"), this,
                    &MainWindow::editSparseCheckout);
    addThemedAction(repoMenu, Icon::Command, QStringLiteral("Identity Profiles…"), this,
                    &MainWindow::editProfiles);
    addThemedAction(repoMenu, Icon::Branch, QStringLiteral("Gitflow…"), this,
                    &MainWindow::openGitFlow);
    addThemedAction(repoMenu, Icon::Paperclip, QStringLiteral("Git LFS…"), this,
                    &MainWindow::openLfs);
    repoMenu->addSeparator();
    addThemedAction(repoMenu, Icon::Gear, QStringLiteral("Optimize Repository…"), this,
                    [this] { runMaintenance(MaintenanceTask::Optimize); });
    addThemedAction(repoMenu, Icon::Cloud, QStringLiteral("Run Scheduled Maintenance"), this,
                    [this] { runMaintenance(MaintenanceTask::Maintenance); });
    repoMenu->addSeparator();
    addThemedAction(repoMenu, Icon::Terminal, QStringLiteral("Garbage Collect (gc)"), this,
                    [this] { runMaintenance(MaintenanceTask::Gc); });
    addThemedAction(repoMenu, Icon::Check, QStringLiteral("Check Integrity (fsck)"), this,
                    [this] { runMaintenance(MaintenanceTask::Fsck); });

    auto* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    auto* about = addThemedAction(helpMenu, Icon::Info, QStringLiteral("&About GitMycelium"));
    connect(about, &QAction::triggered, this, [this] { showAboutBox(this); });
    addThemedAction(helpMenu, Icon::Pull, QStringLiteral("Check for &Updates…"), this,
                    [this] { showUpdateDialog(); });

    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, this, [this] {
        if (m_centerStack && m_centerStack->currentWidget() == m_diffView) {
            showGraphPage();
        }
    });

    auto* paletteShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+P")), this);
    connect(paletteShortcut, &QShortcut::activated, this, [this] { showCommandPalette(false); });

    // The tab strip is a toolbar, not part of the central widget: a QMainWindow
    // always lays its toolbars above the central widget, and the tabs have to sit
    // above the main toolbar, the way every tabbed client arranges them.
    buildTabBar();
    buildToolBar();
    m_refPanel = buildReferencePanel();
    buildCommitPanel();
    buildChangesPanel();
    refreshCommands();

    m_centerStack = new QStackedWidget(this);
    m_centerStack->addWidget(m_commitView);
    m_centerStack->addWidget(m_diffView);
    sizeToCurrentPage(m_centerStack);

    auto* history = new PanelSplitter(this);
    m_history = history;
    history->addWidget(m_refPanel);
    history->addWidget(m_centerStack);
    history->addWidget(m_details);
    history->setCollapsible(1, false);
    connect(history, &PanelSplitter::panelCollapsedChanged, this, [this](int index, bool collapsed) {
        if (m_foldingPanel) {
            return;
        }
        if (index == 0) {
            m_refsVisible = !collapsed;
            m_refsHiddenForDiff = false;
            if (m_refsAction && m_refsAction->isChecked() == collapsed) {
                const QSignalBlocker block(m_refsAction);
                m_refsAction->setChecked(!collapsed);
            }
        } else if (m_detailsAction && m_detailsAction->isChecked() == collapsed) {
            const QSignalBlocker block(m_detailsAction);
            m_detailsAction->setChecked(!collapsed);
        }
    });
    history->setStretchFactor(0, 1);
    history->setStretchFactor(1, 4);
    history->setStretchFactor(2, 3);
    history->setSizes({230, 750, 420});

    m_conflictView = new ConflictView(m_session, this);
    connect(m_conflictView, &ConflictView::repositoryChanged, this, [this] {
        refreshPanels();
    });
    connect(m_conflictView, &ConflictView::mergeCompleted, this, [this] {
        m_commitModel->reset();
        refreshPanels();
        showHistoryPage();
        setStatus(QStringLiteral("Merge committed."));
    });
    connect(m_conflictView, &ConflictView::mergeAborted, this, [this] {
        m_commitModel->reset();
        refreshPanels();
        showHistoryPage();
        setStatus(QStringLiteral("Merge aborted."));
    });
    connect(m_conflictView, &ConflictView::externalMergeRequested, this,
            [this](const QString& path) { openExternalMerge(path); });
    connect(m_conflictView, &ConflictView::closeRequested, this,
            &MainWindow::onConflictPageClosed);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(history);
    m_stack->addWidget(m_conflictView);
    m_startPage = new StartPage(this);
    m_stack->addWidget(m_startPage);
    // The merge editor has its own Ctrl+F; two live bindings for one key fire neither.
    connect(m_stack, &QStackedWidget::currentChanged, this, [this] {
        m_searchAction->setEnabled(m_stack->currentWidget() != m_conflictView);
    });
    connect(m_startPage, &StartPage::openRequested, this, &MainWindow::onOpenRepositoryDialog);
    connect(m_startPage, &StartPage::cloneRequested, this, &MainWindow::cloneFromDialog);
    connect(m_startPage, &StartPage::createRequested, this,
            &MainWindow::createRepositoryFromDialog);
    connect(m_startPage, &StartPage::recentActivated, this,
            [this](const QString& path) { openRepository(path); });
    sizeToCurrentPage(m_stack);
    setCentralWidget(m_stack);


    m_branchLabel = new QLabel(QStringLiteral("no repository"), this);
    m_branchLabel->setObjectName(QStringLiteral("statusBranch"));
    m_branchLabel->setMinimumWidth(theme::scaledPixels(160));
    m_branchSkeleton = new Skeleton(m_branchLabel, Skeleton::Shape::Line);
    statusBar()->addPermanentWidget(m_branchLabel);

    // The graph can be narrowed to one branch or to "everything but these". A
    // reader who forgot they hid a branch must be able to see that, and undo it in
    // one click — otherwise the graph looks broken rather than filtered.
    m_visibilityButton = new QToolButton(this);
    m_visibilityButton->setAutoRaise(true);
    m_visibilityButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_visibilityButton->setVisible(false);
    connect(m_visibilityButton, &QToolButton::clicked, this, &MainWindow::clearVisibility);
    statusBar()->addPermanentWidget(m_visibilityButton);

    // A background check that finds nothing says nothing; one that finds a newer
    // build leaves a button here rather than interrupting with a dialog.
    m_updater = new update::Updater(this);
    m_updateButton = new QToolButton(this);
    m_updateButton->setObjectName(QStringLiteral("updateAvailable"));
    m_updateButton->setAutoRaise(true);
    m_updateButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setThemedIcon(m_updateButton, Icon::Pull);
    m_updateButton->setVisible(false);
    connect(m_updateButton, &QToolButton::clicked, this, &MainWindow::showUpdateDialog);
    connect(m_updater, &update::Updater::checked, this, [this](bool ok, const QString&) {
        const auto& manifest = m_updater->manifest();
        const bool available = ok && m_updater->updateAvailable();
        if (available) {
            m_updateButton->setText(
                QStringLiteral("Update to %1")
                    .arg(update::describeVersion(manifest->version, manifest->build)));
        }
        m_updateButton->setVisible(available);
    });
    statusBar()->addPermanentWidget(m_updateButton);
}

void MainWindow::buildTabBar() {
    auto* tabs = new QToolBar(QStringLiteral("Tabs"), this);
    tabs->setObjectName(QStringLiteral("tabsToolBar"));
    tabs->setMovable(false);
    tabs->setFloatable(false);
    tabs->setIconSize(QSize(theme::iconSize(), theme::iconSize()));
    addToolBar(tabs);
    // Without a break the next toolbar would share this row, putting the main
    // toolbar beside the tabs instead of below them.
    addToolBarBreak();

    m_tabBar = new QTabBar(tabs);
    m_tabBar->setObjectName(QStringLiteral("repositoryTabBar"));
    m_tabBar->setExpanding(false);
    m_tabBar->setMovable(true);
    m_tabBar->setTabsClosable(true);
    m_tabBar->setDrawBase(false);
    m_tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tabBar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    tabs->addWidget(m_tabBar);

    auto* newTabButton = new QToolButton(tabs);
    newTabButton->setObjectName(QStringLiteral("newTabButton"));
    newTabButton->setText(QStringLiteral("+"));
    newTabButton->setToolTip(QStringLiteral("New tab — start page (Ctrl+T)"));
    newTabButton->setAutoRaise(true);
    tabs->addWidget(newTabButton);

    auto* spacer = new QWidget(tabs);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tabs->addWidget(spacer);

    connect(m_tabBar, &QTabBar::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabBar, &QTabBar::tabCloseRequested, this, &MainWindow::onTabCloseRequested);
    connect(m_tabBar, &QWidget::customContextMenuRequested, this, &MainWindow::onTabContextMenu);
    connect(newTabButton, &QToolButton::clicked, this, &MainWindow::onNewTabRequested);
}

void MainWindow::buildToolBar() {
    auto* toolbar = addToolBar(QStringLiteral("Main"));
    toolbar->setObjectName(QStringLiteral("mainToolBar"));
    m_toolBar = toolbar;
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(theme::iconSize(), theme::iconSize()));
    toolbar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);

    const auto addIcon = [&](Icon which, const QString& text, const QString& tip, IconTint tint) {
        QAction* action = toolbar->addAction(text);
        setThemedIcon(action, which, tint);
        action->setToolTip(tip);
        return action;
    };
    const auto caption = [](QAction* action, const QString& word) { action->setIconText(word); };

    toolbar->addAction(m_undoAction);
    toolbar->addAction(m_redoAction);
    caption(m_undoAction, QStringLiteral("Undo"));
    caption(m_redoAction, QStringLiteral("Redo"));
    toolbar->addSeparator();

    m_fetchAction = addIcon(Icon::Fetch, QStringLiteral("Fetch"),
                            QStringLiteral("git fetch --prune from the selected remote"),
                            IconTint::Text);
    connect(m_fetchAction, &QAction::triggered, this, [this] { startSync(SyncKind::Fetch); });

    m_pullAction = new QAction(QStringLiteral("Pull"), this);
    setThemedIcon(m_pullAction, Icon::Pull);
    connect(m_pullAction, &QAction::triggered, this, [this] { startSync(SyncKind::Pull); });
    m_pullButton = new QToolButton(toolbar);
    m_pullButton->setObjectName(QStringLiteral("pullButton"));
    m_pullButton->setDefaultAction(m_pullAction);
    m_pullButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_pullButton->setPopupMode(QToolButton::MenuButtonPopup);
    // A menu button reserves room for the arrow inside its own width; the property
    // lets the stylesheet give that arrow its own space instead of squeezing the icon.
    m_pullButton->setProperty("variant", QStringLiteral("menu"));
    auto* pullMenu = new QMenu(m_pullButton);
    const auto addPullMode = [&](const QString& text, PullMode mode) {
        QAction* entry = pullMenu->addAction(text);
        entry->setCheckable(true);
        entry->setData(static_cast<int>(mode));
        connect(entry, &QAction::triggered, this, [this, mode] { setPullMode(mode); });
        return entry;
    };
    addPullMode(QStringLiteral("Pull (fast-forward only)"), PullMode::FfOnly);
    addPullMode(QStringLiteral("Pull (merge)"), PullMode::Merge);
    addPullMode(QStringLiteral("Pull (rebase)"), PullMode::Rebase);
    m_pullButton->setMenu(pullMenu);
    toolbar->addWidget(m_pullButton);

    m_pushAction = new QAction(QStringLiteral("Push"), this);
    setThemedIcon(m_pushAction, Icon::Push);
    m_pushAction->setToolTip(
        QStringLiteral("git push; sets upstream tracking on first push"));
    connect(m_pushAction, &QAction::triggered, this, [this] { startSync(SyncKind::Push); });
    m_pushButton = new QToolButton(toolbar);
    m_pushButton->setObjectName(QStringLiteral("pushButton"));
    m_pushButton->setDefaultAction(m_pushAction);
    m_pushButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_pushButton->setPopupMode(QToolButton::MenuButtonPopup);
    m_pushButton->setProperty("variant", QStringLiteral("menu"));
    auto* pushMenu = new QMenu(m_pushButton);
    m_forcePushAction = pushMenu->addAction(QStringLiteral("Force Push (with lease)…"));
    connect(m_forcePushAction, &QAction::triggered, this, &MainWindow::forcePushInteractive);
    m_pushButton->setMenu(pushMenu);
    toolbar->addWidget(m_pushButton);

    m_remotePicker = new QComboBox(toolbar);
    m_remotePicker->setToolTip(QStringLiteral("Remote used by Fetch, Pull and Push"));
    m_remotePicker->setMinimumContentsLength(6);
    toolbar->addWidget(m_remotePicker);
    toolbar->addSeparator();
    setPullMode(m_pullMode);

    auto* newBranch = addIcon(Icon::Branch, QStringLiteral("New Branch…"),
                              QStringLiteral("Create a branch at the selected commit"), IconTint::Text);
    connect(newBranch, &QAction::triggered, this, &MainWindow::createBranchFromSelection);
    caption(newBranch, QStringLiteral("Branch"));

    auto* merge = addIcon(Icon::Merge, QStringLiteral("Merge…"),
                          QStringLiteral("Merge a branch into the current one"), IconTint::Text);
    connect(merge, &QAction::triggered, this, &MainWindow::mergeBranchIntoCurrent);
    caption(merge, QStringLiteral("Merge"));

    auto* checkout = addIcon(Icon::Checkout, QStringLiteral("Checkout"),
                             QStringLiteral("Switch to the selected branch"), IconTint::Text);
    connect(checkout, &QAction::triggered, this, &MainWindow::checkoutSelectedBranch);
    toolbar->addSeparator();

    m_stashAction = addIcon(Icon::Stash, QStringLiteral("Stash"),
                            QStringLiteral("Stash the working changes"), IconTint::Text);
    connect(m_stashAction, &QAction::triggered, this, &MainWindow::stashWorkingTree);
    m_popStashAction = addIcon(Icon::Pop, QStringLiteral("Pop Stash"),
                               QStringLiteral("Restore the most recent stash"), IconTint::Text);
    connect(m_popStashAction, &QAction::triggered, this, &MainWindow::popTopStash);
    caption(m_popStashAction, QStringLiteral("Pop"));

    auto* conflicts = addIcon(Icon::Conflicts, QStringLiteral("Conflicts"),
                              QStringLiteral("Open the three-pane merge editor"), IconTint::Text);
    connect(conflicts, &QAction::triggered, this, &MainWindow::showConflictsPage);

    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    auto* palette = addIcon(Icon::Search, QStringLiteral("Command Palette"),
                            QStringLiteral("Command palette (Ctrl+P)"), IconTint::Text);
    connect(palette, &QAction::triggered, this, [this] { showCommandPalette(false); });
    caption(palette, QStringLiteral("Search"));
    m_notificationsAction = addIcon(Icon::Alert, QStringLiteral("Notifications"),
                                    QStringLiteral("Recent notifications"), IconTint::Text);
    connect(m_notificationsAction, &QAction::triggered, this, &MainWindow::showNotifications);
    caption(m_notificationsAction, QStringLiteral("Alerts"));
    connect(&m_notifications, &NotificationLog::changed, this, [this] {
        if (!m_notificationsAction) {
            return;
        }
        const int problems = m_notifications.problemCount();
        m_notificationsAction->setToolTip(
            problems > 0 ? QStringLiteral("Recent notifications (%1 needing attention)").arg(problems)
                         : QStringLiteral("Recent notifications"));
    });
    auto* preferences = addIcon(Icon::Gear, QStringLiteral("Preferences"),
                                QStringLiteral("Preferences (Ctrl+,)"), IconTint::Text);
    connect(preferences, &QAction::triggered, this, &MainWindow::openPreferences);
    caption(preferences, QStringLiteral("Settings"));
}

QWidget* MainWindow::buildReferencePanel() {
    auto* panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("refRail"));
    panel->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* filterRow = new QWidget(panel);
    auto* filterLayout = new QHBoxLayout(filterRow);
    filterLayout->setContentsMargins(4, 4, 4, 4);
    filterLayout->setSpacing(4);

    m_refFilter = new QLineEdit(filterRow);
    m_refFilter->setPlaceholderText(QStringLiteral("Filter branches…"));
    m_refFilter->setClearButtonEnabled(true);
    m_refFilter->setToolTip(QStringLiteral("Substring match, case does not matter"));
    QAction* filterIcon =
        m_refFilter->addAction(icon(Icon::Search, theme::textDim()), QLineEdit::LeadingPosition);
    setThemedIcon(filterIcon, Icon::Search, IconTint::Dim);

    m_refFilterRegex = new QToolButton(filterRow);
    m_refFilterRegex->setText(QStringLiteral(".*"));
    m_refFilterRegex->setCheckable(true);
    m_refFilterRegex->setToolTip(
        QStringLiteral("Treat the filter as a regular expression instead of a substring"));
    m_refFilterRegex->setFixedWidth(30);

    filterLayout->addWidget(m_refFilter, 1);
    filterLayout->addWidget(m_refFilterRegex);

    auto* refView = new RefTreeView(panel);
    m_refView = refView;
    m_refSkeleton = new Skeleton(refView->viewport(), Skeleton::Shape::List);
    refView->setModel(m_refModel);
    refView->setUniformRowHeights(true);
    refView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    refView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    refView->setSelectionBehavior(QAbstractItemView::SelectRows);
    refView->header()->setStretchLastSection(true);
    refView->setContextMenuPolicy(Qt::CustomContextMenu);
    refView->setToolTip(QStringLiteral(
        "Double-click a branch to check it out.\nDrag a branch onto another branch to merge or "
        "rebase, or onto a commit to move it there."));
    refView->setDragEnabled(true);
    refView->setAcceptDrops(true);
    refView->setDropIndicatorShown(true);
    refView->setDragDropMode(QAbstractItemView::DragDrop);
    refView->setDefaultDropAction(Qt::MoveAction);
    refView->setColorResolver(
        [this](const QString& name) { return m_commitModel->refColorIndex(name); });
    connect(refView, &QTreeView::customContextMenuRequested, this, &MainWindow::onRefContextMenu);
    connect(refView, &RefTreeView::eyeClicked, this, &MainWindow::toggleEye);
    connect(refView, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        if (RefModel::isStash(index)) {
            restoreStash(index.data(RefModel::StashOidRole).toString(), false,
                         index.data(Qt::DisplayRole).toString());
            return;
        }
        if (RefModel::isWorktree(index)) {
            openWorktree(index.data(RefModel::WorktreePathRole).toString());
            return;
        }
        if (RefModel::isSubmodule(index)) {
            openSubmodule(index.data(RefModel::SubmodulePathRole).toString());
            return;
        }
        checkoutSelectedBranch();
    });
    connect(refView, &RefTreeView::refFocused, this, [this](const QString& name) {
        m_commitModel->setFocusedBranch(name);
    });
    connect(refView, &RefTreeView::refHovered, this, [this](const QString& name) {
        m_commitModel->setHighlightedBranch(name);
    });
    connect(m_refFilter, &QLineEdit::textChanged, this, [this] { applyRefFilter(); });
    connect(m_refFilterRegex, &QToolButton::toggled, this, [this] { applyRefFilter(); });

    layout->addWidget(filterRow);
    layout->addWidget(refView, 1);
    return panel;
}

void MainWindow::buildCommitPanel() {
    m_commitView = new CommitGraphView(this);
    m_graphSkeleton = new Skeleton(m_commitView->viewport(), Skeleton::Shape::Graph);
    m_commitView->setModel(m_commitModel);
    m_commitView->setUniformRowHeights(true);
    m_commitView->setRootIsDecorated(false);
    m_commitView->setAlternatingRowColors(true);
    m_commitView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_commitView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto* refsDelegate = new RefsDelegate(this);
    refsDelegate->setLabelAlphaHook(
        [this](const QString& name) { return m_commitView->animationLabelAlpha(name); });
    m_commitView->setItemDelegateForColumn(CommitModel::Refs, refsDelegate);
    auto* graphDelegate = new GraphDelegate(this);
    graphDelegate->setAnimationHooks(
        [this](const QString& oid) { return m_commitView->animationFadeAlpha(oid); },
        [this](const QString& oid) { return m_commitView->animationIsMorphing(oid); });
    m_commitView->setItemDelegateForColumn(CommitModel::Graph, graphDelegate);
    m_commitView->setAcceptedMimeType(QString::fromLatin1(kRefMimeType));

    connect(m_commitView, &CommitGraphView::refDroppedOnRef, this,
            &MainWindow::onRefDroppedOnRef);
    // Hovering a label — or a ghost badge — in the graph lights that branch's
    // commits and dims the rest. The view holds no model pointer of its own, so it
    // reports the name and the window decides.
    connect(m_commitView, &CommitGraphView::refHovered, this, [this](const QString& name) {
        m_commitModel->setHighlightedBranch(name);
    });
    // The list is asking for its prediction; answer before it is shown. git access
    // lives here, not in the view.
    connect(m_commitView, &CommitGraphView::dropOperationsOpened, this,
            [this](const QString& source, const QString& target) {
                bool warns = false;
                const QString text = dropPreviewText(source, target, &warns);
                m_commitView->setDropPreview(text, warns);
            });
    // The badge flows report the operation once the reader has picked it from the list
    // beside the badge, which is why these go straight to the actions rather than
    // through the menu handler. The list already carried the prediction, so the modal
    // pre-flight is skipped.
    connect(m_commitView, &CommitGraphView::mergeRequested, this,
            [this](const QString& source, const QString& target) {
                mergeBranchInto(source, target, true);
            });
    connect(m_commitView, &CommitGraphView::rebaseRequested, this,
            [this](const QString& source, const QString& target) {
                rebaseBranchOnto(source, target, true);
            });
    connect(m_commitView, &CommitGraphView::squashRequested, this,
            [this](const QString& source, const QString& target) {
                squashMergeBranch(source, target, true);
            });
    connect(m_commitView, &CommitGraphView::interactiveRebaseRequested, this,
            [this](const QString& source, const QString& target) {
                interactiveRebaseFromDrop(source, target);
            });
    connect(m_commitView, &CommitGraphView::checkoutTargetRequested, this,
            [this](const QString& target) { checkoutBranchFromGraph(target, RefKind::LocalBranch); });
    connect(m_commitView, &CommitGraphView::refDroppedOnCommit, this,
            &MainWindow::onRefDroppedOnCommit);

    QHeaderView* header = m_commitView->header();
    applyCommitColumnModes();
    // The list is labels | graph | message, and nothing else: author,
    // date and hash belong to the details panel. Keeping them here squeezed the
    // message column down to an ellipsis, which is worse than not showing them.
    m_commitView->setColumnHidden(CommitModel::Author, true);
    m_commitView->setColumnHidden(CommitModel::Date, true);
    m_commitView->setColumnHidden(CommitModel::Sha, true);

    const QFontMetrics metrics(m_commitView->font());
    const int padding = 18;
    m_commitView->setColumnWidth(CommitModel::Refs, 260);
    m_commitView->setColumnWidth(CommitModel::Graph, GraphDelegate::widthForLanes(5));
    m_commitView->setColumnWidth(
        CommitModel::Date,
        metrics.horizontalAdvance(QStringLiteral("2026-10-06 02:59")) + padding);
    m_commitView->setColumnWidth(
        CommitModel::Sha, metrics.horizontalAdvance(QStringLiteral("0123456789")) + padding);
    m_commitView->setColumnWidth(CommitModel::Author, 150);

    header->setSectionsMovable(true);
    // A drag fires sectionResized many times; coalesce the writes so the profile is
    // touched once when the reader lets go of the divider.
    m_columnSaveTimer = new QTimer(this);
    m_columnSaveTimer->setSingleShot(true);
    m_columnSaveTimer->setInterval(400);
    connect(m_columnSaveTimer, &QTimer::timeout, this, &MainWindow::saveCommitColumns);
    connect(header, &QHeaderView::sectionMoved, this, [this] { m_columnSaveTimer->start(); });
    connect(header, &QHeaderView::sectionResized, this, [this] { m_columnSaveTimer->start(); });
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QHeaderView::customContextMenuRequested, this,
            &MainWindow::showCommitColumnMenu);

    connect(m_commitView, &CommitGraphView::refFocused, this, [this](const QString& name) {
        m_commitModel->setFocusedBranch(name);
    });
    connect(m_commitView, &CommitGraphView::branchCheckoutRequested, this,
            [this](const QString& name, RefKind kind) { checkoutBranchFromGraph(name, kind); });
    connect(m_commitView, &CommitGraphView::commitContextMenuRequested, this,
            &MainWindow::onCommitContextMenu);
    connect(header, &QHeaderView::sectionResized, this,
            [this](int section, int, int) {
                if (section == CommitModel::Graph && m_commitView->isVisible()) {
                    m_graphColumnUserSized = true;
                }
                if (section == CommitModel::Graph) {
                    // Repaint at the new width. The graph is drawn cell by cell,
                    // and a resize that leaves the old drawing on screen is seen
                    // as graph lines smeared through the commit messages.
                    m_commitView->viewport()->update();
                }
            });
}

void MainWindow::buildChangesPanel() {
    m_details = new CommitDetailsPanel(this);
    connect(m_details, &CommitDetailsPanel::fileSelected, this,
            [this](const QString& path, bool staged) {
                renderFile(path, m_details->showingWorkingTree(), staged);
            });
    connect(m_details, &CommitDetailsPanel::fileActivated, this,
            [this](const QString& path, bool staged) {
                renderFile(path, m_details->showingWorkingTree(), staged);
                showDiffPage();
            });
    connect(m_details, &CommitDetailsPanel::messageAmendRequested, this,
            [this](const QString& message) { amendSelectedCommitMessage(message); });
    connect(m_details, &CommitDetailsPanel::commitActivated, this,
            [this](const QString& commitId) { selectCommit(commitId); });

    connect(m_details, &CommitDetailsPanel::stageRequested, this,
            [this](const QStringList& paths) { applyToFiles(paths, StageOp::Stage); });
    connect(m_details, &CommitDetailsPanel::unstageRequested, this,
            [this](const QStringList& paths) { applyToFiles(paths, StageOp::Unstage); });
    connect(m_details, &CommitDetailsPanel::discardRequested, this,
            [this](const QStringList& paths) { applyToFiles(paths, StageOp::Discard); });
    connect(m_details, &CommitDetailsPanel::stashRequested, this,
            [this](const QStringList& paths) { stashFiles(paths); });
    connect(m_details, &CommitDetailsPanel::stageAllRequested, this,
            [this] { stageAllChanges(); });
    connect(m_details, &CommitDetailsPanel::unstageAllRequested, this,
            [this] { applyToFiles(m_details->stagedPaths(), StageOp::Unstage); });
    connect(m_details, &CommitDetailsPanel::discardAllRequested, this,
            [this] { discardAllChanges(); });
    connect(m_details, &CommitDetailsPanel::stashAllRequested, this,
            [this](const QString& message) { stashAllChanges(message); });
    connect(m_details, &CommitDetailsPanel::amendToggled, this, [this](bool on) {
        if (on) {
            m_details->setCommitMessage(headCommitMessage());
        }
    });
    connect(m_details, &CommitDetailsPanel::commitRequested, this,
            [this](const QString& message) { commitStaged(message); });
    connect(m_details, &CommitDetailsPanel::coauthorsRequested, this,
            [this] { onCoauthorsRequested(); });
    connect(m_details, &CommitDetailsPanel::signChanged, this, [this](bool on) {
        if (!m_session || !m_session->isOpen()) {
            return;
        }
        QString error;
        if (!m_session->setConfigValue(QStringLiteral("commit.gpgsign"),
                                       on ? QStringLiteral("true") : QStringLiteral("false"),
                                       &error)) {
            reportFailure(QStringLiteral("Cannot change signing"), error);
            return;
        }
        setStatus(on ? QStringLiteral("Commits will be signed.")
                     : QStringLiteral("Commits will not be signed."));
    });
    connect(m_details, &CommitDetailsPanel::ignoreRequested, this,
            [this](const QStringList& paths, const QString& scope, const QString& kind) {
                onIgnoreRequested(paths, scope, kind);
            });

    m_diffView = new DiffView(this);
    // Connected after the viewer exists: a connect to a null pointer only warns at
    // run time, and the ✕ that silently stops working is the whole result.
    connect(m_diffView, &DiffView::closeRequested, this, &MainWindow::showGraphPage);
    connect(m_diffView, &DiffView::codecChosen, this, [this](const QString& codec) {
        m_diffCodec = codec;
    });
    connect(m_diffView, &DiffView::changeStagedRequested, this,
            [this](const QString& path, int hunkIndex, const QVector<int>& lines, bool toIndex) {
                applyStagingRequest(path, hunkIndex, lines, toIndex);
            });
    connect(m_diffView, &DiffView::discardRequested, this,
            [this](const QString& path, int hunkIndex, const QVector<int>& lines) {
                applyDiscardRequest(path, hunkIndex, lines);
            });
    connect(m_diffView, &DiffView::blameRequested, this,
            [this](const QString& path) { showBlameFor(path); });
    connect(m_diffView, &DiffView::commitActivated, this,
            [this](const QString& commitId) { selectCommit(commitId); });
    connect(m_diffView, &DiffView::externalDiffRequested, this,
            [this](const QString& path) { openExternalDiff(path); });
}

void MainWindow::onOpenRepositoryDialog() {
    const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("Open Repository"));
    if (!path.isEmpty()) {
        openRepository(path);
    }
}

bool MainWindow::openRepository(const QString& path) {
    // A repository already open comes to the front rather than being opened twice:
    // two sessions on one checkout would each hold a libgit2 handle and race over
    // the index.
    const QString canonical = QFileInfo(path).canonicalFilePath();
    const QString key = canonical.isEmpty() ? QDir::cleanPath(path) : canonical;
    const int existing = contextForPath(key);
    if (existing >= 0) {
        activateContext(existing);
        setStatus(QStringLiteral("Already open: %1").arg(key));
        return true;
    }

    RepoContext* context = active();
    bool created = false;
    int index = m_active;
    if (!context || context->opened) {
        m_contexts.push_back(makeContext());
        context = m_contexts.back().get();
        index = static_cast<int>(m_contexts.size()) - 1;
        created = true;
    }
    if (!openIntoContext(context, path)) {
        if (created) {
            m_contexts.pop_back();
        }
        return false;
    }
    activateContext(index);
    return true;
}

void MainWindow::refreshPanels() {
    // Something the session did not do may have moved a ref (another client, the
    // watcher), so the cached history walk must not be trusted across a refresh.
    m_session->invalidateCommitWalk();
    if (!m_asyncReads || !m_session->isOpen()) {
        m_session->primeSnapshot();
        applyPanels();
        return;
    }
    // Taken now, not when the read lands: a window activation in between must not
    // take the state this refresh is already reading for a change.
    m_lastFingerprint = repositoryFingerprint();
    if (RepoContext* context = active(); context && !context->panelsShown) {
        setPanelsLoading(true);
    }
    m_session->requestSnapshot();
}

void MainWindow::onSnapshotReady(RepoSession* session) {
    RepoContext* context = contextOf(session);
    // A background tab's snapshot waits in its session for the switch to it. One
    // retired by a write since is not applied: that write asked for its own.
    if (!context || context != active() || !m_session->snapshot()) {
        return;
    }
    applyPanels();
}

void MainWindow::setPanelsLoading(bool loading) {
    if (loading) {
        if (m_stack && m_stack->currentIndex() == kStartPage) {
            m_stack->setCurrentIndex(kHistoryPage);
        }
        if (m_branchLabel) {
            m_branchLabel->clear();
        }
        setStatus(QString());
    }
    if (m_refSkeleton) {
        m_refSkeleton->setActive(loading);
    }
    if (m_branchSkeleton) {
        m_branchSkeleton->setActive(loading);
    }
    if (m_details) {
        m_details->setWorkingTreeLoading(loading);
    }
}

void MainWindow::updateGraphSkeleton() {
    if (!m_graphSkeleton) {
        return;
    }
    const bool waiting = m_asyncReads && m_session && m_session->isOpen() && m_commitModel &&
                         m_commitModel->isLoading() && m_commitModel->realRowCount() == 0;
    m_graphSkeleton->setActive(waiting);
}

void MainWindow::setAsyncReadsEnabled(bool on) {
    m_asyncReads = on;
    updateGraphSkeleton();
}

QStringList MainWindow::loadingStateForDiagnostics() const {
    auto state = [](const Skeleton* skeleton) {
        return skeleton && skeleton->isActive() ? QStringLiteral("loading") : QStringLiteral("ready");
    };
    return {QStringLiteral("refs: %1").arg(state(m_refSkeleton)),
            QStringLiteral("graph: %1").arg(state(m_graphSkeleton)),
            QStringLiteral("branch: %1").arg(state(m_branchSkeleton)),
            QStringLiteral("changes: %1")
                .arg(m_details && m_details->workingTreeLoading() ? QStringLiteral("loading")
                                                                  : QStringLiteral("ready")),
            QStringLiteral("files: %1")
                .arg(m_details && m_details->filesLoading() ? QStringLiteral("loading")
                                                            : QStringLiteral("ready")),
            QStringLiteral("diff: %1")
                .arg(m_diffView && m_diffView->loading() ? QStringLiteral("loading")
                                                         : QStringLiteral("ready"))};
}

void MainWindow::applyPanels() {
    if (RepoContext* context = active()) {
        context->panelsShown = m_session->isOpen();
    }
    setPanelsLoading(false);
    refreshRerereActions();
    // Everything the diff pane was showing may have moved: a fetch, a merge or an
    // amend all change what a file at a commit looks like.
    m_renderedPath.clear();
    m_renderedContext.clear();
    // Recording the state drawn here keeps the focus check from refreshing twice for
    // the same change.
    m_lastFingerprint = repositoryFingerprint();

    const RepoInfo info = m_session->info();
    const std::vector<RefInfo> refs = m_session->refs();
    m_refModel->setHeadBranch(QString::fromStdString(info.headBranch));
    refreshStashes();
    refreshWorktrees();
    refreshSubmodules();
    m_refModel->rebuild(refs);

    // Visibility decides which commits the walk reaches, so it has to be handed
    // to the model before the labels are read back off the refs.
    applyVisibility();

    m_remotes = m_session->isOpen() ? m_session->remotes() : std::vector<RemoteInfo>{};
    updateRemotePicker();
    updateSyncActions();
    // Labels and their colours follow the refs, and the refs change on every
    // fetch, merge and branch operation. A move nobody reloaded the history for —
    // a stash, a commit or checkout made in a terminal — reloads it here.
    const quint64 generation = m_commitModel->generation();
    const bool refsMoved = syncHistoryWithRefs(info, refs);
    if (refsMoved && m_commitModel->generation() == generation) {
        m_commitModel->refreshRefs();
    }
    // The working tree rides at the top of the graph as a "// WIP" row: a
    // synthetic row, rebuilt from status every time the refs are. Every dirty
    // worktree gets one, so a change waiting in another checkout is visible here.
    // One status for the whole refresh: the working-tree row, the Commit Panel and
    // the status bar all describe the same working tree, and on a large one each
    // read is the most expensive thing a refresh does.
    const std::vector<FileChange> changes = m_session->status();
    m_commitModel->setWorktreeWip(m_session->worktreeWip(changes));
    m_refModel->setPinned(m_pins.names());
    // A filter mode owns the list, so hide/solo do not also apply while one is on.
    const bool filterMode =
        m_visibility.smart() || m_visibility.preset() != VisibilityPreset::None;
    m_refModel->setVisibility(graphHiddenBranchNames(),
                              filterMode ? QStringList{} : m_visibility.soloNames());
    m_commitModel->setPinnedBranches(m_pins.names());
    m_refModel->setFilter(m_refFilter ? m_refFilter->text() : QString(),
                          m_refFilterRegex && m_refFilterRegex->isChecked());
    syncRefTreeExpansion();

    // The merge editor reads the conflicts from the index, which on a large tree is
    // a re-read of every entry. The status above already says whether anything is
    // conflicted, so with nothing conflicted, no operation open and the editor
    // neither on screen nor holding anything, there is nothing for it to learn;
    // `showConflictsPage` reloads it whenever it is brought up.
    const bool anyConflicted =
        std::any_of(changes.begin(), changes.end(),
                    [](const FileChange& change) { return change.kind == ChangeKind::Conflicted; });
    if (m_conflictView &&
        (anyConflicted || info.operation != RepoOperation::None ||
         m_conflictView->hasConflicts() || m_stack->currentIndex() == kConflictsPage)) {
        m_conflictView->reload();
    }
    // The branch label must stay live on both pages.
    // Ahead/behind live in the permanent label, not the transient status
    // message: renderDiff() overwrites that message as soon as a file is
    // selected, and the counters would vanish.
    const UpstreamInfo tracking = m_session->upstreamInfo();
    QString counters;
    if (tracking.configured) {
        if (tracking.ahead > 0) {
            counters += QStringLiteral(" ↑%1").arg(tracking.ahead);
        }
        if (tracking.behind > 0) {
            counters += QStringLiteral(" ↓%1").arg(tracking.behind);
        }
    }
    m_branchLabel->setText(QStringLiteral("%1%2 @ %3")
                               .arg(describe(info), counters,
                                    QString::fromStdString(info.headId).left(10)));
    // Stay on the merge page while a merge or rebase is in progress, even once
    // every file is resolved — otherwise the "Complete" button would vanish
    // exactly when it becomes usable. A squash leaves no MERGE_HEAD, so it is not
    // "merging" by that test: it keeps the page only once the page is up for it, or
    // while it still has conflicts. A clean squash belongs in the commit panel, not
    // on a conflict page with nothing on it.
    const bool rebasing = m_session->rebaseInProgress();
    const bool squashing = m_session->squashInProgress();
    const bool squashOnPage = squashing &&
                              (m_stack->currentIndex() == kConflictsPage ||
                               (m_conflictView && m_conflictView->hasConflicts()));
    const bool merging = m_session->mergeInProgress() || rebasing || squashOnPage;
    const QString verb = rebasing ? QStringLiteral("rebase")
                                  : (squashOnPage ? QStringLiteral("squash")
                                                  : QStringLiteral("merge"));

    if (merging && !m_conflictPageDismissed) {
        m_stack->setCurrentIndex(kConflictsPage);
        if (m_conflictView && m_conflictView->hasConflicts()) {
            setStatus(QStringLiteral("%1 conflicted file(s) — resolve them, then continue the %2")
                          .arg(m_conflictView->conflictCount())
                          .arg(verb));
        } else {
            setStatus(QStringLiteral("All conflicts resolved — continue the %1.").arg(verb));
        }
    } else {
        m_stack->setCurrentIndex(kHistoryPage);
        // A refresh brings the working tree up to date, but does not take the reader
        // away from the commit or comparison they are reading.
        if (m_selectedCommitId.isEmpty() && m_compareBase.isEmpty()) {
            showWorkingTreeChanges(changes);
        }
        refreshStatusBar(changes);
    }
    refreshCommands();
    updateUndoRedoActions();
}

void MainWindow::refreshStatusBar() {
    refreshStatusBar(m_session->status());
}

void MainWindow::refreshStatusBar(const std::vector<FileChange>& changes) {
    const RepoInfo info = m_session->info();
    QString status = describe(info);
    if (info.operation != RepoOperation::None) {
        status += QStringLiteral(" · %1 in progress")
                      .arg(QString::fromUtf8(toString(info.operation)));
    }
    const UpstreamInfo upstream = m_session->upstreamInfo();
    if (upstream.configured) {
        if (upstream.ahead > 0) {
            status += QStringLiteral(" · ↑%1").arg(upstream.ahead);
        }
        if (upstream.behind > 0) {
            status += QStringLiteral(" · ↓%1").arg(upstream.behind);
        }
    }
    if (info.partialClone) {
        status += QStringLiteral(" · partial clone");
    }
    status += QStringLiteral(" · %1").arg(changesSummary(changes));
    setStatus(status);
}

void MainWindow::showHistoryPage() {
    m_stack->setCurrentIndex(kHistoryPage);
}

void MainWindow::showConflictsPage() {
    // Opening the page is what dismisses a dismissal: every path that starts an
    // operation with conflicts lands here, so a new operation brings its page up
    // even when the reader closed the previous one.
    m_conflictPageDismissed = false;
    m_conflictView->reload();
    m_stack->setCurrentIndex(kConflictsPage);
    if (!m_conflictView->hasConflicts()) {
        setStatus(QStringLiteral("No conflicts in this repository."));
    }
}

void MainWindow::onConflictPageClosed() {
    m_conflictPageDismissed = true;
    showHistoryPage();
    const bool rebasing = m_session->rebaseInProgress();
    const bool squashing = m_session->squashInProgress();
    const QString verb = rebasing ? QStringLiteral("rebase")
                                  : (squashing ? QStringLiteral("squash")
                                               : QStringLiteral("merge"));
    setStatus(QStringLiteral("%1 still open — reopen the merge editor from the Branch menu "
                             "to continue it.")
                  .arg(verb));
}

void MainWindow::showWorkingTreeChanges() {
    if (!m_details) {
        return;
    }
    showWorkingTreeChanges(m_session->status());
}

void MainWindow::showWorkingTreeChanges(const std::vector<FileChange>& changes) {
    if (!m_details) {
        return;
    }
    m_compareBase.clear();
    m_compareTarget.clear();
    m_compareMode = CompareMode::TwoDot;
    m_compareLabel.clear();
    m_details->showWorkingTree(changes);
    m_details->setWorkingTreeBranch(QString::fromStdString(m_session->info().headBranch),
                                    headBranchColor());
    // The Sign switch mirrors `commit.gpgsign`; toggling it writes the config, so
    // re-reading here never fights the reader.
    m_details->setSignCommit(m_session->commitSigningEnabled());
    // `commit.template` is git's own default message. Fill it only while the field
    // is empty, so a refresh after a staging click never overwrites what was typed.
    if (m_details->commitMessage().trimmed().isEmpty()) {
        const QString templateText = m_session->commitTemplate();
        if (!templateText.isEmpty()) {
            m_details->setCommitMessage(templateText);
        }
    }
}

QString MainWindow::repositorySettingsPath() const {
    if (!m_session->isOpen()) {
        return QString();
    }
    // The common git dir, not the checkout path: a linked worktree shares refs with
    // the repository it belongs to, and the view settings should follow the
    // repository, not the directory it happens to be checked out in. For an
    // ordinary repository the common dir is `.git`, which is just as unique.
    const RepoPaths paths = m_session->paths();
    QString key = QString::fromStdString(paths.commonDir);
    if (key.isEmpty()) {
        key = QString::fromStdString(paths.gitDir);
    }
    const QString canonical = QFileInfo(key).canonicalFilePath();
    return canonical.isEmpty() ? key : canonical;
}

QString MainWindow::repositoryLegacySettingsPath() const {
    if (!m_session->isOpen()) {
        return QString();
    }
    const RepoPaths paths = m_session->paths();
    QString path = QString::fromStdString(paths.workDir.empty() ? paths.gitDir : paths.workDir);
    if (path.isEmpty()) {
        path = QString::fromStdString(paths.gitDir);
    }
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    return path;
}

QString MainWindow::repositoryLegacyStoreKey() const {
    const QString path = repositoryLegacySettingsPath();
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? path : canonical;
}

void MainWindow::migrateRepositorySettings(const QString& fromPath, const QString& toPath) {
    if (fromPath.isEmpty() || toPath.isEmpty() || fromPath == toPath) {
        return;
    }
    QSettings settings;
    const QString oldColumns =
        QStringLiteral("ui/columns/") + fromPath + QStringLiteral("/hidden");
    const QString newColumns =
        QStringLiteral("ui/columns/") + toPath + QStringLiteral("/hidden");
    if (!settings.contains(newColumns) && settings.contains(oldColumns)) {
        settings.setValue(newColumns, settings.value(oldColumns));
    }
    for (const char* suffix : {"/enabled", "/remotes"}) {
        const QString oldKey = QStringLiteral("ui/autofetch/") + fromPath + QLatin1String(suffix);
        const QString newKey = QStringLiteral("ui/autofetch/") + toPath + QLatin1String(suffix);
        if (!settings.contains(newKey) && settings.contains(oldKey)) {
            settings.setValue(newKey, settings.value(oldKey));
        }
    }
    const QString oldPre = QStringLiteral("ui/preflight/") + fromPath;
    const QString newPre = QStringLiteral("ui/preflight/") + toPath;
    if (!settings.contains(newPre) && settings.contains(oldPre)) {
        settings.setValue(newPre, settings.value(oldPre));
    }
    const QString oldPrev = QStringLiteral("ui/prevention/") + fromPath;
    const QString newPrev = QStringLiteral("ui/prevention/") + toPath;
    if (!settings.contains(newPrev) && settings.contains(oldPrev)) {
        settings.setValue(newPrev, settings.value(oldPrev));
    }
}

QString MainWindow::columnSettingsKey() const {
    return SettingsStore(repositorySettingsPath()).repositoryKey(QStringLiteral("columns"));
}

QString MainWindow::autoFetchSettingsKey() const {
    return SettingsStore(repositorySettingsPath()).repositoryKey(QStringLiteral("autofetch"));
}

QString MainWindow::preflightSettingsKey() const {
    return SettingsStore(repositorySettingsPath()).repositoryKey(QStringLiteral("preflight"));
}

QString MainWindow::preventionSettingsKey() const {
    return SettingsStore(repositorySettingsPath()).repositoryKey(QStringLiteral("prevention"));
}

void MainWindow::applyCommitColumnModes() {
    QHeaderView* header = m_commitView->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(CommitModel::Refs, QHeaderView::Interactive);
    header->setSectionResizeMode(CommitModel::Graph, QHeaderView::Interactive);
    header->setSectionResizeMode(CommitModel::Message, QHeaderView::Stretch);
    header->setSectionResizeMode(CommitModel::Author, QHeaderView::Interactive);
    header->setSectionResizeMode(CommitModel::Date, QHeaderView::Fixed);
    header->setSectionResizeMode(CommitModel::Sha, QHeaderView::Fixed);
}

static int refsWidthForMode(GraphColumnMode mode) {
    return mode == GraphColumnMode::Compact ? 170 : 260;
}

void MainWindow::applyColumnMode(bool setWidths) {
    if (!m_commitView) {
        return;
    }
    // The glyph is the one thing every mode change alters; the widths are only
    // touched when the reader picked the mode on purpose, so reopening a
    // repository keeps the column widths they dragged afterwards.
    if (m_commitModel) {
        m_commitModel->setRefIcons(m_columnMode == GraphColumnMode::Rich);
    }
    if (setWidths) {
        m_commitView->setColumnWidth(CommitModel::Refs, refsWidthForMode(m_columnMode));
    }
    m_commitView->viewport()->update();
}

void MainWindow::setColumnMode(GraphColumnMode mode, bool setWidths) {
    m_columnMode = mode;
    applyColumnMode(setWidths);
    QSettings settings;
    settings.setValue(columnSettingsKey() + QStringLiteral("/mode"),
                      QString::fromLatin1(graphColumnModeKey(mode)));
}

void MainWindow::loadCommitDateFormat() {
    m_commitDateFormat =
        commitDateFormatFromKey(SettingsStore().stringValue(SettingsStore::globalKey(
                                                               QStringLiteral("commit-date-format")),
                                                           QStringLiteral("iso")));
    if (m_commitModel) {
        m_commitModel->setCommitDateFormat(m_commitDateFormat);
    }
}

void MainWindow::setCommitDateFormat(CommitDateFormat format) {
    m_commitDateFormat = format;
    SettingsStore().setString(SettingsStore::globalKey(QStringLiteral("commit-date-format")),
                              commitDateFormatKey(format));
    if (m_commitModel) {
        m_commitModel->setCommitDateFormat(format);
    }
}

void MainWindow::setTimelineGrouping(TimelinePeriod period) {
    m_timelinePeriod = period;
    if (m_commitModel) {
        m_commitModel->setTimelinePeriod(period);
    }
    QSettings settings;
    settings.setValue(columnSettingsKey() + QStringLiteral("/timeline"),
                      QString::fromLatin1(timelinePeriodKey(period)));
}

void MainWindow::loadTimeline() {
    QSettings settings;
    const QString value =
        settings.value(columnSettingsKey() + QStringLiteral("/timeline"), QStringLiteral("none"))
            .toString();
    m_timelinePeriod = timelinePeriodFromKey(value.toStdString());
    if (m_commitModel) {
        m_commitModel->setTimelinePeriod(m_timelinePeriod);
    }
}

void MainWindow::setCommitColumnVisible(CommitModel::Column column, bool visible) {
    m_commitView->setColumnHidden(column, !visible);
    saveCommitColumns();
}

void MainWindow::saveCommitColumns() {
    if (!m_commitView) {
        return;
    }
    QSettings settings;
    const QString key = columnSettingsKey();
    // The header state carries order, widths and visibility in one value. The hidden
    // list is kept beside it so a profile written by an older build still reads.
    settings.setValue(key + QStringLiteral("/header"), m_commitView->header()->saveState());
    QStringList hidden;
    for (int c = 0; c < CommitModel::ColumnCount; ++c) {
        if (m_commitView->isColumnHidden(c)) {
            hidden.append(QString::number(c));
        }
    }
    settings.setValue(key + QStringLiteral("/hidden"), hidden);
}

void MainWindow::loadCommitColumns() {
    QSettings settings;
    const QString key = columnSettingsKey();
    const QString headerKey = key + QStringLiteral("/header");
    const QByteArray savedMode =
        settings.value(key + QStringLiteral("/mode"), QStringLiteral("rich")).toByteArray();
    m_columnMode = graphColumnModeFromKey(savedMode.constData());

    if (settings.contains(headerKey)) {
        m_commitView->header()->restoreState(settings.value(headerKey).toByteArray());
        // A saved layout does not carry the resize modes, so put them back after it.
        applyCommitColumnModes();
    } else {
        const QString hiddenKey = key + QStringLiteral("/hidden");
        // No saved choice: keep the default the panel was built with. An empty saved
        // list is a real choice — "show everything" — and must not be read as "unset".
        if (settings.contains(hiddenKey)) {
            const QStringList hidden = settings.value(hiddenKey).toStringList();
            for (int c = 0; c < CommitModel::ColumnCount; ++c) {
                m_commitView->setColumnHidden(c, hidden.contains(QString::number(c)));
            }
        }
    }
    // The glyph flag is not part of the header state, so it is always re-applied;
    // the widths already came from the saved layout, so they are not.
    applyColumnMode(false);
}

void MainWindow::showCommitColumnMenu(const QPoint& pos) {
    if (!m_commitView) {
        return;
    }
    QMenu menu(this);
    const auto addColumn = [&](CommitModel::Column column, const QString& label) {
        QAction* action = menu.addAction(label);
        action->setCheckable(true);
        action->setChecked(!m_commitView->isColumnHidden(column));
        connect(action, &QAction::triggered, this,
                [this, column](bool on) { setCommitColumnVisible(column, on); });
    };
    addColumn(CommitModel::Column::Refs, QStringLiteral("Branch / Tag"));
    addColumn(CommitModel::Column::Graph, QStringLiteral("Graph"));
    addColumn(CommitModel::Column::Message, QStringLiteral("Commit Message"));
    menu.addSeparator();
    addColumn(CommitModel::Column::Author, QStringLiteral("Author"));
    addColumn(CommitModel::Column::Date, QStringLiteral("Date / Time"));
    addColumn(CommitModel::Column::Sha, QStringLiteral("SHA"));
    menu.addSeparator();
    QMenu* modeMenu = menu.addMenu(QStringLiteral("Column mode"));
    const auto addMode = [&](GraphColumnMode mode, const QString& label) {
        QAction* action = modeMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(m_columnMode == mode);
        connect(action, &QAction::triggered, this,
                [this, mode] { setColumnMode(mode, /*setWidths=*/true); });
    };
    addMode(GraphColumnMode::Rich, QStringLiteral("Rich (icons and names)"));
    addMode(GraphColumnMode::Text, QStringLiteral("Text (names only)"));
    addMode(GraphColumnMode::Compact, QStringLiteral("Compact (narrow)"));
    menu.exec(m_commitView->header()->mapToGlobal(pos));
}

void MainWindow::showCommitFiles(const QString& commitId) {
    if (!m_details) {
        return;
    }
    const int row = m_commitModel->rowForCommit(commitId);
    const CommitInfo* commit = row >= 0 ? m_commitModel->commitAt(row) : nullptr;
    if (!commit) {
        return;
    }
    const quint64 token = ++m_commitFilesToken;
    if (!m_asyncReads) {
        showCommitDetails(commitId, readCommitDetails(*m_session, commitId));
        return;
    }
    m_details->showCommit(*commit, {}, false);
    m_details->setFilesLoading(true);
    m_diffView->setLoading(true);
    m_session->readAsync<CommitDetails>(
        this, [commitId](RepoSession& session) { return readCommitDetails(session, commitId); },
        [this, token, commitId](CommitDetails details) {
            if (token == m_commitFilesToken) {
                showCommitDetails(commitId, details);
            }
        });
}

void MainWindow::showCommitDetails(const QString& commitId, const CommitDetails& details) {
    const int row = m_commitModel->rowForCommit(commitId);
    const CommitInfo* commit = row >= 0 ? m_commitModel->commitAt(row) : nullptr;
    if (!commit) {
        m_details->setFilesLoading(false);
        m_diffView->setLoading(false);
        return;
    }
    m_details->showCommit(*commit, details.files, details.amendable);
    m_details->setSignatureInfo(details.signature);
    if (details.files.empty()) {
        m_diffView->setLoading(false);
        m_diffView->clear();
    }
}

void MainWindow::showComparison(const QString& base, const QString& target) {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    const QString baseId = m_session->resolveRevision(base);
    const QString targetId = m_session->resolveRevision(target);
    if (baseId.isEmpty() || targetId.isEmpty()) {
        setStatus(QStringLiteral("Cannot compare “%1” with “%2”: one of them does not resolve.")
                      .arg(base, target));
        return;
    }
    applyComparison(baseId, targetId, CompareMode::TwoDot,
                    QStringLiteral("%1..%2").arg(baseId.left(7), targetId.left(7)));
}

void MainWindow::showComparisonMergeBase(const QString& base, const QString& target) {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    const QString baseId = m_session->resolveRevision(base);
    const QString targetId = m_session->resolveRevision(target);
    if (baseId.isEmpty() || targetId.isEmpty()) {
        setStatus(QStringLiteral("Cannot compare “%1” with “%2”: one of them does not resolve.")
                      .arg(base, target));
        return;
    }
    // `A...B` is the merge base of the two against B: what A has done since they
    // parted, not what both have done.
    const QString mergeBaseId = m_session->mergeBase(baseId, targetId);
    if (mergeBaseId.isEmpty()) {
        setStatus(QStringLiteral("“%1” and “%2” share no history to compare from.")
                      .arg(base, target));
        return;
    }
    applyComparison(mergeBaseId, targetId, CompareMode::MergeBase,
                    QStringLiteral("%1...%2").arg(baseId.left(7), targetId.left(7)));
}

void MainWindow::showComparisonWorkingTree(const QString& revision) {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    const QString baseId = m_session->resolveRevision(revision);
    if (baseId.isEmpty()) {
        setStatus(QStringLiteral("Cannot compare with the working tree: “%1” does not resolve.")
                      .arg(revision));
        return;
    }
    applyComparison(baseId, QString(), CompareMode::WorkingTree,
                    QStringLiteral("%1..working tree").arg(baseId.left(7)));
}

void MainWindow::applyComparison(const QString& baseId, const QString& targetId, CompareMode mode,
                                 const QString& label) {
    m_compareBase = baseId;
    m_compareTarget = targetId;
    m_compareMode = mode;
    m_compareLabel = label;
    m_selectedCommitId.clear();
    // Force a re-render: the same path under the same commit id must re-read the
    // comparison diff, and the cache key is what tells the viewer it changed.
    m_renderedPath.clear();

    const std::vector<ChangedFile> files =
        mode == CompareMode::WorkingTree ? m_session->workdirChangedFilesAgainst(baseId)
                                         : m_session->changedFilesBetween(baseId, targetId);
    m_details->showComparison(baseId, targetId, files, label);
    if (files.empty()) {
        m_diffView->clear();
    }
    showDiffPage();
    setStatus(QStringLiteral("Comparing %1 — %2 file(s)").arg(label).arg(files.size()));
}

void MainWindow::restoreRenderedFileFromView() {
    if (m_renderedPath.isEmpty()) {
        setStatus(QStringLiteral("Open a file's diff first."));
        return;
    }
    const QString revision = !m_compareBase.isEmpty() ? m_compareBase : m_selectedCommitId;
    if (revision.isEmpty()) {
        setStatus(QStringLiteral("Select a commit or a comparison first."));
        return;
    }
    if (!askYes(QStringLiteral("Restore file"),
                QStringLiteral("Replace the working-tree copy of %1 with its content at %2?\n\n"
                               "Anything uncommitted in that file is overwritten.")
                    .arg(m_renderedPath, revision.left(10)))) {
        return;
    }
    QString error;
    if (!m_session->restoreFile(revision, m_renderedPath, /*staged=*/false, &error)) {
        reportFailure(QStringLiteral("Cannot restore the file"), error);
        return;
    }
    refreshPanels();
    m_compareBase.clear();
    m_compareTarget.clear();
    m_compareMode = CompareMode::TwoDot;
    m_compareLabel.clear();
    renderFile(m_renderedPath, /*workingTree=*/true, /*staged=*/false);
    setStatus(QStringLiteral("Restored %1 from %2.").arg(m_renderedPath, revision.left(10)));
}

void MainWindow::showBlameFor(const QString& path) {
    if (!m_session || !m_session->isOpen() || path.isEmpty()) {
        return;
    }
    const QString revision = !m_compareTarget.isEmpty() ? m_compareTarget : m_selectedCommitId;
    const std::vector<BlameLine> lines = m_session->blame(path, revision);
    m_diffView->setBlame(lines);
    setStatus(QStringLiteral("Blame for %1 — %2 line(s)").arg(path).arg(lines.size()));
}

void MainWindow::showFileHistory(const QString& path) {
    if (!m_session || !m_session->isOpen() || path.isEmpty()) {
        setStatus(QStringLiteral("Select a file first."));
        return;
    }
    const std::vector<FileHistoryEntry> entries = m_session->fileHistory(path);
    FileHistoryDialog dialog(path, entries, this);
    if (dialog.exec() == QDialog::Accepted) {
        const QString commit = dialog.selectedCommit();
        if (!commit.isEmpty()) {
            selectCommit(commit);
        }
    }
}

void MainWindow::showReflog() {
    if (!m_session || !m_session->isOpen()) {
        setStatus(QStringLiteral("Open a repository first."));
        return;
    }
    const std::vector<ReflogEntry> entries = m_session->reflog(500);
    if (entries.empty()) {
        setStatus(QStringLiteral("This repository has no reflog entries."));
        return;
    }
    ReflogDialog dialog(entries, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString commit = dialog.chosenCommit();
    if (commit.isEmpty()) {
        return;
    }
    if (dialog.chosenAction() == QStringLiteral("checkout")) {
        m_selectedCommitId = commit;
        runCheckout(QStringLiteral("Cannot check out that commit"), commit.left(10),
                    [commit](RepoSession& s, QString* error) {
                        return s.checkoutCommit(commit, error);
                    },
                    [this, commit] {
                        setStatus(QStringLiteral("Checked out %1 (detached).").arg(commit.left(10)));
                    });
    } else if (dialog.chosenAction() == QStringLiteral("branch")) {
        createBranchAtCommit(commit);
    }
}

void MainWindow::editSparseCheckout() {
    if (!m_session || !m_session->isOpen()) {
        setStatus(QStringLiteral("Open a repository first."));
        return;
    }
    const bool enabled = m_session->sparseCheckoutEnabled();
    const QStringList inCone = m_session->sparseCheckoutDirectories();
    const QStringList directories = m_session->sparseCheckoutTopLevelDirectories();
    SparseCheckoutDialog dialog(directories, inCone, enabled, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QStringList chosen = dialog.action() == SparseCheckoutDialog::Action::Disable
                                   ? QStringList{}
                                   : dialog.chosenDirectories();
    QString error;
    if (!m_session->setSparseCheckout(chosen, &error)) {
        QMessageBox::warning(this, QStringLiteral("Sparse checkout failed"), error);
        return;
    }
    refreshPanels();
    notify(NotificationLog::Success,
           chosen.isEmpty()
               ? QStringLiteral("Sparse checkout disabled; the whole tree is checked out.")
               : QStringLiteral("Sparse checkout: keeping %1.").arg(chosen.join(QStringLiteral(", "))));
}

void MainWindow::applyProfile(const GitProfile& profile) {
    if (!m_session || !m_session->isOpen()) {
        setStatus(QStringLiteral("Open a repository first."));
        return;
    }
    if (profile.name.isEmpty() || profile.userName.isEmpty() || profile.email.isEmpty()) {
        setStatus(QStringLiteral("A profile needs a name and an email."));
        return;
    }
    QString error;
    const auto write = [&](const QString& key, const QString& value) {
        QString local;
        if (!m_session->setConfigValue(key, value, &local)) {
            error = local;
        }
    };
    write(QStringLiteral("user.name"), profile.userName);
    write(QStringLiteral("user.email"), profile.email);
    if (!profile.signingKey.isEmpty()) {
        write(QStringLiteral("user.signingkey"), profile.signingKey);
    }
    // A profile that does not sign must say so, or a repository left signing by an
    // earlier profile keeps signing with a key that is no longer the reader's.
    write(QStringLiteral("commit.gpgsign"), profile.sign ? QStringLiteral("true")
                                                         : QStringLiteral("false"));
    if (!error.isEmpty()) {
        notify(NotificationLog::Error, QStringLiteral("Cannot apply the profile"));
        QMessageBox::warning(this, QStringLiteral("Cannot apply the profile"), error);
        return;
    }
    profiles::setActiveFor(repositorySettingsPath(), profile.name);
    refreshPanels();
    notify(NotificationLog::Success,
           QStringLiteral("Committing as %1 <%2>.").arg(profile.userName, profile.email));
}

void MainWindow::editProfiles() {
    if (!m_session || !m_session->isOpen()) {
        setStatus(QStringLiteral("Open a repository first."));
        return;
    }
    ProfileDialog dialog(profiles::activeFor(repositorySettingsPath()), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString name = dialog.chosenProfile();
    if (!name.isEmpty()) {
        applyProfile(profiles::load(name));
    }
}

void MainWindow::openGitFlow() {
    if (!m_session || !m_session->isOpen()) {
        setStatus(QStringLiteral("Open a repository first."));
        return;
    }
    const bool initialized = m_session->gitflowInitialized();
    QString master = QStringLiteral("master");
    QString develop = QStringLiteral("develop");
    for (const QString& line : m_session->gitflowSummary()) {
        if (line.startsWith(QStringLiteral("master: "))) {
            master = line.mid(QStringLiteral("master: ").size());
        } else if (line.startsWith(QStringLiteral("develop: "))) {
            develop = line.mid(QStringLiteral("develop: ").size());
        }
    }

    GitFlowDialog dialog(initialized, master, develop, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    QString error;
    bool ok = false;
    QString what;
    switch (dialog.action()) {
    case GitFlowDialog::Action::Initialize:
        ok = m_session->initGitflow(dialog.masterBranch(), dialog.developBranch(), &error);
        what = QStringLiteral("Gitflow initialised.");
        break;
    case GitFlowDialog::Action::Start:
        ok = m_session->gitflowStart(dialog.kind(), dialog.name(), &error);
        what = QStringLiteral("Started %1 %2.").arg(dialog.kind(), dialog.name());
        break;
    case GitFlowDialog::Action::Finish:
        ok = m_session->gitflowFinish(dialog.kind(), dialog.name(), dialog.version(), &error);
        what = QStringLiteral("Finished %1 %2.").arg(dialog.kind(), dialog.name());
        break;
    case GitFlowDialog::Action::None:
        return;
    }
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Gitflow"), error);
        return;
    }
    m_commitModel->reset();
    refreshPanels();
    notify(NotificationLog::Success, what);
}

void MainWindow::openLfs() {
    if (!m_session || !m_session->isOpen()) {
        setStatus(QStringLiteral("Open a repository first."));
        return;
    }
    for (;;) {
        const bool available = m_session->lfsAvailable();
        const QString version = available ? m_session->lfsVersion() : QString();
        const bool configured = available && m_session->lfsConfigured();
        const QStringList patterns = available ? m_session->lfsTrackedPatterns() : QStringList();
        const QStringList files = available ? m_session->lfsFiles() : QStringList();

        LfsDialog dialog(available, version, configured, patterns, files, this);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const LfsDialog::Action action = dialog.action();
        if (action == LfsDialog::Action::None) {
            return;
        }

        // The network operations are the only ones that must not block the GUI; they
        // run on a worker and the dialog closes while they do.
        if (action == LfsDialog::Action::Fetch || action == LfsDialog::Action::Pull ||
            action == LfsDialog::Action::Push) {
            if (m_lfsRunning) {
                setStatus(QStringLiteral("An LFS operation is already running."));
                return;
            }
            LfsTask task = LfsTask::Fetch;
            if (action == LfsDialog::Action::Pull) {
                task = LfsTask::Pull;
            } else if (action == LfsDialog::Action::Push) {
                task = LfsTask::Push;
            }
            m_lfsRunning = true;
            if (m_commitView) {
                m_commitView->setBusy(true);
            }
            m_session->requestLfs(task, currentSyncRemote(),
                                  QString::fromStdString(m_session->info().headBranch));
            return;
        }

        QString error;
        bool ok = true;
        switch (action) {
        case LfsDialog::Action::Install:
            ok = m_session->lfsInstall(&error);
            break;
        case LfsDialog::Action::Track:
            ok = m_session->lfsTrack(dialog.pattern(), &error);
            break;
        case LfsDialog::Action::Untrack:
            ok = m_session->lfsUntrack(dialog.pattern(), &error);
            break;
        case LfsDialog::Action::Checkout:
            ok = m_session->lfsCheckout(&error);
            break;
        case LfsDialog::Action::Prune: {
            const SyncResult result = m_session->lfsBlocking(LfsTask::Prune, {}, {});
            ok = result.ok;
            error = QString::fromStdString(result.output);
            break;
        }
        case LfsDialog::Action::Fetch:
        case LfsDialog::Action::Pull:
        case LfsDialog::Action::Push:
        case LfsDialog::Action::None:
            return;
        }

        if (!ok) {
            notify(NotificationLog::Error,
                   QStringLiteral("git lfs failed: %1").arg(error.trimmed()));
        } else {
            notify(NotificationLog::Success, QStringLiteral("git lfs done."));
        }
        refreshPanels();
    }
}

QStringList MainWindow::lfsForDiagnostics(const QString& op, const QString& argument) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        return {QStringLiteral("no repository is open")};
    }
    const bool available = m_session->lfsAvailable();
    report << QStringLiteral("available: %1")
                  .arg(available ? QStringLiteral("yes") : QStringLiteral("no"));
    if (!available) {
        report << QStringLiteral("note: git-lfs is not installed");
        return report;
    }
    report << QStringLiteral("version: %1").arg(m_session->lfsVersion());

    QString error;
    bool ok = true;
    const QString remote = currentSyncRemote();
    const QString branch = QString::fromStdString(m_session->info().headBranch);
    if (op == QStringLiteral("init")) {
        ok = m_session->lfsInstall(&error);
    } else if (op == QStringLiteral("track")) {
        ok = m_session->lfsTrack(argument, &error);
    } else if (op == QStringLiteral("untrack")) {
        ok = m_session->lfsUntrack(argument, &error);
    } else if (op == QStringLiteral("checkout")) {
        ok = m_session->lfsCheckout(&error);
    } else if (op == QStringLiteral("prune") || op == QStringLiteral("prune-dry-run")) {
        const SyncResult result = m_session->lfsBlocking(
            op == QStringLiteral("prune") ? LfsTask::Prune : LfsTask::PruneDryRun, remote, {});
        ok = result.ok;
        error = QString::fromStdString(result.output);
    } else if (op == QStringLiteral("fetch") || op == QStringLiteral("pull") ||
               op == QStringLiteral("push") || op == QStringLiteral("push-all")) {
        LfsTask task = LfsTask::Fetch;
        lfsTaskFromName(op.toStdString(), &task);
        const SyncResult result = m_session->lfsBlocking(
            task, remote, op == QStringLiteral("push") ? branch : QString());
        ok = result.ok;
        error = QString::fromStdString(result.output);
        report << QStringLiteral("%1 output: %2")
                      .arg(op, QString::fromStdString(result.output).trimmed().left(400));
    } else if (op == QStringLiteral("status")) {
        report << m_session->lfsStatusReport();
    }
    if (!ok) {
        report << QStringLiteral("error: %1").arg(error.trimmed());
    }
    report << QStringLiteral("configured: %1")
                  .arg(m_session->lfsConfigured() ? QStringLiteral("yes") : QStringLiteral("no"));
    report << QStringLiteral("patterns: %1")
                  .arg(m_session->lfsTrackedPatterns().join(QLatin1Char(';')));
    report << QStringLiteral("files: %1").arg(m_session->lfsFiles().join(QLatin1Char(';')));
    return report;
}

void MainWindow::notify(int level, const QString& text) {
    m_notifications.add(static_cast<NotificationLog::Level>(level), text);
    setStatus(text);
}

void MainWindow::showNotifications() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Notifications"));
    dialog.resize(theme::scaledPixels(640), theme::scaledPixels(420));
    dialog.setStyleSheet(theme::styleSheet());

    auto* root = new QVBoxLayout(&dialog);
    auto* list = new QListWidget(&dialog);
    const auto populate = [this, list] {
        list->clear();
        for (const Notification& entry : m_notifications.entries()) {
            QColor color = theme::textDim();
            switch (entry.level) {
            case NotificationLog::Success: color = theme::success(); break;
            case NotificationLog::Warning: color = theme::warning(); break;
            case NotificationLog::Error: color = theme::danger(); break;
            default: color = theme::text(); break;
            }
            auto* item = new QListWidgetItem(
                QStringLiteral("%1  %2")
                    .arg(entry.when.toString(QStringLiteral("HH:mm:ss")), entry.text));
            item->setForeground(color);
            list->addItem(item);
        }
        if (list->count() == 0) {
            list->addItem(QStringLiteral("Nothing yet."));
        }
    };
    populate();
    root->addWidget(list, 1);

    auto* buttons = new QHBoxLayout;
    auto* clear = new QPushButton(QStringLiteral("Clear"), &dialog);
    connect(clear, &QPushButton::clicked, this, [this, populate] {
        m_notifications.clear();
        populate();
    });
    buttons->addWidget(clear);
    buttons->addStretch(1);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    buttons->addWidget(close);
    root->addLayout(buttons);
    dialog.exec();
}

QStringList MainWindow::notificationsForDiagnostics(const QString& op) {
    if (op == QStringLiteral("clear")) {
        m_notifications.clear();
    }
    QStringList report;
    report << QStringLiteral("count: %1").arg(m_notifications.entries().size());
    report << QStringLiteral("problems: %1").arg(m_notifications.problemCount());
    for (const Notification& entry : m_notifications.entries()) {
        report << QStringLiteral("%1 level=%2 %3")
                      .arg(entry.when.toString(Qt::ISODate))
                      .arg(entry.level)
                      .arg(entry.text);
    }
    return report;
}

QStringList MainWindow::dateFormatForDiagnostics(const QString& key) {
    // Applies to the model without storing: a headless check must not rewrite the
    // reader's saved preference.
    if (!key.isEmpty()) {
        const CommitDateFormat format = commitDateFormatFromKey(key);
        m_commitDateFormat = format;
        if (m_commitModel) {
            m_commitModel->setCommitDateFormat(format);
        }
    }
    QStringList report;
    report << QStringLiteral("format: %1").arg(commitDateFormatKey(m_commitDateFormat));
    if (m_commitModel && m_commitModel->rowCount() > 0) {
        report << QStringLiteral("first: %1")
                      .arg(m_commitModel
                               ->data(m_commitModel->index(0, CommitModel::Date), Qt::DisplayRole)
                               .toString());
    }
    return report;
}

void MainWindow::amendSelectedCommitMessage(const QString& message) {
    QString error;
    if (!m_session->amendHeadMessage(message, &error)) {
        QMessageBox::warning(this, QStringLiteral("Cannot amend the commit"), error);
        return;
    }
    // The amended commit is a new object with a new hash, so the old one is gone
    // from the graph: follow HEAD rather than the id we came in with.
    m_selectedCommitId = QString::fromStdString(m_session->info().headId);
    m_commitModel->reset();
    refreshPanels();
    showCommitFiles(m_selectedCommitId);
    setStatus(QStringLiteral("Commit message amended."));
}

void MainWindow::verifyCommitSignature(const QString& commitId) {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    const CommitSignature signature = m_session->commitSignature(commitId);
    QString verdict;
    const QString status = QString::fromStdString(signature.status);
    if (status.isEmpty() || status.startsWith(QLatin1Char('N'))) {
        verdict = QStringLiteral("This commit is not signed.");
    } else {
        const QChar code = status.at(0);
        if (code == QLatin1Char('G')) {
            verdict = QStringLiteral("Good signature.");
        } else if (code == QLatin1Char('U')) {
            verdict = QStringLiteral("Good signature, but the key is not trusted.");
        } else if (code == QLatin1Char('X')) {
            verdict = QStringLiteral("The signature has expired.");
        } else if (code == QLatin1Char('Y')) {
            verdict = QStringLiteral("The signing key has expired.");
        } else if (code == QLatin1Char('R')) {
            verdict = QStringLiteral("The signing key has been revoked.");
        } else if (code == QLatin1Char('B')) {
            verdict = QStringLiteral("BAD signature — the commit was altered or the key is wrong.");
        } else if (code == QLatin1Char('E')) {
            verdict = QStringLiteral("The signature cannot be checked (the key is missing).");
        } else {
            verdict = QStringLiteral("Signed.");
        }
        const QString signer = QString::fromStdString(signature.signer);
        if (!signer.isEmpty()) {
            verdict += QStringLiteral("\nSigner: %1").arg(signer);
        }
        const QString key = QString::fromStdString(signature.key);
        if (!key.isEmpty()) {
            verdict += QStringLiteral("\nKey: %1").arg(key);
        }
    }
    QMessageBox::information(this, QStringLiteral("Commit signature"),
                             QStringLiteral("%1\n\n%2").arg(verdict, commitId));
}

void MainWindow::selectCommit(const QString& commitId) {
    const int row = m_commitModel->rowForCommit(commitId);
    if (row < 0) {
        setStatus(QStringLiteral("Commit %1 is outside the loaded range")
                      .arg(commitId.left(7)));
        return;
    }
    m_commitView->setCurrentIndex(m_commitModel->index(row, 0));
}

void MainWindow::onCommitSelected() {
    m_compareBase.clear();
    m_compareTarget.clear();
    const QModelIndex current = m_commitView->currentIndex();
    if (m_commitModel->isTimelineRow(current.row())) {
        return;
    }
    m_commitModel->setPathAnchorRow(current.row());
    // A working-tree row is not a commit. Selecting the current checkout's shows
    // the changes waiting to be committed; another worktree's names the checkout
    // they live in, because this window's panel is not that working tree.
    if (m_commitModel->isWipRow(current.row())) {
        m_selectedCommitId.clear();
        if (current.row() != m_commitModel->wipRow()) {
            setStatus(QStringLiteral("Worktree “%1” has uncommitted changes — open it from the "
                                     "Worktrees panel to work on them.")
                          .arg(current.data(CommitModel::WipNameRole).toString()));
        }
        showWorkingTreeChanges();
        return;
    }
    const CommitInfo* commit = m_commitModel->commitAt(current.row());
    if (!commit) {
        m_selectedCommitId.clear();
        showWorkingTreeChanges();
        return;
    }
    m_selectedCommitId = QString::fromStdString(commit->id);
    // showCommitFiles() auto-selects the first changed file, which renders its
    // diff — do not clear the diff view afterwards.
    showCommitFiles(m_selectedCommitId);
}

void MainWindow::renderFile(const QString& path, bool workingTree, bool staged) {
    if (!m_diffView || path.isEmpty()) {
        return;
    }

    // A click both selects and activates, and the selection already rendered this
    // file: re-reading the diff for the same path would be work for nothing. The
    // staged flag is part of the identity: the same path means two different things
    // in the two groups.
    const QString context =
        workingTree ? (staged ? QStringLiteral("staged") : QStringLiteral("unstaged"))
                    : (m_compareBase.isEmpty()
                           ? m_selectedCommitId
                           : (m_compareMode == CompareMode::WorkingTree
                                  ? QStringLiteral("wip:%1").arg(m_compareBase)
                                  : (m_compareMode == CompareMode::MergeBase
                                         ? QStringLiteral("cmp3:%1..%2")
                                               .arg(m_compareBase, m_compareTarget)
                                         : QStringLiteral("cmp:%1..%2")
                                               .arg(m_compareBase, m_compareTarget))));
    if (m_renderedPath == path && m_renderedContext == context) {
        // Already on screen, or on its way: the skeleton a commit selection put up
        // for it is not this call's to keep.
        if (!m_documentPending) {
            m_diffView->setLoading(false);
        }
        return;
    }
    m_renderedPath = path;
    m_renderedContext = context;
    m_renderedStaged = staged;
    if (!workingTree && m_compareBase.isEmpty() && m_selectedCommitId.isEmpty()) {
        return;
    }

    DocumentRequest request;
    request.path = path;
    request.workingTree = workingTree;
    request.staged = staged;
    request.commitId = m_selectedCommitId;
    request.compareBase = m_compareBase;
    request.compareTarget = m_compareTarget;
    request.compareLabel = m_compareLabel;
    request.compareWorkingTree = m_compareMode == CompareMode::WorkingTree;

    const quint64 token = ++m_documentToken;
    if (!m_asyncReads) {
        showDocument(readDocument(*m_session, request));
        return;
    }
    // The text stays readable until the new one is ready, under skeleton rows, so
    // the pane never flashes empty and never shows the old file as if it were new.
    m_diffView->setLoading(true);
    m_documentPending = true;
    m_session->readAsync<DiffView::Document>(
        this, [request](RepoSession& session) { return readDocument(session, request); },
        [this, token](DiffView::Document document) {
            if (token == m_documentToken) {
                m_documentPending = false;
                showDocument(document);
            }
        });
}

void MainWindow::showDocument(const DiffView::Document& document) {
    m_diffView->setCodec(m_diffCodec);
    m_diffView->show(document);
    if (m_diffView->blameView()) {
        showBlameFor(document.path);
    }

    const FileDiff& diff = document.diff;
    if (!diff.binary && !diff.hunks.empty()) {
        setStatus(QStringLiteral("%1 · +%2 −%3")
                      .arg(QString::fromStdString(diff.newPath))
                      .arg(diff.added)
                      .arg(diff.removed));
    }
}

void MainWindow::setGraphColumnWidth(int width) {
    // Pin the width the way dragging the header does: the auto-fit must not fight
    // it, and the delegate must compress into it. Used by the headless snapshot.
    m_graphColumnUserSized = true;
    m_commitView->setColumnWidth(CommitModel::Graph, width);
    m_commitView->viewport()->update();
}

void MainWindow::updateGraphColumnWidth() {
    // Auto-fit the graph column to the lanes, but only until the user drags the
    // header themselves: from then on their width is the right width, and
    // fighting them on every layout change is worse than a clipped lane. Lanes
    // are ordered by importance, so what stays visible when narrow is the
    // branch you are on and the one you clicked. The fit stops at a few lanes: a
    // repository with hundreds of live branches would open with the messages
    // pushed off the screen.
    if (m_graphColumnUserSized) {
        return;
    }
    m_commitView->setColumnWidth(
        CommitModel::Graph,
        GraphDelegate::widthForLanes(std::min(m_commitModel->laneCount(), kDefaultGraphLanes)));
}

void MainWindow::selectFirstCommit() {
    if (m_commitModel->rowCount() == 0) {
        return;
    }
    m_commitView->setCurrentIndex(m_commitModel->index(0, CommitModel::Message));
}

void MainWindow::centerCommitRow(int row) {
    const QPersistentModelIndex index(m_commitModel->index(row, CommitModel::Message));
    if (!index.isValid()) {
        return;
    }
    m_commitView->scrollTo(index, QAbstractItemView::PositionAtCenter);
    // Rows that just landed are laid out on the next turn; centre again once they are.
    QTimer::singleShot(0, this, [this, index] {
        if (index.isValid() && m_commitView) {
            m_commitView->scrollTo(index, QAbstractItemView::PositionAtCenter);
        }
    });
}

void MainWindow::selectCommitRow(int row) {
    if (row < 0) {
        selectFirstCommit();
        return;
    }
    const QModelIndex index = m_commitModel->index(row, CommitModel::Message);
    if (index.isValid()) {
        m_commitView->setCurrentIndex(index);
    }
}

void MainWindow::openFirstFileDiff() {
    const QString path = m_details ? m_details->firstFilePath() : QString();
    if (path.isEmpty()) {
        return;
    }
    renderFile(path, m_details->showingWorkingTree(), m_details->firstFileIsStaged());
    showDiffPage();
}

void MainWindow::setDiffFileView(bool on) {
    if (m_diffView) {
        m_diffView->setFileView(on);
    }
}

void MainWindow::setDiffSplitView(bool on) {
    if (m_diffView) {
        m_diffView->setSplitView(on);
    }
}

void MainWindow::setDiffBlameView(bool on) {
    if (m_diffView) {
        m_diffView->setBlameView(on);
        if (on && !m_renderedPath.isEmpty()) {
            showBlameFor(m_renderedPath);
        }
    }
}

void MainWindow::setDiffWrap(bool on) {
    if (m_diffView) {
        m_diffView->setWrap(on);
    }
}

void MainWindow::showDiffPage() {
    if (!m_centerStack || m_centerStack->currentWidget() == m_diffView) {
        return;
    }
    m_centerStack->setCurrentWidget(m_diffView);
    if (m_refsVisible && m_history && !m_history->isPanelCollapsed(0)) {
        m_foldingPanel = true;
        m_history->setPanelCollapsed(0, true);
        m_foldingPanel = false;
        m_refsHiddenForDiff = true;
    }
}

void MainWindow::showGraphPage() {
    if (!m_centerStack) {
        return;
    }
    m_centerStack->setCurrentWidget(m_commitView);
    if (m_refsHiddenForDiff) {
        // Only undo what the diff did: a rail the reader hid stays hidden.
        m_foldingPanel = true;
        m_history->setPanelCollapsed(0, !m_refsVisible);
        m_foldingPanel = false;
        m_refsHiddenForDiff = false;
    }
}

bool MainWindow::diffPageVisible() const {
    return m_centerStack && m_centerStack->currentWidget() == m_diffView;
}

bool MainWindow::refsPanelVisible() const {
    // isHidden(), not isVisible(): a widget in a window that has not been shown yet
    // reports "not visible" while being perfectly present, and this answer is about
    // whether the rail is up, not about whether the window is on screen.
    return m_refPanel && !m_refPanel->isHidden() && !(m_history && m_history->isPanelCollapsed(0));
}

void MainWindow::setRefsPanelVisible(bool visible) {
    m_refsVisible = visible;
    m_refsHiddenForDiff = false;
    if (m_history) {
        m_foldingPanel = true;
        m_history->setPanelCollapsed(0, !visible);
        m_foldingPanel = false;
    }
    if (m_refsAction && m_refsAction->isChecked() != visible) {
        m_refsAction->setChecked(visible);
    }
}

bool MainWindow::event(QEvent* event) {
    if (event->type() == QEvent::WindowActivate) {
        refreshIfRepositoryChanged();
    }
    return QMainWindow::event(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // The tab set is saved here as well as on every change: a session opened with a
    // repository path starts before the window is visible, and the per-change save
    // skips a hidden window.
    saveTabs();
    QMainWindow::closeEvent(event);
}

QString MainWindow::repositoryFingerprint() const {
    if (!m_session->isOpen()) {
        return QString();
    }
    // Read from the files, not the session: this runs every time the window comes
    // back to the front, and a session read would wait behind a history page. HEAD's
    // text names the branch (or the detached commit); times and sizes say whether
    // anything under it, or an operation's marker, moved.
    const RepoPaths paths = m_session->paths();
    const QString gitDir = QString::fromStdString(paths.gitDir);
    const QString commonDir =
        QString::fromStdString(paths.commonDir.empty() ? paths.gitDir : paths.commonDir);
    QString head;
    QFile headFile(gitDir + QStringLiteral("/HEAD"));
    if (headFile.open(QIODevice::ReadOnly)) {
        head = QString::fromUtf8(headFile.read(512)).trimmed();
    }
    const QString refPrefix = QStringLiteral("ref: refs/heads/");
    const QString branch = head.startsWith(refPrefix) ? head.mid(refPrefix.size()) : QString();

    QString out = head;
    const QStringList watched = {
        gitDir + QStringLiteral("/index"),
        gitDir + QStringLiteral("/FETCH_HEAD"),
        gitDir + QStringLiteral("/MERGE_HEAD"),
        gitDir + QStringLiteral("/CHERRY_PICK_HEAD"),
        gitDir + QStringLiteral("/REVERT_HEAD"),
        gitDir + QStringLiteral("/SQUASH_MSG"),
        gitDir + QStringLiteral("/rebase-merge"),
        gitDir + QStringLiteral("/rebase-apply"),
        commonDir + QStringLiteral("/packed-refs"),
        branch.isEmpty() ? QString() : commonDir + QStringLiteral("/refs/heads/") + branch,
    };
    for (const QString& file : watched) {
        if (file.isEmpty()) {
            continue;
        }
        const QFileInfo stat(file);
        out += QLatin1Char('|');
        out += stat.exists()
                   ? QString::number(stat.lastModified().toMSecsSinceEpoch()) +
                         QLatin1Char(':') + QString::number(stat.size())
                   : QStringLiteral("-");
    }
    return out;
}

void MainWindow::refreshIfRepositoryChanged() {
    const QString fingerprint = repositoryFingerprint();
    if (fingerprint.isEmpty() || fingerprint == m_lastFingerprint) {
        return;
    }
    m_lastFingerprint = fingerprint;
    refreshPanels();
    setStatus(QStringLiteral("Repository changed — refreshed."));
}

bool MainWindow::syncHistoryWithRefs(const RepoInfo& info, const std::vector<RefInfo>& refs) {
    RepoContext* context = active();
    if (!context || !m_session->isOpen()) {
        return true;
    }
    std::uint64_t digest = 1469598103934665603ULL;
    const auto mix = [&digest](const std::string& text) {
        for (const unsigned char c : text) {
            digest = (digest ^ c) * 1099511628211ULL;
        }
        digest = (digest ^ 0xffU) * 1099511628211ULL;
    };
    mix(info.headBranch);
    mix(info.headId);
    for (const RefInfo& ref : refs) {
        mix(ref.fullName);
        mix(ref.target);
    }
    const bool known = context->historyDigestKnown;
    const bool moved = known && context->historyDigest != digest;
    const bool reloadedSince = context->historyGeneration != m_commitModel->generation();
    context->historyDigest = digest;
    context->historyDigestKnown = true;
    if (moved && !reloadedSince) {
        reloadHistory();
    }
    context->historyGeneration = m_commitModel->generation();
    return !known || moved;
}

void MainWindow::reloadHistory() {
    m_restoreCommit = m_selectedCommitId;
    m_restoreWip = m_selectedCommitId.isEmpty() && m_details && m_details->showingWorkingTree();
    m_commitModel->reset();
    restoreSelectionAfterReload();
}

void MainWindow::restoreSelectionAfterReload() {
    if (m_restoreCommit.isEmpty() && !m_restoreWip) {
        return;
    }
    const int row = m_restoreWip ? m_commitModel->wipRow()
                                 : m_commitModel->rowForCommit(m_restoreCommit);
    if (row < 0) {
        // Only the first page is waited for: a commit that is not on it any more
        // (amended away, or far down) is not worth paging the history for.
        if (!m_commitModel->isLoading() && m_commitModel->realRowCount() > 0) {
            m_restoreCommit.clear();
            m_restoreWip = false;
        }
        return;
    }
    m_restoreCommit.clear();
    m_restoreWip = false;
    // Put back quietly: the panels already describe this commit, and selecting it
    // through the signal would reload its files and its diff for nothing.
    const QModelIndex index = m_commitModel->index(row, CommitModel::Message);
    {
        const QSignalBlocker block(m_commitView->selectionModel());
        m_commitView->selectionModel()->setCurrentIndex(
            index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    m_commitModel->setPathAnchorRow(row);
    m_commitView->viewport()->update();
}

void MainWindow::onRepositoryFilesChanged(bool gitState) {
    if (!m_session->isOpen() || m_syncRunning || m_maintenanceRunning || m_checkoutRunning) {
        return;
    }
    // The refresh compares the refs with what the history was read for, and reloads
    // the graph when they moved; a file saved in the work tree changes only the panels.
    refreshPanels();
    setStatus(gitState ? QStringLiteral("Repository changed — refreshed.")
                       : QStringLiteral("Working tree changed — refreshed."));
}

void MainWindow::setStatus(const QString& text) {
    statusBar()->showMessage(text);
}

void MainWindow::runWrite(const QString& what, const std::function<bool(QString*)>& action,
                          bool resetHistory) {
    QString error;
    if (!action(&error)) {
        QMessageBox::warning(this, what, error);
        return;
    }
    if (resetHistory) {
        m_commitModel->reset();
    }
    refreshPanels();
}

void MainWindow::runCheckout(const QString& what, const QString& target,
                             std::function<bool(RepoSession&, QString*)> action,
                             std::function<void()> done) {
    if (!m_asyncReads) {
        QString error;
        if (!action(*m_session, &error)) {
            QMessageBox::warning(this, what, error);
            return;
        }
        m_commitModel->reset();
        refreshPanels();
        if (done) {
            done();
        }
        return;
    }
    if (m_checkoutRunning) {
        setStatus(QStringLiteral("A checkout is still running."));
        return;
    }
    m_checkoutRunning = true;
    m_commitView->setBusy(true);
    setStatus(QStringLiteral("Checking out %1…").arg(target));
    struct Outcome {
        bool ok = false;
        QString error;
    };
    RepoSession* session = m_session;
    session->readAsync<Outcome>(
        this,
        [action = std::move(action)](RepoSession& s) {
            Outcome outcome;
            outcome.ok = action(s, &outcome.error);
            return outcome;
        },
        [this, session, what, done = std::move(done)](Outcome outcome) {
            m_checkoutRunning = false;
            m_commitView->setBusy(false);
            RepoContext* context = contextOf(session);
            if (!context) {
                return;
            }
            if (context != active()) {
                context->commitModel->reset();
                return;
            }
            if (!outcome.ok) {
                setStatus(QString());
                QMessageBox::warning(this, what, outcome.error);
                return;
            }
            m_commitModel->reset();
            refreshPanels();
            if (done) {
                done();
            }
        });
}

void MainWindow::checkoutLocalBranch(const QString& name) {
    runCheckout(
        QStringLiteral("Cannot check out %1").arg(name), name,
        [name](RepoSession& s, QString* error) { return s.checkoutBranch(name, error); },
        [this, name] {
            setStatus(QStringLiteral("On branch %1.").arg(name));
            // The branch may record different submodule commits than the one left behind.
            maybeAutoUpdateSubmodules();
        });
}

void MainWindow::applyToFiles(const QStringList& paths, StageOp op) {
    if (paths.isEmpty()) {
        return;
    }
    if (op == StageOp::Discard) {
        const QString what = paths.size() == 1 ? paths.front()
                                               : QStringLiteral("%1 files").arg(paths.size());
        if (!askYes(QStringLiteral("Discard changes"),
                    QStringLiteral("Discard all unstaged changes to %1? This cannot be undone; "
                                   "an untracked file is deleted.")
                        .arg(what))) {
            return;
        }
    }

    QStringList failures;
    for (const QString& path : paths) {
        QString error;
        bool ok = false;
        switch (op) {
        case StageOp::Stage: ok = m_session->stageFile(path, &error); break;
        case StageOp::Unstage: ok = m_session->unstageFile(path, &error); break;
        case StageOp::Discard: ok = m_session->discardFileChanges(path, &error); break;
        }
        if (!ok) {
            failures.append(QStringLiteral("%1: %2").arg(path, error));
        }
    }
    if (!failures.isEmpty()) {
        reportFailure(QStringLiteral("The change could not be applied"),
                      failures.join(QLatin1Char('\n')));
    }
    // Refresh either way: some paths may have gone through before one failed, and
    // the panel must show what the repository actually holds.
    refreshPanels();
}

void MainWindow::applyStagingRequest(const QString& path, int hunkIndex, const QVector<int>& lines,
                                     bool toIndex) {
    if (!m_session->isOpen() || path.isEmpty()) {
        return;
    }
    // The anchor the reader saw. The session re-derives the diff and refuses when
    // this hunk is no longer the one on screen, so a click cannot stage the wrong
    // change after the file moved underneath it.
    const FileDiff diff =
        toIndex ? m_session->workdirFileDiff(path) : m_session->stagedFileDiff(path);
    if (hunkIndex < 0 || static_cast<std::size_t>(hunkIndex) >= diff.hunks.size()) {
        return;
    }
    const DiffHunk& hunk = diff.hunks[static_cast<std::size_t>(hunkIndex)];
    const RepoSession::HunkAnchor anchor{hunk.oldStart, hunk.oldLines, hunk.newStart,
                                         hunk.newLines};

    QString error;
    const RepoSession::HunkApply what = toIndex ? RepoSession::HunkApply::Stage
                                                : RepoSession::HunkApply::Unstage;
    if (!m_session->applyHunkSelection(path, hunkIndex, lines, what, anchor, &error)) {
        reportFailure(QStringLiteral("Cannot move the change"), error);
        return;
    }
    refreshPanels();
    // Stay on the file. After a stage it still belongs to Unstaged while it has
    // anything left; after an unstage it belongs to Staged.
    revealFileAfterRefresh(path, !toIndex);
}

void MainWindow::applyDiscardRequest(const QString& path, int hunkIndex,
                                     const QVector<int>& lines) {
    if (!m_session->isOpen() || path.isEmpty()) {
        return;
    }
    const QString what = lines.isEmpty() ? QStringLiteral("hunk") : QStringLiteral("line");
    // Content that was never committed is not in the operation log, so a discard
    // cannot be undone. Ask once, plainly.
    if (!askYes(QStringLiteral("Discard change"),
                QStringLiteral("Throw away this %1 of the change in %2?\n\nIt was never "
                               "committed, so it cannot be recovered.")
                    .arg(what, path))) {
        return;
    }

    const FileDiff diff = m_session->workdirFileDiff(path);
    if (hunkIndex < 0 || static_cast<std::size_t>(hunkIndex) >= diff.hunks.size()) {
        return;
    }
    const DiffHunk& hunk = diff.hunks[static_cast<std::size_t>(hunkIndex)];
    const RepoSession::HunkAnchor anchor{hunk.oldStart, hunk.oldLines, hunk.newStart,
                                         hunk.newLines};
    QString error;
    if (!m_session->applyHunkSelection(path, hunkIndex, lines, RepoSession::HunkApply::Discard,
                                       anchor, &error)) {
        reportFailure(QStringLiteral("Cannot discard the change"), error);
        return;
    }
    refreshPanels();
    revealFileAfterRefresh(path, false);
}

void MainWindow::revealFileAfterRefresh(const QString& path, bool staged) {
    if (!m_details) {
        return;
    }
    if (m_details->selectPath(path, staged)) {
        renderFile(path, true, staged);
        return;
    }
    if (m_details->selectPath(path, !staged)) {
        renderFile(path, true, !staged);
    }
}

void MainWindow::commitStaged(const QString& message) {
    QString error;
    QString newId;
    const bool sign = m_details && m_details->signCommit();
    const bool amend = m_details && m_details->amendRequested();
    const bool push = m_details && m_details->pushAfterCommit();
    const bool ok = amend ? m_session->amendHead(message, sign, &newId, &error)
                          : m_session->createCommit(message, sign, &newId, &error);
    if (!ok) {
        reportFailure(amend ? QStringLiteral("Cannot amend the commit")
                            : QStringLiteral("Cannot commit"),
                      error);
        return;
    }
    // The new commit is HEAD; nothing is selected in the graph, and the commit
    // panel goes back to the working tree, which is now clean.
    m_selectedCommitId.clear();
    m_commitModel->reset();
    m_details->clearCommitMessage();
    refreshPanels();
    setStatus(amend ? QStringLiteral("Amended %1").arg(newId.left(7))
                    : QStringLiteral("Committed %1").arg(newId.left(7)));
    if (push) {
        startSync(SyncKind::Push);
    }
}

QString MainWindow::headCommitMessage() const {
    const QString headId = QString::fromStdString(m_session->info().headId);
    const int row = headId.isEmpty() ? -1 : m_commitModel->rowForCommit(headId);
    const CommitInfo* commit = row >= 0 ? m_commitModel->commitAt(row) : nullptr;
    if (!commit) {
        return QString();
    }
    QString text = QString::fromStdString(commit->summary);
    if (!commit->body.empty()) {
        text += QStringLiteral("\n\n") + QString::fromStdString(commit->body);
    }
    return text;
}

QColor MainWindow::headBranchColor() const {
    const QString head = m_commitModel ? m_commitModel->headBranchName() : QString();
    if (head.isEmpty()) {
        return theme::accent();
    }
    return theme::branchColor(m_commitModel->refColorIndex(head));
}

void MainWindow::discardAllChanges() {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    if (!askYes(QStringLiteral("Discard all changes"),
                QStringLiteral("Discard every uncommitted change, staged or not, and delete the "
                               "untracked files? This cannot be undone."))) {
        return;
    }
    QString error;
    if (!m_session->discardAllChanges(&error)) {
        reportFailure(QStringLiteral("Cannot discard the changes"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Discarded all changes."));
}

void MainWindow::stashAllChanges(const QString& message) {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    const std::size_t before = m_stashes.size();
    QString error;
    // Untracked files too: the form counts them among the files it stashes.
    if (!m_session->stashPush(message, true, {}, &error)) {
        reportFailure(QStringLiteral("Cannot stash"), error);
        return;
    }
    m_details->clearStashMessage();
    refreshPanels();
    setStatus(m_stashes.size() > before ? QStringLiteral("Stashed the working changes.")
                                        : QStringLiteral("Nothing to stash."));
}

void MainWindow::onCoauthorsRequested() {
    if (!m_session || !m_session->isOpen() || !m_details) {
        return;
    }
    CoAuthorDialog dialog(this);
    dialog.setSuggestions(m_session->recentAuthors(80));
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const std::vector<CoAuthor> chosen = dialog.authors();
    if (chosen.empty()) {
        return;
    }
    m_details->setCommitMessage(m_session->withCoAuthors(m_details->commitMessage(), chosen));
}

QString MainWindow::ignorePatternFor(const QString& path, const QString& kind) {
    if (kind == QStringLiteral("ext")) {
        const QString suffix = QFileInfo(path).suffix();
        return suffix.isEmpty() ? QString() : QStringLiteral("*.%1").arg(suffix);
    }
    if (kind == QStringLiteral("dir")) {
        const QString dir = QFileInfo(path).path();
        return dir.isEmpty() || dir == QStringLiteral(".") ? QString()
                                                            : QStringLiteral("/%1/").arg(dir);
    }
    return QStringLiteral("/%1").arg(path);
}

QString MainWindow::ignorePathspecFor(const QString& path, const QString& kind) {
    // `:(literal)` so a file whose name contains `*` or `[` is still itself, not a
    // pattern that matches somebody else's file.
    if (kind == QStringLiteral("ext")) {
        const QString suffix = QFileInfo(path).suffix();
        return suffix.isEmpty() ? QString() : QStringLiteral("*.%1").arg(suffix);
    }
    if (kind == QStringLiteral("dir")) {
        const QString dir = QFileInfo(path).path();
        return dir.isEmpty() || dir == QStringLiteral(".") ? QString()
                                                            : QStringLiteral(":(literal)%1/").arg(dir);
    }
    return QStringLiteral(":(literal)%1").arg(path);
}

void MainWindow::openLocalIgnore() {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    LocalIgnoreDialog dialog(m_session->localExcludePatterns(), m_session->skipWorktreePaths(),
                             this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    applyLocalIgnore(dialog.addedPatterns(), dialog.removedPatterns(), dialog.unhiddenFiles());
}

bool MainWindow::applyLocalIgnore(const QStringList& added, const QStringList& removed,
                                  const QStringList& unhidden) {
    QString error;
    for (const QString& pattern : added) {
        if (!m_session->ignorePath(pattern, true, &error)) {
            reportFailure(QStringLiteral("Cannot add the rule"), error);
            return false;
        }
    }
    for (const QString& pattern : removed) {
        if (!m_session->removeLocalExcludePattern(pattern, &error)) {
            reportFailure(QStringLiteral("Cannot remove the rule"), error);
            return false;
        }
    }
    if (!unhidden.isEmpty() && !m_session->setSkipWorktree(unhidden, false, &error)) {
        reportFailure(QStringLiteral("Cannot show the files again"), error);
        return false;
    }
    refreshPanels();
    return true;
}

void MainWindow::onIgnoreRequested(const QStringList& paths, const QString& scope,
                                   const QString& kind) {
    if (!m_session || !m_session->isOpen() || paths.isEmpty()) {
        return;
    }
    const bool globalExclude = scope == QStringLiteral("exclude");
    QString error;
    if (globalExclude) {
        // "Only for me" must not change what anyone else gets: no `git rm --cached`
        // (that stages a deletion for everyone). An untracked path gets a rule in
        // `.git/info/exclude`; a tracked file cannot be ignored, so its changes are
        // hidden here with skip-worktree instead.
        QSet<QString> tracked;
        for (const FileChange& change : m_session->status()) {
            if (change.kind != ChangeKind::Untracked) {
                tracked.insert(QString::fromStdString(change.path));
            }
        }
        QStringList hide;
        for (const QString& path : paths) {
            if (kind == QStringLiteral("file") && tracked.contains(path)) {
                hide << path;
                continue;
            }
            const QString pattern = ignorePatternFor(path, kind);
            if (!pattern.isEmpty() && !m_session->ignorePath(pattern, true, &error)) {
                reportFailure(QStringLiteral("Cannot ignore the path"), error);
                return;
            }
        }
        if (!hide.isEmpty() && !m_session->setSkipWorktree(hide, true, &error)) {
            reportFailure(QStringLiteral("Cannot hide the file"), error);
            return;
        }
        refreshPanels();
        setStatus(QStringLiteral("Hidden for you only: %1 (Repository → Local Ignore Rules… "
                                 "brings it back)")
                      .arg(paths.first()));
        return;
    }
    for (const QString& path : paths) {
        const QString pattern = ignorePatternFor(path, kind);
        const QString pathspec = ignorePathspecFor(path, kind);
        if (pattern.isEmpty() || pathspec.isEmpty()) {
            continue;
        }
        if (!m_session->ignorePath(pattern, globalExclude, &error)) {
            reportFailure(QStringLiteral("Cannot ignore the path"), error);
            return;
        }
        // Ignoring a tracked file is two actions: the pattern says "never show it",
        // and the index has to let go of it or the rule does nothing.
        if (!m_session->untrackPathspec(pathspec, &error)) {
            reportFailure(QStringLiteral("Cannot untrack the path"), error);
            return;
        }
    }
    refreshPanels();
    setStatus(QStringLiteral("Ignoring %1").arg(paths.first()));
}

void MainWindow::stageAllChanges() {
    QString error;
    if (!m_session->stageAll(&error)) {
        reportFailure(QStringLiteral("Cannot stage the changes"), error);
        return;
    }
    refreshPanels();
}

void MainWindow::updateUndoRedoActions() {
    if (!m_undoAction || !m_redoAction) {
        return;
    }
    const bool open = m_session && m_session->isOpen();
    const bool canUndo = open && m_session->canUndo();
    const bool canRedo = open && m_session->canRedo();
    m_undoAction->setEnabled(canUndo);
    m_redoAction->setEnabled(canRedo);
    m_undoAction->setToolTip(canUndo ? QStringLiteral("Undo %1").arg(m_session->nextUndoLabel())
                                     : QStringLiteral("Nothing to undo"));
    m_redoAction->setToolTip(canRedo ? QStringLiteral("Redo %1").arg(m_session->nextRedoLabel())
                                     : QStringLiteral("Nothing to redo"));
}

void MainWindow::undoLastOperation() {
    if (!m_session->isOpen()) {
        return;
    }
    QString label;
    QString error;
    if (!m_session->undo(&label, &error)) {
        reportFailure(QStringLiteral("Cannot undo"), error);
        return;
    }
    // The tip moved, so the graph is stale and the selected commit may no longer
    // be there at all.
    m_selectedCommitId.clear();
    m_commitModel->reset();
    refreshPanels();
    setStatus(QStringLiteral("Undid %1").arg(label));
}

void MainWindow::redoLastOperation() {
    if (!m_session->isOpen()) {
        return;
    }
    QString label;
    QString error;
    if (!m_session->redo(&label, &error)) {
        reportFailure(QStringLiteral("Cannot redo"), error);
        return;
    }
    m_selectedCommitId.clear();
    m_commitModel->reset();
    refreshPanels();
    setStatus(QStringLiteral("Redid %1").arg(label));
}

void MainWindow::buildCommands() {
    m_commands.clear();
    const auto add = [this](const QString& id, const QString& title, const QString& category,
                            const QKeySequence& shortcut, std::function<bool()> enabled,
                            std::function<void()> run) {
        Command command;
        command.id = id;
        command.title = title;
        command.category = category;
        command.shortcut = shortcut;
        command.enabled = std::move(enabled);
        command.run = std::move(run);
        m_commands.add(std::move(command));
    };
    const auto toggleColumn = [this](CommitModel::Column column) {
        return [this, column] {
            setCommitColumnVisible(column, m_commitView->isColumnHidden(column));
        };
    };

    add(QStringLiteral("app.open"), QStringLiteral("Open Repository…"),
        QStringLiteral("Application"), QKeySequence::Open, {}, [this] { onOpenRepositoryDialog(); });
    add(QStringLiteral("app.clone"), QStringLiteral("Clone Repository…"),
        QStringLiteral("Application"), QKeySequence(QStringLiteral("Ctrl+Shift+O")), {},
        [this] { cloneFromDialog(); });
    add(QStringLiteral("app.new-repository"), QStringLiteral("New Repository…"),
        QStringLiteral("Application"), {}, {}, [this] { createRepositoryFromDialog(); });
    add(QStringLiteral("app.quit"), QStringLiteral("Quit"), QStringLiteral("Application"),
        QKeySequence::Quit, {}, [this] { close(); });
    add(QStringLiteral("tab.new"), QStringLiteral("New Tab"), QStringLiteral("Application"),
        QKeySequence(QStringLiteral("Ctrl+T")), {}, [this] { onNewTabRequested(); });
    add(QStringLiteral("tab.close"), QStringLiteral("Close Tab"), QStringLiteral("Application"),
        QKeySequence(QStringLiteral("Ctrl+W")), [this] { return m_active >= 0; },
        [this] { closeContext(m_active); });
    add(QStringLiteral("app.preferences"), QStringLiteral("Preferences…"),
        QStringLiteral("Application"), QKeySequence::Preferences, {},
        [this] { openPreferences(); });

    const bool canUndo = m_session && m_session->canUndo();
    const bool canRedo = m_session && m_session->canRedo();
    add(QStringLiteral("edit.undo"),
        canUndo ? QStringLiteral("Undo %1").arg(m_session->nextUndoLabel())
                : QStringLiteral("Undo"),
        QStringLiteral("Edit"), QKeySequence::Undo,
        [this] { return m_session && m_session->canUndo(); },
        [this] { undoLastOperation(); });
    add(QStringLiteral("edit.redo"),
        canRedo ? QStringLiteral("Redo %1").arg(m_session->nextRedoLabel())
                : QStringLiteral("Redo"),
        QStringLiteral("Edit"), QKeySequence::Redo,
        [this] { return m_session && m_session->canRedo(); },
        [this] { redoLastOperation(); });

    add(QStringLiteral("view.branches"), QStringLiteral("Toggle Branches Panel"),
        QStringLiteral("View"), QKeySequence(QStringLiteral("Ctrl+B")), {},
        [this] { m_refsAction->setChecked(!m_refsAction->isChecked()); });
    add(QStringLiteral("view.details"), QStringLiteral("Toggle Details Panel"),
        QStringLiteral("View"), {}, {},
        [this] { m_detailsAction->setChecked(!m_detailsAction->isChecked()); });
    add(QStringLiteral("view.graph"), QStringLiteral("Show Commit Graph"), QStringLiteral("View"),
        {}, {}, [this] { showGraphPage(); });
    add(QStringLiteral("view.conflicts"), QStringLiteral("Show Conflicts"), QStringLiteral("View"), {},
        {}, [this] { showConflictsPage(); });
    add(QStringLiteral("view.search"), QStringLiteral("Find in History…"), QStringLiteral("View"),
        QKeySequence::Find, {}, [this] { showCommandPalette(true); });
    add(QStringLiteral("view.column.author"), QStringLiteral("Toggle Author Column"),
        QStringLiteral("View"), {}, {}, toggleColumn(CommitModel::Column::Author));
    add(QStringLiteral("view.column.date"), QStringLiteral("Toggle Date Column"), QStringLiteral("View"),
        {}, {}, toggleColumn(CommitModel::Column::Date));
    add(QStringLiteral("view.column.sha"), QStringLiteral("Toggle SHA Column"), QStringLiteral("View"),
        {}, {}, toggleColumn(CommitModel::Column::Sha));
    add(QStringLiteral("view.show-all-branches"), QStringLiteral("Show All Branches"),
        QStringLiteral("View"), {}, {}, [this] { clearVisibility(); });
    add(QStringLiteral("view.smart-visibility"), QStringLiteral("Toggle Focus on Current Branch"),
        QStringLiteral("View"), {}, {}, [this] { toggleSmart(); });
    add(QStringLiteral("view.preset.mine"), QStringLiteral("Visibility: My Branches"),
        QStringLiteral("View"), {}, {}, [this] { applyPreset(VisibilityPreset::Mine); });
    add(QStringLiteral("view.preset.recent"), QStringLiteral("Visibility: Active in 30 Days"),
        QStringLiteral("View"), {}, {}, [this] { applyPreset(VisibilityPreset::Recent30); });
    add(QStringLiteral("view.preset.prefix"), QStringLiteral("Visibility: Branches by Prefix…"),
        QStringLiteral("View"), {}, {}, [this] { applyPrefixPreset(); });
    add(QStringLiteral("view.preset.no-upstream"),
        QStringLiteral("Visibility: Branches Without Upstream"), QStringLiteral("View"), {}, {},
        [this] { applyPreset(VisibilityPreset::NoUpstream); });
    add(QStringLiteral("view.preset.ahead"), QStringLiteral("Visibility: Branches Ahead"),
        QStringLiteral("View"), {}, {}, [this] { applyPreset(VisibilityPreset::Ahead); });
    add(QStringLiteral("view.preset.behind"), QStringLiteral("Visibility: Branches Behind"),
        QStringLiteral("View"), {}, {}, [this] { applyPreset(VisibilityPreset::Behind); });

    add(QStringLiteral("remote.fetch"), QStringLiteral("Fetch"), QStringLiteral("Remote"), {}, {},
        [this] { startSync(SyncKind::Fetch); });
    add(QStringLiteral("remote.pull"), QStringLiteral("Pull"), QStringLiteral("Remote"), {}, {},
        [this] { startSync(SyncKind::Pull); });
    add(QStringLiteral("remote.push"), QStringLiteral("Push"), QStringLiteral("Remote"), {}, {},
        [this] { startSync(SyncKind::Push); });

    add(QStringLiteral("stash.push"), QStringLiteral("Stash Working Changes…"),
        QStringLiteral("Stash"), {}, {}, [this] { stashWorkingTree(); });
    add(QStringLiteral("stash.pop"), QStringLiteral("Pop Most Recent Stash"),
        QStringLiteral("Stash"), {}, {}, [this] { popTopStash(); });
    // One apply/drop per entry, rebuilt with the list: a stash has no name to
    // address it by, so the command has to carry the oid it was built against.
    if (m_session && m_session->isOpen()) {
        for (std::size_t i = 0; i < m_stashes.size(); ++i) {
            const QString oid = QString::fromStdString(m_stashes.at(i).oid);
            const QString label = QString::fromStdString(m_stashes.at(i).message);
            add(QStringLiteral("stash.apply:") + QString::number(i),
                QStringLiteral("Apply stash: %1").arg(label), QStringLiteral("Stash"), {}, {},
                [this, oid, label] { restoreStash(oid, false, label); });
            add(QStringLiteral("stash.drop:") + QString::number(i),
                QStringLiteral("Drop stash: %1").arg(label), QStringLiteral("Stash"), {}, {},
                [this, oid, label] { dropStash(oid, label); });
        }
    }

    add(QStringLiteral("remote.fetch-all"), QStringLiteral("Fetch All Remotes"),
        QStringLiteral("Remote"), {}, {}, [this] { startSync(SyncKind::FetchAll); });
    add(QStringLiteral("remote.pull-merge"), QStringLiteral("Pull (merge)"),
        QStringLiteral("Remote"), {}, {}, [this] {
            setPullMode(PullMode::Merge);
            startSync(SyncKind::Pull);
        });
    add(QStringLiteral("remote.pull-rebase"), QStringLiteral("Pull (rebase)"),
        QStringLiteral("Remote"), {}, {}, [this] {
            setPullMode(PullMode::Rebase);
            startSync(SyncKind::Pull);
        });
    add(QStringLiteral("remote.push-tags"), QStringLiteral("Push All Tags"),
        QStringLiteral("Remote"), {}, {}, [this] { pushAllTags(); });
    add(QStringLiteral("remote.push-force"), QStringLiteral("Force Push (with lease)…"),
        QStringLiteral("Remote"), {}, {}, [this] { forcePushInteractive(); });
    add(QStringLiteral("remote.autofetch"), QStringLiteral("Toggle Auto-fetch"),
        QStringLiteral("Remote"), {}, {}, [this] { setAutoFetch(!m_autoFetchEnabled); });
    add(QStringLiteral("branch.set-upstream"), QStringLiteral("Set Upstream…"),
        QStringLiteral("Remote"), {}, {}, [this] { setUpstreamInteractive(); });
    add(QStringLiteral("branch.preflight"),
        QStringLiteral("Toggle Check-for-Conflicts Before Merging"), QStringLiteral("Branch"),
        {}, {}, [this] { setPreflightEnabled(!m_preflightEnabled); });
    add(QStringLiteral("branch.rerere"), QStringLiteral("Toggle Remember Conflict Resolutions"),
        QStringLiteral("Branch"), {}, {}, [this] { setRerereEnabled(!rerereEnabled()); });
    add(QStringLiteral("branch.rerere-autoupdate"),
        QStringLiteral("Toggle Auto-stage Remembered Resolutions"), QStringLiteral("Branch"), {}, {},
        [this] { setRerereAutoupdate(!(m_session && m_session->rerereAutoupdate())); });
    add(QStringLiteral("branch.prevention"),
        QStringLiteral("Toggle Watch for Conflicts with the Base Branch"), QStringLiteral("Branch"),
        {}, {}, [this] { setPreventionEnabled(!m_preventionEnabled); });
    add(QStringLiteral("branch.prevention-check"),
        QStringLiteral("Check Conflicts with the Base Branch Now"), QStringLiteral("Branch"), {}, {},
        [this] { checkConflictsWithBase(false); });

    add(QStringLiteral("branch.new"), QStringLiteral("New Branch…"), QStringLiteral("Branch"), {}, {},
        [this] { createBranchFromSelection(); });
    add(QStringLiteral("worktree.new"), QStringLiteral("New Worktree…"), QStringLiteral("Branch"), {},
        {}, [this] { createWorktree(); });
    add(QStringLiteral("worktree.prune"), QStringLiteral("Prune Worktrees"), QStringLiteral("Branch"),
        {}, {}, [this] { pruneWorktrees(); });
    add(QStringLiteral("submodule.add"), QStringLiteral("Add Submodule…"),
        QStringLiteral("Submodules"), {}, {}, [this] { addSubmodule(); });
    add(QStringLiteral("submodule.init-all"), QStringLiteral("Initialize All Submodules"),
        QStringLiteral("Submodules"), {}, {}, [this] { initializeAllSubmodules(); });
    add(QStringLiteral("submodule.update-all"), QStringLiteral("Update All Submodules"),
        QStringLiteral("Submodules"), {}, {}, [this] { updateAllSubmodules(); });
    add(QStringLiteral("submodule.sync-all"), QStringLiteral("Sync All Submodules"),
        QStringLiteral("Submodules"), {}, {}, [this] { syncAllSubmodules(); });
    add(QStringLiteral("submodule.autoupdate"),
        QStringLiteral("Toggle Update Submodules After Fetch/Checkout/Merge"),
        QStringLiteral("Submodules"), {}, {},
        [this] { setSubmoduleAutoupdate(!m_submoduleAutoupdate); });
    add(QStringLiteral("submodule.autoupdate-remote"),
        QStringLiteral("Toggle Submodule Auto-update Tracking Remote"),
        QStringLiteral("Submodules"), {}, {},
        [this] { setSubmoduleAutoupdateRemote(!m_submoduleAutoupdateRemote); });
    add(QStringLiteral("branch.merge"), QStringLiteral("Merge Branch Into Current…"),
        QStringLiteral("Branch"), {}, {}, [this] { mergeBranchIntoCurrent(); });
    add(QStringLiteral("branch.squash-merge"), QStringLiteral("Squash Merge Into Current…"),
        QStringLiteral("Branch"), {}, {}, [this] { squashMergeIntoCurrent(); });
    add(QStringLiteral("history.interactive-rebase"),
        QStringLiteral("Interactive Rebase from Here…"), QStringLiteral("History"), {}, {},
        [this] {
            if (!m_selectedCommitId.isEmpty()) {
                interactiveRebaseFromCommit(m_selectedCommitId);
            } else {
                setStatus(QStringLiteral("Select a commit to rebase from."));
            }
        });
    add(QStringLiteral("history.amend-rebase"),
        QStringLiteral("Amend Stopped Rebase Commit (keep message)"),
        QStringLiteral("History"), {},
        [this] { return m_session && m_session->rebaseInProgress(); },
        [this] { amendStoppedRebaseCommit(); });
    add(QStringLiteral("history.compare"),
        QStringLiteral("Compare Selected Commit with HEAD"), QStringLiteral("History"), {}, {},
        [this] {
            if (m_selectedCommitId.isEmpty()) {
                setStatus(QStringLiteral("Select a commit to compare with HEAD."));
                return;
            }
            showComparison(m_selectedCommitId, QStringLiteral("HEAD"));
        });
    add(QStringLiteral("history.compare-worktree"),
        QStringLiteral("Compare Selected Commit with Working Tree"), QStringLiteral("History"), {},
        [this] { return !m_selectedCommitId.isEmpty(); },
        [this] {
            if (m_selectedCommitId.isEmpty()) {
                setStatus(QStringLiteral("Select a commit to compare with the working tree."));
                return;
            }
            showComparisonWorkingTree(m_selectedCommitId);
        });
    add(QStringLiteral("history.compare-base"),
        QStringLiteral("Compare Merge Base with Selected Commit"), QStringLiteral("History"), {},
        [this] { return !m_selectedCommitId.isEmpty(); },
        [this] {
            if (m_selectedCommitId.isEmpty()) {
                setStatus(QStringLiteral("Select a commit to compare its merge base with."));
                return;
            }
            // mergeBase(HEAD, sel)..sel: what the selected branch added on top,
            // ignoring what HEAD has done since — `git diff HEAD...sel`.
            showComparisonMergeBase(QStringLiteral("HEAD"), m_selectedCommitId);
        });
    add(QStringLiteral("history.restore-file"),
        QStringLiteral("Restore Viewed File from the Revision"), QStringLiteral("History"), {}, {},
        [this] { restoreRenderedFileFromView(); });
    add(QStringLiteral("history.blame"), QStringLiteral("Toggle Blame on the Viewed File"),
        QStringLiteral("History"), {}, {},
        [this] {
            if (m_renderedPath.isEmpty()) {
                setStatus(QStringLiteral("Open a file first."));
                return;
            }
            m_diffView->setBlameView(!m_diffView->blameView());
            if (m_diffView->blameView()) {
                showBlameFor(m_renderedPath);
            }
        });
    add(QStringLiteral("history.file-history"),
        QStringLiteral("File History of the Viewed File"), QStringLiteral("History"), {}, {},
        [this] { showFileHistory(m_renderedPath); });
    add(QStringLiteral("history.reflog"), QStringLiteral("Show the Reflog"), QStringLiteral("History"),
        {}, {}, [this] { showReflog(); });
    add(QStringLiteral("view.columns-rich"), QStringLiteral("Graph Columns: Rich (Icons and Names)"),
        QStringLiteral("View"), {}, {},
        [this] { setColumnMode(GraphColumnMode::Rich, /*setWidths=*/true); });
    add(QStringLiteral("view.columns-text"), QStringLiteral("Graph Columns: Text (Names Only)"),
        QStringLiteral("View"), {}, {},
        [this] { setColumnMode(GraphColumnMode::Text, /*setWidths=*/true); });
    add(QStringLiteral("view.columns-compact"), QStringLiteral("Graph Columns: Compact"),
        QStringLiteral("View"), {}, {},
        [this] { setColumnMode(GraphColumnMode::Compact, /*setWidths=*/true); });
    add(QStringLiteral("view.timeline-none"), QStringLiteral("Timeline Separators: Off"),
        QStringLiteral("View"), {}, {}, [this] { setTimelineGrouping(TimelinePeriod::None); });
    add(QStringLiteral("view.timeline-day"), QStringLiteral("Timeline Separators: By Day"),
        QStringLiteral("View"), {}, {}, [this] { setTimelineGrouping(TimelinePeriod::Day); });
    add(QStringLiteral("view.timeline-week"), QStringLiteral("Timeline Separators: By Week"),
        QStringLiteral("View"), {}, {}, [this] { setTimelineGrouping(TimelinePeriod::Week); });
    add(QStringLiteral("view.timeline-month"), QStringLiteral("Timeline Separators: By Month"),
        QStringLiteral("View"), {}, {}, [this] { setTimelineGrouping(TimelinePeriod::Month); });
    add(QStringLiteral("view.notifications"), QStringLiteral("Show Notifications"),
        QStringLiteral("View"), {}, {}, [this] { showNotifications(); });
    add(QStringLiteral("repo.sparse-checkout"), QStringLiteral("Sparse Checkout…"),
        QStringLiteral("Repository"), {}, {}, [this] { editSparseCheckout(); });
    add(QStringLiteral("repo.profiles"), QStringLiteral("Identity Profiles…"),
        QStringLiteral("Repository"), {}, {}, [this] { editProfiles(); });
    add(QStringLiteral("repo.gitflow"), QStringLiteral("Gitflow…"), QStringLiteral("Repository"),
        {}, {}, [this] { openGitFlow(); });
    add(QStringLiteral("repo.lfs"), QStringLiteral("Git LFS…"), QStringLiteral("Repository"), {}, {},
        [this] { openLfs(); });
    add(QStringLiteral("view.wrap-diff"), QStringLiteral("Toggle Word Wrap in the Diff"),
        QStringLiteral("View"), {}, {},
        [this] { setDiffWrap(!(m_diffView && m_diffView->wrapEnabled())); });
    add(QStringLiteral("branch.delete"), QStringLiteral("Delete Selected Branch"),
        QStringLiteral("Branch"), {}, {}, [this] { deleteSelectedBranch(); });
    add(QStringLiteral("branch.delete-selected"),
        QStringLiteral("Delete Selected Branches"), QStringLiteral("Branch"), {}, {},
        [this] { deleteSelectedBranches(); });
    add(QStringLiteral("branch.delete-merged"),
        QStringLiteral("Delete Branches Merged Into Target…"), QStringLiteral("Branch"), {}, {},
        [this] { deleteMergedBranches(); });
    add(QStringLiteral("branch.set-upstream-selected"),
        QStringLiteral("Set Upstream for Selected Branches…"), QStringLiteral("Branch"), {}, {},
        [this] { setUpstreamsForSelected(); });

    add(QStringLiteral("commit.stageAll"), QStringLiteral("Stage All Changes"),
        QStringLiteral("Commit"), {}, {}, [this] { stageAllChanges(); });
    add(QStringLiteral("commit.panel"), QStringLiteral("Go to Commit Message"),
        QStringLiteral("Commit"), {}, {}, [this] {
            m_details->focusCommitMessage();
        });
    add(QStringLiteral("commit.coauthors"), QStringLiteral("Add Co-authors…"),
        QStringLiteral("Commit"), {}, [this] { return m_session && m_session->isOpen(); },
        [this] { onCoauthorsRequested(); });
    add(QStringLiteral("commit.template"), QStringLiteral("Apply Commit Template"),
        QStringLiteral("Commit"), {}, [this] { return m_session && m_session->isOpen(); },
        [this] {
            const QString text = m_session->commitTemplate();
            if (text.isEmpty()) {
                setStatus(QStringLiteral("No commit.template is configured."));
                return;
            }
            m_details->setCommitMessage(text);
        });

    add(QStringLiteral("repo.apply-patch"), QStringLiteral("Apply Patch…"),
        QStringLiteral("Repository"), {}, {}, [this] { applyPatchInteractive(); });
    add(QStringLiteral("repo.terminal"), QStringLiteral("Open Repository in Terminal"),
        QStringLiteral("Repository"), {}, [this] { return m_session && m_session->isOpen(); },
        [this] { openTerminal(); });
    add(QStringLiteral("repo.localIgnore"), QStringLiteral("Local Ignore Rules…"),
        QStringLiteral("Repository"), {}, [this] { return m_session && m_session->isOpen(); },
        [this] { openLocalIgnore(); });
    add(QStringLiteral("repo.hooks"), QStringLiteral("Git Hooks…"),
        QStringLiteral("Repository"), {}, [this] { return m_session && m_session->isOpen(); },
        [this] { openHooks(); });
    add(QStringLiteral("commit.sign"), QStringLiteral("Toggle Commit Signing"),
        QStringLiteral("Commit"), {}, [this] { return m_session && m_session->isOpen(); }, [this] {
            if (!m_session || !m_session->isOpen()) {
                return;
            }
            const bool next = !m_session->commitSigningEnabled();
            QString error;
            if (!m_session->setConfigValue(QStringLiteral("commit.gpgsign"),
                                           next ? QStringLiteral("true") : QStringLiteral("false"),
                                           &error)) {
                reportFailure(QStringLiteral("Cannot change signing"), error);
                return;
            }
            if (m_details) {
                m_details->setSignCommit(next);
            }
            setStatus(next ? QStringLiteral("Commits will be signed.")
                           : QStringLiteral("Commits will not be signed."));
        });
    add(QStringLiteral("history.verify-signature"), QStringLiteral("Verify Commit Signature"),
        QStringLiteral("History"), {}, [this] { return !m_selectedCommitId.isEmpty(); },
        [this] { verifyCommitSignature(m_selectedCommitId); });
    add(QStringLiteral("repo.optimize"), QStringLiteral("Optimize Repository…"),
        QStringLiteral("Repository"), {}, {},
        [this] { runMaintenance(MaintenanceTask::Optimize); });
    add(QStringLiteral("repo.maintenance"), QStringLiteral("Run Scheduled Maintenance"),
        QStringLiteral("Repository"), {}, {},
        [this] { runMaintenance(MaintenanceTask::Maintenance); });
    add(QStringLiteral("repo.commit-graph"), QStringLiteral("Write Commit-Graph"),
        QStringLiteral("Repository"), {}, {},
        [this] { runMaintenance(MaintenanceTask::CommitGraph); });
    add(QStringLiteral("repo.multi-pack-index"), QStringLiteral("Write Multi-Pack-Index"),
        QStringLiteral("Repository"), {}, {},
        [this] { runMaintenance(MaintenanceTask::MultiPackIndex); });
    add(QStringLiteral("repo.gc"), QStringLiteral("Garbage Collect (gc)"),
        QStringLiteral("Repository"), {}, {}, [this] { runMaintenance(MaintenanceTask::Gc); });
    add(QStringLiteral("repo.fsck"), QStringLiteral("Check Integrity (fsck)"),
        QStringLiteral("Repository"), {}, {}, [this] { runMaintenance(MaintenanceTask::Fsck); });

    add(QStringLiteral("help.about"), QStringLiteral("About GitMycelium"), QStringLiteral("Help"), {}, {},
        [this] { showAboutBox(this); });
    add(QStringLiteral("help.update"), QStringLiteral("Check for Updates"), QStringLiteral("Help"), {},
        {}, [this] { showUpdateDialog(); });

    // One checkout per local branch: the reason a palette beats a menu. Rebuilt
    // here rather than stored, because the set of branches changes under us.
    if (m_session && m_session->isOpen()) {
        const QString current = QString::fromStdString(m_session->info().headBranch);
        for (const RefInfo& ref : m_session->refs()) {
            if (ref.kind != RefKind::LocalBranch) {
                continue;
            }
            const QString name = QString::fromStdString(ref.shortName);
            if (name == current) {
                continue;
            }
            add(QStringLiteral("branch.checkout:") + name,
                QStringLiteral("Checkout %1").arg(name), QStringLiteral("Branch"), {}, {},
                [this, name] { checkoutBranchFromGraph(name, RefKind::LocalBranch); });
        }
        for (const RefInfo& ref : m_session->refs()) {
            if (ref.kind != RefKind::Tag) {
                continue;
            }
            const QString tag = QString::fromStdString(ref.shortName);
            add(QStringLiteral("tag.push:") + tag, QStringLiteral("Push tag %1").arg(tag),
                QStringLiteral("Remote"), {}, {}, [this, tag] { pushTagToRemote(tag); });
        }
        for (const WorktreeEntry& worktree : m_worktrees) {
            if (worktree.current) {
                continue;
            }
            const QString label = QString::fromStdString(worktree.name);
            const QString path = QString::fromStdString(worktree.path);
            add(QStringLiteral("worktree.open:") + label,
                QStringLiteral("Open Worktree %1").arg(label), QStringLiteral("Branch"), {}, {},
                [this, path] { openWorktree(path); });
        }
        for (const SubmoduleEntry& submodule : m_submodules) {
            if (!submodule.initialized) {
                continue;
            }
            const QString path = QString::fromStdString(submodule.path);
            add(QStringLiteral("submodule.open:") + path,
                QStringLiteral("Open Submodule %1").arg(path), QStringLiteral("Submodules"), {},
                {}, [this, path] { openSubmodule(path); });
        }
    }
}

std::vector<PaletteRef> MainWindow::paletteRefs() const {
    std::vector<PaletteRef> refs;
    if (!m_session || !m_session->isOpen()) {
        return refs;
    }
    for (const RefInfo& ref : m_session->refs()) {
        if (ref.kind != RefKind::LocalBranch && ref.kind != RefKind::RemoteBranch &&
            ref.kind != RefKind::Tag) {
            continue;
        }
        PaletteRef entry;
        entry.name = QString::fromStdString(ref.shortName);
        entry.kind = ref.kind;
        entry.target = QString::fromStdString(ref.target);
        entry.colorIndex = m_commitModel ? m_commitModel->refColorIndex(entry.name) : 0;
        refs.push_back(std::move(entry));
    }
    return refs;
}

void MainWindow::preparePalette(CommandPalette& palette, bool preferHistory) {
    palette.setRefs(paletteRefs());
    if (m_session && m_session->isOpen()) {
        palette.setSession(m_session, historyFilter().firstParentHistory);
    }
    palette.setPreferHistory(preferHistory);
}

void MainWindow::showCommandPalette(bool preferHistory) {
    // Refresh first: the branch commands are per-branch, and a fetch may have
    // added one since the last rebuild.
    refreshCommands();
    CommandPalette palette(m_commands, this);
    preparePalette(palette, preferHistory);
    if (palette.exec() == QDialog::Accepted) {
        // Act after the palette has closed: a command that opens its own dialog
        // must not open it underneath a modal palette.
        actOnPaletteChoice(palette);
    }
}

void MainWindow::actOnPaletteChoice(const CommandPalette& palette) {
    switch (palette.choice()) {
    case CommandPalette::Choice::Command:
        runCommand(palette.chosenId());
        break;
    case CommandPalette::Choice::Ref: {
        QString target = palette.chosenTarget();
        if (target.isEmpty() && m_session) {
            target = m_session->resolveRevision(palette.chosenId());
        }
        revealCommit(target);
        break;
    }
    case CommandPalette::Choice::Commit:
        revealCommit(palette.chosenId());
        break;
    case CommandPalette::Choice::None:
        break;
    }
}

void MainWindow::runCommand(const QString& id) {
    for (const Command& command : m_commands.commands()) {
        if (command.id != id) {
            continue;
        }
        if (!command.isEnabled()) {
            setStatus(QStringLiteral("“%1” is not available right now.").arg(command.title));
            return;
        }
        // A copy, because running it refreshes the window and the refresh rebuilds the
        // registry: the closure being executed, and the names it captured, are freed
        // under it — a checkout from the palette crashed on its own branch name.
        const std::function<void()> run = command.run;
        if (run) {
            run();
        }
        return;
    }
    setStatus(QStringLiteral("No such command: %1").arg(id));
}

QStringList MainWindow::commandPaletteForDiagnostics(const QString& query, bool runFirst) {
    refreshCommands();
    QStringList report;
    for (const Command* command : m_commands.search(query)) {
        report << QStringLiteral("%1 %2\t%3")
                      .arg(command->isEnabled() ? QStringLiteral("on ") : QStringLiteral("off"),
                           command->id, command->title);
    }
    report << QStringLiteral("— %1 match(es)").arg(report.size());
    if (runFirst) {
        CommandPalette palette(m_commands);
        preparePalette(palette, false);
        const QString chosen = palette.chooseFirst(query);
        report << QStringLiteral("chosen: %1")
                      .arg(chosen.isEmpty() ? QStringLiteral("(none)") : chosen);
        actOnPaletteChoice(palette);
    }
    return report;
}

QStringList MainWindow::undoForDiagnostics() {
    QStringList report;
    if (!m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    QString label;
    QString error;
    const bool ok = m_session->undo(&label, &error);
    report << (ok ? QStringLiteral("undo: %1").arg(label)
                  : QStringLiteral("undo failed: %1").arg(error));
    report << QStringLiteral("head: %1")
                  .arg(QString::fromStdString(m_session->info().headBranch));
    report << QStringLiteral("canUndo=%1 canRedo=%2")
                  .arg(m_session->canUndo() ? QStringLiteral("yes") : QStringLiteral("no"),
                       m_session->canRedo() ? QStringLiteral("yes") : QStringLiteral("no"));
    return report;
}

QStringList MainWindow::redoForDiagnostics() {
    QStringList report;
    if (!m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    QString label;
    QString error;
    const bool ok = m_session->redo(&label, &error);
    report << (ok ? QStringLiteral("redo: %1").arg(label)
                  : QStringLiteral("redo failed: %1").arg(error));
    report << QStringLiteral("head: %1")
                  .arg(QString::fromStdString(m_session->info().headBranch));
    report << QStringLiteral("canUndo=%1 canRedo=%2")
                  .arg(m_session->canUndo() ? QStringLiteral("yes") : QStringLiteral("no"),
                       m_session->canRedo() ? QStringLiteral("yes") : QStringLiteral("no"));
    return report;
}

QStringList MainWindow::oplogForDiagnostics() const {
    return m_session ? m_session->oplogDescription() : QStringList();
}

QStringList MainWindow::stageHunkForDiagnostics(const QString& path, int hunkIndex, bool unstage) {
    return stageLineForDiagnostics(path, hunkIndex, -1, unstage);
}

QStringList MainWindow::stageLineForDiagnostics(const QString& path, int hunkIndex, int lineIndex,
                                                bool unstage) {
    QStringList report;
    if (!m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const bool toIndex = !unstage;
    const FileDiff diff =
        toIndex ? m_session->workdirFileDiff(path) : m_session->stagedFileDiff(path);
    if (hunkIndex < 0 || static_cast<std::size_t>(hunkIndex) >= diff.hunks.size()) {
        report << QStringLiteral("no hunk %1 in %2").arg(hunkIndex).arg(path);
        return report;
    }
    const DiffHunk& hunk = diff.hunks[static_cast<std::size_t>(hunkIndex)];
    const RepoSession::HunkAnchor anchor{hunk.oldStart, hunk.oldLines, hunk.newStart,
                                         hunk.newLines};
    QVector<int> lines;
    if (lineIndex >= 0) {
        lines.push_back(lineIndex);
    }
    QString error;
    const RepoSession::HunkApply what = unstage ? RepoSession::HunkApply::Unstage
                                                : RepoSession::HunkApply::Stage;
    const bool ok = m_session->applyHunkSelection(path, hunkIndex, lines, what, anchor, &error);
    if (ok) {
        report << QStringLiteral("moved %1 %2 of %3")
                      .arg(lines.isEmpty() ? QStringLiteral("hunk") : QStringLiteral("line"))
                      .arg(hunkIndex)
                      .arg(path);
    } else {
        report << QStringLiteral("failed: %1").arg(error);
    }
    report << QStringLiteral("staged diff: %1 hunk(s)")
                  .arg(m_session->stagedFileDiff(path).hunks.size());
    report << QStringLiteral("unstaged diff: %1 hunk(s)")
                  .arg(m_session->workdirFileDiff(path).hunks.size());
    return report;
}

QStringList MainWindow::discardHunkForDiagnostics(const QString& path, int hunkIndex,
                                                  int lineIndex) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const FileDiff diff = m_session->workdirFileDiff(path);
    if (hunkIndex < 0 || static_cast<std::size_t>(hunkIndex) >= diff.hunks.size()) {
        report << QStringLiteral("no hunk %1 in %2").arg(hunkIndex).arg(path);
        return report;
    }
    const DiffHunk& hunk = diff.hunks[static_cast<std::size_t>(hunkIndex)];
    const RepoSession::HunkAnchor anchor{hunk.oldStart, hunk.oldLines, hunk.newStart,
                                         hunk.newLines};
    QVector<int> lines;
    if (lineIndex >= 0) {
        lines.push_back(lineIndex);
    }
    QString error;
    if (!m_session->applyHunkSelection(path, hunkIndex, lines, RepoSession::HunkApply::Discard,
                                       anchor, &error)) {
        report << QStringLiteral("failed: %1").arg(error);
        return report;
    }
    report << QStringLiteral("discarded %1 %2 of %3")
                  .arg(lines.isEmpty() ? QStringLiteral("hunk") : QStringLiteral("line"))
                  .arg(hunkIndex)
                  .arg(path);
    report << QStringLiteral("unstaged diff: %1 hunk(s)")
                  .arg(m_session->workdirFileDiff(path).hunks.size());
    report << QStringLiteral("staged diff: %1 hunk(s)")
                  .arg(m_session->stagedFileDiff(path).hunks.size());
    return report;
}

QString MainWindow::selectedRefShortName() const {
    const QModelIndex index = m_refView->currentIndex();
    if (!index.isValid() || !index.parent().isValid()) {
        return {};
    }
    return RefModel::refShortName(index);
}

bool MainWindow::selectedRefIsLocalBranch() const {
    const QModelIndex index = m_refView->currentIndex();
    if (!index.isValid() || !index.parent().isValid()) {
        return false;
    }
    return index.data(Qt::UserRole).toString().startsWith(QStringLiteral("refs/heads/"));
}

QStringList MainWindow::selectedLocalBranches() const {
    QStringList out;
    if (!m_refView || !m_refView->selectionModel()) {
        return out;
    }
    for (const QModelIndex& index : m_refView->selectionModel()->selectedRows()) {
        // Sections, folders, stashes, worktrees and submodules are rows too, and a
        // group operation handed one of them would act on nothing or on the wrong
        // thing. Only local branches are collected.
        if (!RefModel::isRef(index) || RefModel::refKind(index) != RefKind::LocalBranch) {
            continue;
        }
        const QString name = RefModel::refShortName(index);
        if (!name.isEmpty() && !out.contains(name)) {
            out << name;
        }
    }
    return out;
}

void MainWindow::refreshRefs() {
    m_refModel->setHeadBranch(QString::fromStdString(m_session->info().headBranch));
    m_refModel->rebuild(m_session->refs());
}

void MainWindow::applyRefFilter() {
    if (!m_refModel) {
        return;
    }
    const QString pattern = m_refFilter ? m_refFilter->text() : QString();
    const bool regex = m_refFilterRegex && m_refFilterRegex->isChecked();
    m_refModel->setFilter(pattern, regex);

    const QString error = m_refModel->filterError();
    if (m_refFilter) {
        // A broken pattern matches nothing, and an empty list with no reason is
        // the worst of both worlds, so the reason is carried on the field itself.
        m_refFilter->setToolTip(error.isEmpty()
                                    ? QStringLiteral("Substring match, case does not matter")
                                    : QStringLiteral("Invalid regular expression: %1").arg(error));
        if (m_refFilter->property("filterError").toBool() != !error.isEmpty()) {
            m_refFilter->setProperty("filterError", !error.isEmpty());
            m_refFilter->style()->unpolish(m_refFilter);
            m_refFilter->style()->polish(m_refFilter);
        }
    }
    syncRefTreeExpansion();
}

void MainWindow::togglePin(const QModelIndex& index) {
    const QString key = RefModel::pinKey(index);
    if (key.isEmpty()) {
        return; // a section is not something you pin
    }
    const bool wasPinned = m_pins.isPinned(key);
    m_pins.setPinned(key, !wasPinned);
    m_refModel->setPinned(m_pins.names());
    // A pinned branch also earns the leftmost lane, so the commit model has to
    // hear about it — this is the "pin to left" in the graph, not just the list.
    m_commitModel->setPinnedBranches(m_pins.names());
    syncRefTreeExpansion();
    setStatus(wasPinned ? QStringLiteral("Unpinned %1").arg(key)
                        : QStringLiteral("Pinned %1 to the top").arg(key));
}

std::vector<std::string> MainWindow::smartVisibleRefs() const {
    std::vector<std::string> out;
    if (!m_session || !m_session->isOpen()) {
        return out;
    }
    const auto add = [&](const QString& full) {
        if (full.isEmpty()) {
            return;
        }
        const std::string name = full.toStdString();
        if (std::find(out.begin(), out.end(), name) == out.end()) {
            out.push_back(name);
        }
    };

    const std::string head = m_session->info().headBranch;
    const QString target = m_session->targetBranch();
    if (!head.empty()) {
        add(QStringLiteral("refs/heads/") + QString::fromStdString(head));
    }
    add(target);

    // Each side's upstream, plus the other side of it: a local branch brings its
    // remote-tracking sibling, a remote-tracking branch brings its local one, so
    // the pair stays visible together.
    const auto addUpstreamOf = [&](const QString& branchShort) {
        if (branchShort.isEmpty()) {
            return;
        }
        const UpstreamInfo upstream = m_session->upstreamInfo(branchShort);
        if (!upstream.configured || upstream.name.empty()) {
            return;
        }
        const QString remoteTracking = QString::fromStdString(upstream.name); // origin/main
        add(QStringLiteral("refs/remotes/") + remoteTracking);
        const int slash = remoteTracking.indexOf(QLatin1Char('/'));
        if (slash >= 0) {
            add(QStringLiteral("refs/heads/") + remoteTracking.mid(slash + 1));
        }
    };

    if (!head.empty()) {
        addUpstreamOf(QString::fromStdString(head));
    }
    const QString localPrefix = QStringLiteral("refs/heads/");
    const QString remotePrefix = QStringLiteral("refs/remotes/");
    if (target.startsWith(localPrefix)) {
        addUpstreamOf(target.mid(localPrefix.size()));
    } else if (target.startsWith(remotePrefix)) {
        const QString rest = target.mid(remotePrefix.size());
        const int slash = rest.indexOf(QLatin1Char('/'));
        if (slash >= 0) {
            add(localPrefix + rest.mid(slash + 1));
        }
    }
    return out;
}

std::vector<std::string> MainWindow::visibilityWantedRefs(bool* active) const {
    *active = false;
    std::vector<std::string> out;
    if (!m_session || !m_session->isOpen()) {
        return out;
    }
    const auto addUnique = [&](const std::string& full) {
        if (full.empty()) {
            return;
        }
        if (std::find(out.begin(), out.end(), full) == out.end()) {
            out.push_back(full);
        }
    };

    if (m_visibility.smart()) {
        *active = true;
        for (const std::string& full : smartVisibleRefs()) {
            addUnique(full);
        }
        return out;
    }

    const VisibilityPreset preset = m_visibility.preset();
    if (preset == VisibilityPreset::None) {
        return out;
    }
    *active = true;

    // HEAD always stays: the branch you are on is not something a filter hides.
    const std::string head = m_session->info().headBranch;
    if (!head.empty()) {
        addUnique("refs/heads/" + head);
    }

    const QString userEmail = m_session->configValue(QStringLiteral("user.email")).trimmed();
    const QString userName = m_session->configValue(QStringLiteral("user.name")).trimmed();
    const std::int64_t now = QDateTime::currentSecsSinceEpoch();
    const QString presetPrefix = m_visibility.presetPrefix();

    for (const RefMeta& meta : m_session->refMeta()) {
        if (!meta.local) {
            continue;
        }
        const QString shortName = QString::fromStdString(meta.shortName);
        bool match = false;
        switch (preset) {
        case VisibilityPreset::Mine:
            if (!userEmail.isEmpty()) {
                match = QString::fromStdString(meta.authorEmail).compare(userEmail,
                                                                        Qt::CaseInsensitive) == 0;
            } else if (!userName.isEmpty()) {
                match = QString::fromStdString(meta.authorName) == userName;
            }
            break;
        case VisibilityPreset::Recent30:
            match = meta.commitUnix > 0 && (now - meta.commitUnix) <= 30LL * 24 * 3600;
            break;
        case VisibilityPreset::Prefix:
            match = !presetPrefix.isEmpty() && shortName.startsWith(presetPrefix);
            break;
        case VisibilityPreset::NoUpstream:
            match = meta.upstream.empty();
            break;
        case VisibilityPreset::Ahead:
            match = meta.ahead;
            break;
        case VisibilityPreset::Behind:
            match = meta.behind;
            break;
        case VisibilityPreset::None:
            break;
        }
        if (match) {
            addUnique(meta.fullName);
            if (!meta.upstream.empty()) {
                addUnique("refs/remotes/" + meta.upstream);
            }
        }
    }
    return out;
}

QStringList MainWindow::graphHiddenBranchNames() const {
    bool active = false;
    const std::vector<std::string> wanted = visibilityWantedRefs(&active);
    if (!active) {
        return m_visibility.hiddenNames();
    }
    QStringList hidden;
    for (const RefInfo& ref : m_session->refs()) {
        if (ref.kind != RefKind::LocalBranch && ref.kind != RefKind::RemoteBranch) {
            continue;
        }
        if (std::find(wanted.begin(), wanted.end(), ref.fullName) == wanted.end()) {
            hidden << QString::fromStdString(ref.shortName);
        }
    }
    return hidden;
}

HistoryFilter MainWindow::historyFilter() const {
    HistoryFilter filter;
    filter.scope = HistoryScope::AllRefs;
    filter.firstParentHistory = m_mergedCommitsAction && !m_mergedCommitsAction->isChecked();
    if (!m_session || !m_session->isOpen()) {
        return filter; // an empty visibleRefs is "every ref"
    }
    if (filter.firstParentHistory) {
        filter.foldInto = m_session->info().trunkBranch;
    }
    bool active = false;
    const std::vector<std::string> wanted = visibilityWantedRefs(&active);
    if (!active && m_visibility.empty()) {
        return filter;
    }
    if (active && wanted.empty()) {
        // Smart with no anchor (an unborn or detached HEAD and no target): an empty
        // filter is "everything"; a blank graph would be a lie, not a filter.
        return filter;
    }

    const QString head = QString::fromStdString(m_session->info().headBranch);
    for (const RefInfo& ref : m_session->refs()) {
        // Only branches are filtered: a tag is a name for a commit, and taking it
        // out of the walk would drop the label that says a release exists.
        const bool branch =
            ref.kind == RefKind::LocalBranch || ref.kind == RefKind::RemoteBranch;
        bool visible = true;
        if (active) {
            if (branch) {
                visible = std::find(wanted.begin(), wanted.end(), ref.fullName) != wanted.end();
            }
        } else if (branch) {
            const QString name = QString::fromStdString(ref.shortName);
            if (m_visibility.soloActive()) {
                visible = m_visibility.isSolo(name) || name == head;
            } else {
                visible = !m_visibility.isHidden(name) || name == head;
            }
        }
        if (visible) {
            filter.visibleRefs.push_back(ref.fullName);
        }
    }
    return filter;
}

namespace {

QString presetLabel(VisibilityPreset preset) {
    switch (preset) {
    case VisibilityPreset::Mine: return QStringLiteral("my branches");
    case VisibilityPreset::Recent30: return QStringLiteral("branches active in 30 days");
    case VisibilityPreset::Prefix: return QStringLiteral("branches by prefix");
    case VisibilityPreset::NoUpstream: return QStringLiteral("branches without an upstream");
    case VisibilityPreset::Ahead: return QStringLiteral("branches ahead of upstream");
    case VisibilityPreset::Behind: return QStringLiteral("branches behind upstream");
    case VisibilityPreset::None: break;
    }
    return QStringLiteral("all branches");
}

} // namespace

void MainWindow::applyVisibility() {
    m_commitModel->setHistoryFilter(historyFilter());

    // The menu tick must agree with what the filter actually does, including after
    // a clear that turned it off. Silent: setChecked would otherwise re-enter.
    if (m_smartAction) {
        const bool on = m_visibility.smart();
        if (m_smartAction->isChecked() != on) {
            m_smartAction->blockSignals(true);
            m_smartAction->setChecked(on);
            m_smartAction->blockSignals(false);
        }
    }

    if (!m_visibilityButton) {
        return;
    }
    if (m_visibility.empty() || !m_session || !m_session->isOpen()) {
        m_visibilityButton->setVisible(false);
        return;
    }
    QStringList parts;
    if (m_visibility.smart()) {
        const QString target = m_session->targetBranch();
        const QString remotePrefix = QStringLiteral("refs/remotes/");
        QString shown;
        if (target.isEmpty()) {
            shown = QStringLiteral("smart visibility");
        } else if (target.startsWith(remotePrefix)) {
            shown = QStringLiteral("smart: %1").arg(target.mid(remotePrefix.size()));
        } else {
            const int cut = target.lastIndexOf(QLatin1Char('/'));
            shown = QStringLiteral("smart: %1")
                        .arg(cut >= 0 ? target.mid(cut + 1) : target);
        }
        parts << shown;
    } else if (m_visibility.preset() != VisibilityPreset::None) {
        parts << QStringLiteral("preset: %1").arg(presetLabel(m_visibility.preset()));
    }
    if (m_visibility.soloActive()) {
        parts << QStringLiteral("solo: %1")
                     .arg(m_visibility.soloNames().join(QStringLiteral(", ")));
    }
    if (m_visibility.anyHidden()) {
        parts << QStringLiteral("%1 hidden").arg(m_visibility.hiddenNames().size());
    }
    m_visibilityButton->setText(parts.join(QStringLiteral(" · ")));
    m_visibilityButton->setToolTip(QStringLiteral("Filtered graph — click to show all branches"));
    m_visibilityButton->setVisible(true);
}

void MainWindow::clearVisibility() {
    if (m_visibility.empty()) {
        return;
    }
    m_visibility.clear();
    // refreshPanels hands the new (unfiltered) filter to the model, which resets
    // the walk on its own — resetting here would only reset it twice.
    refreshPanels();
    setStatus(QStringLiteral("Showing all branches."));
}

QSet<QString> MainWindow::effectiveHiddenBranches() const {
    QSet<QString> hidden;
    const bool filterMode =
        m_visibility.smart() || m_visibility.preset() != VisibilityPreset::None;
    if (filterMode) {
        const QStringList names = graphHiddenBranchNames();
        return QSet<QString>(names.begin(), names.end());
    }
    if (!m_visibility.soloActive()) {
        const QStringList& names = m_visibility.hiddenNames();
        return QSet<QString>(names.begin(), names.end());
    }
    for (const RefInfo& ref : m_session->refs()) {
        if (ref.kind != RefKind::LocalBranch && ref.kind != RefKind::RemoteBranch) {
            continue;
        }
        const QString name = QString::fromStdString(ref.shortName);
        if (!m_visibility.isSolo(name)) {
            hidden.insert(name);
        }
    }
    return hidden;
}

void MainWindow::toggleEye(const QModelIndex& index) {
    const QStringList names = RefModel::eyeBranchNames(index);
    if (names.isEmpty() || !m_session || !m_session->isOpen()) {
        return;
    }
    QSet<QString> hidden = effectiveHiddenBranches();
    const bool show = std::all_of(names.begin(), names.end(),
                                  [&hidden](const QString& name) { return hidden.contains(name); });
    for (const QString& name : names) {
        if (show) {
            hidden.remove(name);
        } else {
            hidden.insert(name);
        }
    }
    QStringList stored(hidden.begin(), hidden.end());
    stored.sort();
    m_visibility.replaceHidden(stored);
    refreshPanels();
    const QString what = names.size() == 1 ? names.front()
                                           : QStringLiteral("%1 branches").arg(names.size());
    setStatus(show ? QStringLiteral("Showing %1").arg(what) : QStringLiteral("Hid %1").arg(what));
}

void MainWindow::toggleHidden(const QModelIndex& index) {
    const QString name = RefModel::refShortName(index);
    if (name.isEmpty()) {
        return;
    }
    const bool wasHidden = m_visibility.isHidden(name);
    m_visibility.setHidden(name, !wasHidden);
    refreshPanels();
    setStatus(wasHidden ? QStringLiteral("Showing %1").arg(name)
                        : QStringLiteral("Hid %1").arg(name));
}

void MainWindow::toggleSolo(const QModelIndex& index) {
    const QString name = RefModel::refShortName(index);
    if (name.isEmpty()) {
        return;
    }
    const bool wasSolo = m_visibility.isSolo(name);
    m_visibility.setSolo(name, !wasSolo);
    refreshPanels();
    setStatus(wasSolo ? QStringLiteral("Stopped soloing %1").arg(name)
                      : QStringLiteral("Soloing %1").arg(name));
}

void MainWindow::toggleSmart() {
    const bool on = !m_visibility.smart();
    m_visibility.setSmart(on);
    refreshPanels();
    setStatus(on ? QStringLiteral("Focus on current branch on — the current branch, its "
                                  "target and their upstreams.")
                 : QStringLiteral("Focus on current branch off."));
}

void MainWindow::applyPreset(VisibilityPreset preset) {
    m_visibility.setSmart(false);
    m_visibility.setPreset(preset);
    refreshPanels();
    setStatus(QStringLiteral("Showing %1.").arg(presetLabel(preset)));
}

void MainWindow::applyPrefixPreset() {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    bool ok = false;
    const QString initial = m_visibility.presetPrefix().isEmpty()
                                ? QStringLiteral("feature/")
                                : m_visibility.presetPrefix();
    const QString prefix = QInputDialog::getText(this, QStringLiteral("Branches by prefix"),
                                                 QStringLiteral("Branch name begins with:"),
                                                 QLineEdit::Normal, initial, &ok);
    if (!ok || prefix.trimmed().isEmpty()) {
        return;
    }
    m_visibility.setPresetPrefix(prefix.trimmed());
    m_visibility.setSmart(false);
    m_visibility.setPreset(VisibilityPreset::Prefix);
    refreshPanels();
    setStatus(QStringLiteral("Showing branches starting with %1.").arg(prefix.trimmed()));
}

void MainWindow::syncRefTreeExpansion() {
    if (!m_refView || !m_refModel) {
        return;
    }
    if (m_refModel->filterActive()) {
        // A match hidden behind a collapsed folder is a match the user asked for
        // and cannot see.
        m_refView->expandAll();
        return;
    }

    // Sections open, folders closed. Opening every folder would turn the panel
    // back into the flat list the folders exist to break up.
    m_refView->collapseAll();
    for (int row = 0; row < m_refModel->rowCount(); ++row) {
        const QModelIndex section = m_refModel->index(row, 0);
        m_refView->expand(section);
        if (section.data(Qt::DisplayRole).toString() == QLatin1String("Pinned")) {
            m_refView->expandRecursively(section);
        }
    }
}

void MainWindow::onRefContextMenu(const QPoint& pos) {
    const QModelIndex index = m_refView->indexAt(pos);
    if (!index.isValid()) {
        return;
    }
    // `setCurrentIndex` works out the selection command of a click that never
    // happened, and for an extended selection that command is ClearAndSelect — the
    // group would vanish before the menu was built. Move the current row without
    // touching the selection when the right-click landed inside it; select it alone
    // only when it came from outside the group.
    if (m_refView->selectionModel() && m_refView->selectionModel()->isSelected(index)) {
        m_refView->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
    } else {
        m_refView->setCurrentIndex(index);
    }

    if (RefModel::isStash(index)) {
        const QString oid = index.data(RefModel::StashOidRole).toString();
        const QString label = index.data(Qt::DisplayRole).toString();
        QMenu stashMenu(this);
        QAction* apply = stashMenu.addAction(QStringLiteral("Apply"));
        QAction* pop = stashMenu.addAction(QStringLiteral("Pop"));
        stashMenu.addSeparator();
        QAction* branch = stashMenu.addAction(QStringLiteral("Branch from stash…"));
        QAction* drop = stashMenu.addAction(QStringLiteral("Drop"));
        QAction* copy = stashMenu.addAction(QStringLiteral("Copy SHA"));
        QAction* chosen = stashMenu.exec(m_refView->viewport()->mapToGlobal(pos));
        if (!chosen) {
            return;
        }
        if (chosen == apply) {
            restoreStash(oid, false, label);
        } else if (chosen == pop) {
            restoreStash(oid, true, label);
        } else if (chosen == branch) {
            branchFromStash(oid, label);
        } else if (chosen == drop) {
            dropStash(oid, label);
        } else if (chosen == copy) {
            copyTextToClipboard(oid, QStringLiteral("stash id"));
        }
        return;
    }

    if (RefModel::isWorktree(index)) {
        const QString wtPath = index.data(RefModel::WorktreePathRole).toString();
        const bool isCurrent = index.data(RefModel::WorktreeCurrentRole).toBool();
        const bool isLocked = index.data(RefModel::WorktreeLockedRole).toBool();
        QMenu worktreeMenu(this);
        QAction* open = worktreeMenu.addAction(QStringLiteral("Open in this window"));
        open->setEnabled(!isCurrent);
        QAction* lock = worktreeMenu.addAction(isLocked ? QStringLiteral("Unlock")
                                                        : QStringLiteral("Lock…"));
        worktreeMenu.addSeparator();
        QAction* remove = worktreeMenu.addAction(QStringLiteral("Remove"));
        remove->setEnabled(!isCurrent && !isLocked);
        QAction* copy = worktreeMenu.addAction(QStringLiteral("Copy path"));
        QAction* chosen = worktreeMenu.exec(m_refView->viewport()->mapToGlobal(pos));
        if (!chosen) {
            return;
        }
        if (chosen == open) {
            openWorktree(wtPath);
        } else if (chosen == lock) {
            if (isLocked) {
                unlockWorktreeAt(index);
            } else {
                lockWorktreeAt(index);
            }
        } else if (chosen == remove) {
            removeWorktreeAt(index);
        } else if (chosen == copy) {
            copyTextToClipboard(wtPath, QStringLiteral("worktree path"));
        }
        return;
    }

    if (RefModel::isSubmodule(index)) {
        const QString subPath = index.data(RefModel::SubmodulePathRole).toString();
        const QString subUrl = index.data(RefModel::SubmoduleUrlRole).toString();
        const auto subState =
            static_cast<SubmoduleState>(index.data(RefModel::SubmoduleStateRole).toInt());
        const bool subInitialized = subState != SubmoduleState::NotInitialized;

        QMenu submoduleMenu(this);
        QAction* open = submoduleMenu.addAction(QStringLiteral("Open"));
        open->setEnabled(subInitialized);
        QAction* initialize = nullptr;
        QAction* update = nullptr;
        QAction* updateRemote = nullptr;
        if (subInitialized) {
            update = submoduleMenu.addAction(QStringLiteral("Update"));
            if (subState == SubmoduleState::InSync || subState == SubmoduleState::OutOfSync) {
                updateRemote = submoduleMenu.addAction(QStringLiteral("Update to remote"));
            }
        } else {
            initialize = submoduleMenu.addAction(QStringLiteral("Initialize"));
        }
        QAction* sync = submoduleMenu.addAction(QStringLiteral("Sync URL"));
        submoduleMenu.addSeparator();
        QAction* deinit = submoduleMenu.addAction(QStringLiteral("Deinitialize…"));
        deinit->setEnabled(subInitialized);
        QAction* copyPath = submoduleMenu.addAction(QStringLiteral("Copy path"));
        QAction* copyUrl = submoduleMenu.addAction(QStringLiteral("Copy URL"));

        QAction* chosen = submoduleMenu.exec(m_refView->viewport()->mapToGlobal(pos));
        if (!chosen) {
            return;
        }
        if (chosen == open) {
            openSubmodule(subPath);
        } else if (chosen == initialize) {
            initSubmoduleAt(index);
        } else if (chosen == update) {
            updateSubmoduleAt(index, false);
        } else if (chosen == updateRemote) {
            updateSubmoduleAt(index, true);
        } else if (chosen == sync) {
            syncSubmoduleAt(index);
        } else if (chosen == deinit) {
            deinitSubmoduleAt(index);
        } else if (chosen == copyPath) {
            copyTextToClipboard(subPath, QStringLiteral("submodule path"));
        } else if (chosen == copyUrl) {
            copyTextToClipboard(subUrl, QStringLiteral("submodule URL"));
        }
        return;
    }

    // A group of branches selected in the rail gets its own menu. The single-branch
    // menu below reads `selectedRefShortName()`, i.e. one branch, and showing it for
    // a group would silently ignore everything but the row under the cursor.
    const QStringList selectedBranches = selectedLocalBranches();
    if (selectedBranches.size() > 1) {
        bool allHidden = true;
        bool anySolo = false;
        for (const QString& name : selectedBranches) {
            allHidden = allHidden && m_visibility.isHidden(name);
            anySolo = anySolo || m_visibility.isSolo(name);
        }

        QMenu groupMenu(this);
        QAction* deleteGroup = groupMenu.addAction(
            QStringLiteral("Delete %1 branches…").arg(selectedBranches.size()));
        QAction* upstreamGroup = groupMenu.addAction(
            QStringLiteral("Set upstream for %1…").arg(selectedBranches.size()));
        groupMenu.addSeparator();
        QAction* hideGroup = groupMenu.addAction(allHidden ? QStringLiteral("Show selected")
                                                           : QStringLiteral("Hide selected"));
        QAction* soloGroup = groupMenu.addAction(anySolo ? QStringLiteral("Stop soloing selected")
                                                         : QStringLiteral("Solo selected"));
        groupMenu.addSeparator();
        QAction* copyGroup = groupMenu.addAction(
            QStringLiteral("Copy %1 branch names").arg(selectedBranches.size()));

        QAction* groupChosen = groupMenu.exec(m_refView->viewport()->mapToGlobal(pos));
        if (!groupChosen) {
            return;
        }
        if (groupChosen == deleteGroup) {
            deleteSelectedBranches();
        } else if (groupChosen == upstreamGroup) {
            setUpstreamsForSelected();
        } else if (groupChosen == hideGroup) {
            hideSelectedBranches(!allHidden);
        } else if (groupChosen == soloGroup) {
            soloSelectedBranches(!anySolo);
        } else if (groupChosen == copyGroup) {
            copyTextToClipboard(selectedBranches.join(QLatin1Char('\n')),
                                QStringLiteral("branch names"));
        }
        return;
    }

    QMenu menu(this);
    const QString pinKey = RefModel::pinKey(index);
    QAction* pin = nullptr;
    if (!pinKey.isEmpty()) {
        const bool pinned = m_pins.isPinned(pinKey);
        pin = menu.addAction(pinned ? QStringLiteral("Unpin from left")
                                    : QStringLiteral("Pin to left"));
        pin->setIcon(glyphIcon(Glyph::Paperclip, theme::accent(), 14));
        menu.addSeparator();
    }

    if (RefModel::refKind(index) == RefKind::Tag) {
        const QString tag = RefModel::refShortName(index);
        QAction* pushOne = menu.addAction(QStringLiteral("Push tag"));
        QAction* pushAll = menu.addAction(QStringLiteral("Push all tags"));
        menu.addSeparator();
        QAction* createTagBranch = menu.addAction(QStringLiteral("New branch here…"));
        QAction* mergeTag = menu.addAction(QStringLiteral("Merge into current"));
        menu.addSeparator();
        QAction* removeTag = menu.addAction(QStringLiteral("Delete tag"));
        QAction* removeRemoteTag = menu.addAction(QStringLiteral("Delete tag from all remotes…"));
        QAction* copyTag = menu.addAction(QStringLiteral("Copy tag name"));

        QAction* tagChosen = menu.exec(m_refView->viewport()->mapToGlobal(pos));
        if (!tagChosen) {
            return;
        }
        if (tagChosen == pin) {
            togglePin(index);
        } else if (tagChosen == pushOne) {
            pushTagToRemote(tag);
        } else if (tagChosen == pushAll) {
            pushAllTags();
        } else if (tagChosen == createTagBranch) {
            createBranchFromSelection();
        } else if (tagChosen == mergeTag) {
            mergeBranchIntoCurrent();
        } else if (tagChosen == removeTag) {
            deleteTagInteractive(tag);
        } else if (tagChosen == removeRemoteTag) {
            deleteTagFromRemotesInteractive(tag);
        } else if (tagChosen == copyTag) {
            copyTextToClipboard(tag, QStringLiteral("tag name"));
        }
        return;
    }

    QAction* checkout = menu.addAction(QStringLiteral("Checkout"));
    checkout->setEnabled(selectedRefIsLocalBranch());
    QAction* create = menu.addAction(QStringLiteral("New branch here…"));
    QAction* remove = menu.addAction(QStringLiteral("Delete branch"));
    remove->setEnabled(selectedRefIsLocalBranch());
    menu.addSeparator();
    QAction* merge = menu.addAction(QStringLiteral("Merge into current"));

    const QString refName = RefModel::refShortName(index);
    const bool isBranch = RefModel::isRef(index) && !refName.isEmpty();
    QAction* hide = nullptr;
    QAction* solo = nullptr;
    QAction* showAll = nullptr;
    QAction* smart = nullptr;
    if (isBranch) {
        menu.addSeparator();
        hide = menu.addAction(m_visibility.isHidden(refName) ? QStringLiteral("Show branch")
                                                             : QStringLiteral("Hide branch"));
        solo = menu.addAction(m_visibility.isSolo(refName)
                                  ? QStringLiteral("Stop soloing")
                                  : QStringLiteral("Solo this branch"));
        smart = menu.addAction(QStringLiteral("Focus on Current Branch"));
        smart->setCheckable(true);
        smart->setChecked(m_visibility.smart());
        if (!m_visibility.empty()) {
            showAll = menu.addAction(QStringLiteral("Show all branches"));
        }
    }

    QAction* chosen = menu.exec(m_refView->viewport()->mapToGlobal(pos));
    if (!chosen) {
        return;
    }
    if (chosen == pin) {
        togglePin(index);
    } else if (chosen == checkout) {
        checkoutSelectedBranch();
    } else if (chosen == create) {
        createBranchFromSelection();
    } else if (chosen == remove) {
        deleteSelectedBranch();
    } else if (chosen == merge) {
        mergeBranchIntoCurrent();
    } else if (chosen == hide) {
        toggleHidden(index);
    } else if (chosen == solo) {
        toggleSolo(index);
    } else if (chosen == smart) {
        toggleSmart();
    } else if (chosen == showAll) {
        clearVisibility();
    }
}

void MainWindow::onCommitContextMenu(const QString& commitId, const QPoint& globalPos) {
    if (commitId.startsWith(QStringLiteral("//wip"))) {
        const int row = m_commitModel->rowForCommit(commitId);
        const QModelIndex wipIndex = row >= 0 ? m_commitModel->index(row, 0) : QModelIndex();
        const QString worktreePath = wipIndex.data(CommitModel::WipWorktreeRole).toString();
        const QString worktreeName = wipIndex.data(CommitModel::WipNameRole).toString();
        const bool isCurrent = row >= 0 && row == m_commitModel->wipRow();

        QMenu wipMenu(this);
        QAction* open = wipMenu.addAction(
            icon(Icon::Checkout),
            worktreeName.isEmpty() ? QStringLiteral("Open Worktree")
                                   : QStringLiteral("Open “%1”").arg(worktreeName));
        open->setEnabled(!isCurrent && !worktreePath.isEmpty());
        QAction* stageAll =
            wipMenu.addAction(icon(Icon::Plus), QStringLiteral("Stage All Changes"));
        // Staging runs in the window's own checkout; offering it for another
        // worktree's row would stage the wrong tree.
        stageAll->setEnabled(isCurrent);
        if (m_headless) {
            m_lastMenuReport.clear();
            for (const QAction* action : wipMenu.actions()) {
                if (!action->isSeparator()) {
                    m_lastMenuReport << QStringLiteral("%1%2")
                                            .arg(action->isEnabled() ? QString()
                                                                     : QStringLiteral("(off) "))
                                            .arg(action->text());
                }
            }
            return;
        }
        QAction* chosen = wipMenu.exec(globalPos);
        if (chosen == stageAll) {
            QString error;
            if (!m_session->stageAll(&error)) {
                reportFailure(QStringLiteral("Cannot stage the changes"), error);
                return;
            }
            refreshPanels();
        } else if (chosen == open) {
            openWorktree(worktreePath);
        }
        return;
    }

    QMenu menu(this);
    buildCommitMenu(menu, commitId);
    // A headless run has nobody to click, and exec() would wait for a person who is not
    // there. The menu is still built — the entries and which of them are on offer are the
    // things worth checking — and then written down instead of shown.
    if (m_headless) {
        m_lastMenuReport.clear();
        for (const QAction* action : menu.actions()) {
            if (action->isSeparator()) {
                continue;
            }
            m_lastMenuReport << QStringLiteral("%1%2")
                                    .arg(action->isEnabled() ? QString() : QStringLiteral("(off) "))
                                    .arg(action->text());
        }
        return;
    }
    if (menu.isEmpty()) {
        return;
    }

    QAction* chosen = menu.exec(globalPos);
    if (!chosen) {
        return;
    }
    // Every entry carries its own identifier, so the dispatcher cannot act on
    // the wrong one when entries are added or reordered.
    dispatchCommitMenuAction(chosen->data().toString(), commitId);
}

void MainWindow::buildCommitMenu(QMenu& menu, const QString& commitId) const {
    if (!m_session->isOpen()) {
        return;
    }
    const int row = m_commitModel->rowForCommit(commitId);
    if (row < 0) {
        return;
    }
    const CommitInfo* commit = m_commitModel->commitAt(row);
    if (!commit) {
        return;
    }

    const std::vector<RefInfo> labels = m_commitModel->refsAt(row);
    // A stash is not a commit to stand on or build from: it is put back or thrown
    // away. Checkout, reset, cherry-pick, branch and the rest are not offered.
    if (isStashCommit(commitId)) {
        menu.addAction(QStringLiteral("Apply Stash"))->setData(QStringLiteral("stash-apply"));
        menu.addAction(QStringLiteral("Pop Stash"))->setData(QStringLiteral("stash-pop"));
        menu.addAction(QStringLiteral("Delete Stash"))->setData(QStringLiteral("stash-drop"));
        menu.addSeparator();
        menu.addAction(QStringLiteral("Copy commit SHA"))->setData(QStringLiteral("copy-sha"));
        menu.addAction(QStringLiteral("Copy commit message"))
            ->setData(QStringLiteral("copy-message"));
        return;
    }
    const RepoInfo repoInfo = m_session->info();
    const QString current = QString::fromStdString(repoInfo.headBranch);
    const bool onBranch = !current.isEmpty() && !repoInfo.detached;

    // A local branch sitting on this commit is what makes the branch-level
    // entries meaningful: "merge X into Y" only exists if X is here.
    QString labelBranch;
    for (const RefInfo& ref : labels) {
        if (ref.kind == RefKind::LocalBranch) {
            labelBranch = QString::fromStdString(ref.shortName);
            break;
        }
    }

    // `add` targets the top-level menu, `addTo` an explicit submenu. Mixing
    // them up silently lands entries one level up, where they read as
    // unrelated commands.
    const auto addTo = [](QMenu* target, const QString& text, const QString& key) {
        QAction* action = target->addAction(text);
        action->setData(key);
        return action;
    };
    const auto add = [&menu, &addTo](const QString& text, const QString& key) {
        return addTo(&menu, text, key);
    };

    QAction* pull = add(QStringLiteral("Pull (fast-forward if possible)"),
                        QStringLiteral("pull"));
    pull->setEnabled(!m_remotes.empty() && onBranch);
    QAction* push = add(QStringLiteral("Push"), QStringLiteral("push"));
    push->setEnabled(!m_remotes.empty() && onBranch);
    QAction* upstream = add(QStringLiteral("Set Upstream"), QStringLiteral("set-upstream"));
    upstream->setEnabled(!m_remotes.empty() && onBranch);
    upstream->setToolTip(QStringLiteral("Choose the remote-tracking branch this branch follows"));
    menu.addSeparator();

    if (!labelBranch.isEmpty() && onBranch && labelBranch != current) {
        QAction* fastForward =
            add(QStringLiteral("Fast-forward %1 to %2").arg(current, labelBranch),
                QStringLiteral("fast-forward"));
        // Shown but greyed out when it would lose commits, so it is clear why
        // the entry is unavailable instead of it silently missing.
        fastForward->setEnabled(m_session->canFastForward(current, commitId));
        add(QStringLiteral("Merge %1 into %2").arg(labelBranch, current), QStringLiteral("merge"));
        add(QStringLiteral("Rebase %1 onto %2").arg(current, labelBranch),
            QStringLiteral("rebase"));
        menu.addSeparator();
    }

    QMenu* checkout = menu.addMenu(QStringLiteral("Checkout"));
    addTo(checkout, QStringLiteral("Detached HEAD at %1").arg(commitId.left(7)),
          QStringLiteral("checkout-detached"));
    bool firstBranch = true;
    for (const RefInfo& ref : labels) {
        if (ref.kind != RefKind::LocalBranch) {
            continue;
        }
        if (firstBranch) {
            checkout->addSeparator();
            firstBranch = false;
        }
        const QString name = QString::fromStdString(ref.shortName);
        addTo(checkout, name, QStringLiteral("checkout:") + name);
    }
    menu.addSeparator();

    add(QStringLiteral("Create branch here…"), QStringLiteral("create-branch"));

    // With several commits selected these entries act on all of them, so they say so, and
    // the ones the selection does not allow are greyed out with the reason in the label —
    // a reader should not have to click to be told why nothing happened.
    const int selectedCount = m_commitView->selectedRows().size();
    const SelectionPlan multi = selectedCount > 1 ? selectionPlan() : SelectionPlan();
    const bool many = selectedCount > 1;

    QAction* cherry = add(many ? QStringLiteral("Cherry-pick %1 commits").arg(selectedCount)
                               : QStringLiteral("Cherry pick commit"),
                          QStringLiteral("cherry-pick"));
    if (many) {
        cherry->setEnabled(multi.canCherryPick);
        if (!multi.canCherryPick) {
            cherry->setText(QStringLiteral("Cherry-pick %1 commits — %2")
                                .arg(selectedCount)
                                .arg(multi.refusal.empty()
                                         ? QStringLiteral("only commits from elsewhere")
                                         : QString::fromStdString(multi.refusal)));
        }
    }

    QMenu* reset = menu.addMenu(QStringLiteral("Reset %1 to this commit")
                                    .arg(onBranch ? current : QStringLiteral("HEAD")));
    reset->setEnabled(onBranch);
    addTo(reset, QStringLiteral("Soft — keep index and working tree"),
          QStringLiteral("reset-soft"));
    addTo(reset, QStringLiteral("Mixed — keep working tree"), QStringLiteral("reset-mixed"));
    addTo(reset, QStringLiteral("Hard — discard local changes"), QStringLiteral("reset-hard"));

    add(QStringLiteral("Revert commit"), QStringLiteral("revert"));
    QAction* dropAction =
        add(many ? QStringLiteral("Drop %1 commits from history").arg(selectedCount)
                 : QStringLiteral("Drop commit from history"),
            QStringLiteral("drop"));
    if (many) {
        dropAction->setEnabled(multi.canDrop);
        if (!multi.canDrop) {
            dropAction->setText(QStringLiteral("Drop %1 commits — %2")
                                    .arg(selectedCount)
                                    .arg(multi.refusal.empty()
                                             ? QStringLiteral("only commits of this branch")
                                             : QString::fromStdString(multi.refusal)));
        }
    }
    QAction* squashAction =
        add(many ? QStringLiteral("Squash %1 commits into the one below").arg(selectedCount)
                 : QStringLiteral("Squash into the commit below"),
            QStringLiteral("squash"));
    if (many) {
        if (multi.canSquash) {
            squashAction->setEnabled(true);
        } else {
            squashAction->setEnabled(false);
            squashAction->setText(QStringLiteral("Squash %1 commits — %2")
                                      .arg(selectedCount)
                                      .arg(multi.squashRefusal.empty()
                                               ? QStringLiteral("not possible here")
                                               : QString::fromStdString(multi.squashRefusal)));
        }
    }
    add(QStringLiteral("Fixup into the commit below"), QStringLiteral("fixup"));
    add(QStringLiteral("Reword commit…"), QStringLiteral("reword"));

    // Interactive rebase: from this commit upwards. The editor lists the range
    // `parent(this)..HEAD`, so a root commit has no range and the entry says why.
    if (many && !multi.branchCommits.empty()) {
        add(QStringLiteral("Interactively Rebase %1 Selected Commits…").arg(selectedCount),
            QStringLiteral("interactive-rebase-selected"));
    } else if (!many) {
        QAction* interactive = add(QStringLiteral("Interactive Rebase from Here…"),
                                   QStringLiteral("interactive-rebase"));
        const bool hasParent = !m_session->resolveRevision(commitId + QLatin1Char('^')).isEmpty();
        interactive->setEnabled(hasParent);
        if (!hasParent) {
            interactive->setText(
                QStringLiteral("Interactive Rebase from Here — the root commit has no range"));
        }
    }
    // Exactly two selected commits can be compared: the graph orders them newest
    // first, so the one lower on screen is the base and the one above it the target.
    if (many && selectedCount == 2) {
        const std::vector<Oid> ids = m_commitView->selectedCommitIds();
        if (ids.size() == 2) {
            add(QStringLiteral("Compare %1 with %2")
                    .arg(QString::fromStdString(ids[1]).left(7),
                         QString::fromStdString(ids[0]).left(7)),
                QStringLiteral("compare"));
            add(QStringLiteral("Compare %1...%2 (merge base)")
                    .arg(QString::fromStdString(ids[1]).left(7),
                         QString::fromStdString(ids[0]).left(7)),
                QStringLiteral("compare-merge-base"));
        }
    }
    add(QStringLiteral("Compare with Working Tree"), QStringLiteral("compare-working-tree"));
    menu.addSeparator();

    if (!labelBranch.isEmpty()) {
        add(QStringLiteral("Rename %1…").arg(labelBranch), QStringLiteral("rename"));
        QAction* remove = add(QStringLiteral("Delete %1").arg(labelBranch),
                              QStringLiteral("delete"));
        remove->setEnabled(labelBranch != current);
        menu.addSeparator();
    }

    add(QStringLiteral("Go to parent commit"), QStringLiteral("goto-parent"));
    add(QStringLiteral("Go to first child commit"), QStringLiteral("goto-child"));
    add(QStringLiteral("Copy commit SHA"), QStringLiteral("copy-sha"));
    add(QStringLiteral("Copy commit message"), QStringLiteral("copy-message"));
    add(QStringLiteral("Verify Signature"), QStringLiteral("verify-signature"));
    if (!labelBranch.isEmpty()) {
        add(QStringLiteral("Copy branch name"), QStringLiteral("copy-branch"));
    }
    add(QStringLiteral("Create patch from commit…"), QStringLiteral("patch"));
    menu.addSeparator();

    add(QStringLiteral("Create tag here…"), QStringLiteral("tag"));
}

bool MainWindow::isStashCommit(const QString& commitId) const {
    return std::any_of(m_stashes.begin(), m_stashes.end(), [&commitId](const StashEntry& entry) {
        return QString::fromStdString(entry.oid) == commitId;
    });
}

void MainWindow::dispatchCommitMenuAction(const QString& key, const QString& commitId) {
    if (key.startsWith(QLatin1String("stash-"))) {
        QString label = commitId.left(8);
        for (std::size_t i = 0; i < m_stashes.size(); ++i) {
            if (QString::fromStdString(m_stashes[i].oid) == commitId) {
                label = QStringLiteral("stash@{%1}").arg(i);
            }
        }
        if (key == QLatin1String("stash-drop")) {
            dropStash(commitId, label);
        } else {
            restoreStash(commitId, key == QLatin1String("stash-pop"), label);
        }
        return;
    }
    const int row = m_commitModel->rowForCommit(commitId);
    const CommitInfo* commit = row < 0 ? nullptr : m_commitModel->commitAt(row);
    const std::vector<RefInfo> labels = row < 0 ? std::vector<RefInfo>{} : m_commitModel->refsAt(row);
    const QString current = QString::fromStdString(m_session->info().headBranch);
    QString labelBranch;
    for (const RefInfo& ref : labels) {
        if (ref.kind == RefKind::LocalBranch) {
            labelBranch = QString::fromStdString(ref.shortName);
            break;
        }
    }

    if (key == QLatin1String("pull")) {
        startSync(SyncKind::Pull);
    } else if (key == QLatin1String("push")) {
        startSync(SyncKind::Push);
    } else if (key == QLatin1String("set-upstream")) {
        setUpstreamInteractive();
    } else if (key == QLatin1String("fast-forward")) {
        runWrite(QStringLiteral("Fast-forward failed"),
                 [&](QString* error) {
                     return m_session->fastForwardBranch(current, commitId, error);
                 },
                 true);
        setStatus(QStringLiteral("Fast-forwarded %1 to %2.").arg(current, commitId.left(7)));
    } else if (key == QLatin1String("merge")) {
        mergeBranchInto(labelBranch, current);
    } else if (key == QLatin1String("rebase")) {
        rebaseBranchOnto(current, labelBranch);
    } else if (key == QLatin1String("checkout-detached")) {
        runCheckout(QStringLiteral("Cannot check out commit"), commitId.left(7),
                    [commitId](RepoSession& s, QString* error) {
                        return s.checkoutCommit(commitId, error);
                    },
                    [this, commitId] {
                        setStatus(QStringLiteral("Detached HEAD at %1.").arg(commitId.left(7)));
                        maybeAutoUpdateSubmodules();
                    });
    } else if (key.startsWith(QLatin1String("checkout:"))) {
        const QString target = key.mid(QStringLiteral("checkout:").size());
        checkoutLocalBranch(target);
    } else if (key == QLatin1String("create-branch")) {
        createBranchAtCommit(commitId);
    } else if (key == QLatin1String("cherry-pick") && m_commitView->selectedRows().size() > 1) {
        runSelectionOperation(SelectionOperation::CherryPick);
    } else if (key == QLatin1String("cherry-pick")) {
        cherryPickCommit(commitId);
    } else if (key == QLatin1String("reset-soft")) {
        resetCurrentBranchTo(commitId, ResetMode::Soft);
    } else if (key == QLatin1String("reset-mixed")) {
        resetCurrentBranchTo(commitId, ResetMode::Mixed);
    } else if (key == QLatin1String("reset-hard")) {
        resetCurrentBranchTo(commitId, ResetMode::Hard);
    } else if (key == QLatin1String("revert")) {
        revertCommit(commitId);
    } else if (key == QLatin1String("drop") && m_commitView->selectedRows().size() > 1) {
        runSelectionOperation(SelectionOperation::Drop);
    } else if (key == QLatin1String("drop")) {
        dropCommitFromHistory(commitId);
    } else if (key == QLatin1String("squash") && m_commitView->selectedRows().size() > 1) {
        runSelectionOperation(SelectionOperation::Squash);
    } else if (key == QLatin1String("squash")) {
        rewriteCommitInHistory(commitId, RewriteAction::Squash);
    } else if (key == QLatin1String("fixup")) {
        rewriteCommitInHistory(commitId, RewriteAction::Fixup);
    } else if (key == QLatin1String("reword")) {
        rewriteCommitInHistory(commitId, RewriteAction::Reword);
    } else if (key == QLatin1String("interactive-rebase")) {
        interactiveRebaseFromCommit(commitId);
    } else if (key == QLatin1String("interactive-rebase-selected")) {
        const SelectionPlan plan = selectionPlan();
        interactiveRebaseRange(rootBelow(plan.branchCommits));
    } else if (key == QLatin1String("compare")) {
        const std::vector<Oid> ids = m_commitView->selectedCommitIds();
        if (ids.size() == 2) {
            showComparison(QString::fromStdString(ids[1]), QString::fromStdString(ids[0]));
        }
    } else if (key == QLatin1String("compare-merge-base")) {
        const std::vector<Oid> ids = m_commitView->selectedCommitIds();
        if (ids.size() == 2) {
            showComparisonMergeBase(QString::fromStdString(ids[1]),
                                    QString::fromStdString(ids[0]));
        }
    } else if (key == QLatin1String("compare-working-tree")) {
        showComparisonWorkingTree(commitId);
    } else if (key == QLatin1String("rename")) {
        renameBranchInteractive(labelBranch);
    } else if (key == QLatin1String("delete")) {
        removeBranchByName(labelBranch);
    } else if (key == QLatin1String("goto-parent")) {
        m_commitView->navigate(QStringLiteral("parent"));
    } else if (key == QLatin1String("goto-child")) {
        m_commitView->navigate(QStringLiteral("child"));
    } else if (key == QLatin1String("copy-sha")) {
        copyTextToClipboard(commitId, QStringLiteral("Commit SHA"));
    } else if (key == QLatin1String("copy-message")) {
        copyTextToClipboard(commit ? QString::fromStdString(commit->summary) : QString(),
                            QStringLiteral("Commit message"));
    } else if (key == QLatin1String("verify-signature")) {
        verifyCommitSignature(commitId);
    } else if (key == QLatin1String("copy-branch")) {
        copyTextToClipboard(labelBranch, QStringLiteral("Branch name"));
    } else if (key == QLatin1String("patch")) {
        savePatchForCommit(commitId);
    } else if (key == QLatin1String("tag")) {
        createTagAtCommit(commitId);
    }
}

QStringList MainWindow::commitMenuEntriesForFirstLabelledCommit() const {
    const int rows = m_commitModel->rowCount();
    for (int row = 0; row < rows; ++row) {
        for (const RefInfo& ref : m_commitModel->refsAt(row)) {
            if (ref.kind == RefKind::LocalBranch) {
                const CommitInfo* commit = m_commitModel->commitAt(row);
                if (commit) {
                    return commitMenuEntries(QString::fromStdString(commit->id));
                }
            }
        }
    }
    return {};
}

QStringList MainWindow::commitMenuEntries(const QString& commitId) const {
    QMenu menu;
    buildCommitMenu(menu, commitId);

    QStringList entries;
    std::function<void(QMenu*, const QString&)> walk = [&](QMenu* current, const QString& prefix) {
        for (QAction* action : current->actions()) {
            if (action->isSeparator()) {
                entries << QStringLiteral("---");
                continue;
            }
            if (action->menu()) {
                entries << prefix + action->text() + QStringLiteral(" ▸");
                walk(action->menu(), prefix + QStringLiteral("  "));
                continue;
            }
            const QString key = action->data().toString();
            entries << QStringLiteral("%1|%2%3")
                           .arg(action->isEnabled() ? QStringLiteral("enabled")
                                                    : QStringLiteral("disabled"),
                                prefix, action->text())
                    << QStringLiteral("  key:%1").arg(key);
        }
    };
    walk(&menu, QString());
    return entries;
}

void MainWindow::createBranchAtCommit(const QString& commitId) {
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("New branch"),
        QStringLiteral("Name of the branch to create at %1:").arg(commitId.left(7)),
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted || name.trimmed().isEmpty()) {
        return;
    }
    runWrite(QStringLiteral("Cannot create branch"),
             [&](QString* error) {
                 return m_session->createBranch(name.trimmed(), commitId, error);
             });
    setStatus(QStringLiteral("Branch %1 created at %2.").arg(name.trimmed(), commitId.left(7)));
}

void MainWindow::createTagAtCommit(const QString& commitId) {
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("New tag"),
        QStringLiteral("Name of the tag to create at %1:").arg(commitId.left(7)),
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted || name.trimmed().isEmpty()) {
        return;
    }
    runWrite(QStringLiteral("Cannot create tag"),
             [&](QString* error) {
                 return m_session->createTag(name.trimmed(), commitId, QString(), error);
             });
    setStatus(QStringLiteral("Tag %1 created at %2.").arg(name.trimmed(), commitId.left(7)));
}

void MainWindow::renameBranchInteractive(const QString& from) {
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Rename branch"), QStringLiteral("New name for “%1”:").arg(from),
        QLineEdit::Normal, from, &accepted);
    if (!accepted || name.trimmed().isEmpty() || name.trimmed() == from) {
        return;
    }
    runWrite(QStringLiteral("Cannot rename branch"),
             [&](QString* error) {
                 return m_session->renameBranch(from, name.trimmed(), error);
             });
    setStatus(QStringLiteral("Branch %1 renamed to %2.").arg(from, name.trimmed()));
}

void MainWindow::removeBranchByName(const QString& name) {
    const QString current = QString::fromStdString(m_session->info().headBranch);
    if (name == current) {
        QMessageBox::warning(this, QStringLiteral("Delete branch"),
                             QStringLiteral("“%1” is checked out. Switch away first.").arg(name));
        return;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Delete branch"),
        QStringLiteral("Delete branch “%1”? Commits only reachable from it are lost.").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    // The question above was the consent. The engine refuses to drop commits that
    // nothing else reaches, so a yes here has to be able to mean yes — asking a
    // second time after the refusal would be asking the reader to repeat themselves.
    QString error;
    bool deleted = m_session->deleteBranch(name, &error);
    if (!deleted) {
        deleted = m_session->deleteBranchForced(name, &error);
    }
    if (!deleted) {
        QMessageBox::warning(this, QStringLiteral("Cannot delete branch"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Branch %1 deleted.").arg(name));
}

QString MainWindow::rootBelow(const std::vector<Oid>& commits) const {
    if (commits.empty()) {
        return QString();
    }
    return rebaseBaseBelow(QString::fromStdString(commits.front()));
}

QString MainWindow::rebaseBaseBelow(const QString& commitId) const {
    // Asked of the repository, not read off the loaded rows: the parent may be older than
    // anything loaded, and "not loaded" must not read as "does not exist". No parent means
    // the commit is the root, so the replay starts at the root itself.
    const QString parent = m_session->resolveRevision(commitId + QLatin1Char('^'));
    return parent.isEmpty() ? QString::fromLatin1(kRebaseRoot) : parent;
}

SelectionPlan MainWindow::selectionPlan() const {
    const std::vector<Oid> selection = m_commitView->selectedCommitIds();
    const std::vector<Oid> lineage = m_commitModel->headLineage();

    // Asked twice: the structure decides whether a squash is conceivable at all, and only
    // then is the repository asked whether the stretch above the selection is one line.
    // Asking the repository first would be a git call per selection change for nothing.
    SelectionPlan plan = planSelection(selection, lineage, true);
    if (!plan.canSquash) {
        return plan;
    }

    // The base may be the root itself: `git rebase --root` folds the commit above the root.
    const QString base = rootBelow(plan.branchCommits);
    QString error;
    if (!m_session->rangeIsLinear(base, QString::fromStdString(m_commitModel->headTipId()),
                                 &error)) {
        return planSelection(selection, lineage, false); // carries the reason about the merge
    }
    return plan;
}

void MainWindow::runSelectionOperation(SelectionOperation operation) {
    const SelectionPlan plan = selectionPlan();

    if (operation == SelectionOperation::CherryPick) {
        if (!plan.canCherryPick) {
            setStatus(plan.refusal.empty()
                          ? QStringLiteral("Nothing in this selection can be picked.")
                          : QString::fromStdString(plan.refusal));
            return;
        }
        QString error;
        QString created;
        std::vector<QString> picked;
        picked.reserve(plan.pickedCommits.size());
        for (const Oid& id : plan.pickedCommits) {
            picked.push_back(QString::fromStdString(id));
        }
        if (!m_session->cherryPick(picked, &created, &error)) {
            m_commitModel->reset();
            refreshPanels();
            if (!m_session->conflicts().empty()) {
                showConflictsPage();
                setStatus(QStringLiteral("Picking %1 commits stopped on conflicts — %2 file(s) need you.")
                              .arg(plan.pickedCommits.size())
                              .arg(m_session->conflicts().size()));
                return;
            }
            QMessageBox::warning(this, QStringLiteral("Cherry pick"), error);
            return;
        }
        m_commitModel->reset();
        refreshPanels();
        setStatus(QStringLiteral("Cherry-picked %1 commits, newest is %2.")
                      .arg(plan.pickedCommits.size())
                      .arg(created.left(7)));
        return;
    }

    if (operation == SelectionOperation::Drop && !plan.canDrop) {
        setStatus(QString::fromStdString(plan.refusal));
        return;
    }
    if (operation == SelectionOperation::Squash && !plan.canSquash) {
        setStatus(QString::fromStdString(plan.squashRefusal));
        return;
    }

    const QString root = rootBelow(plan.branchCommits);
    if (root.isEmpty()) {
        setStatus(QStringLiteral("Nothing below these commits to replay onto."));
        return;
    }

    QStringList summaries;
    for (const Oid& id : plan.branchCommits) {
        const int row = m_commitModel->rowForCommit(QString::fromStdString(id));
        const CommitInfo* commit = row >= 0 ? m_commitModel->commitAt(row) : nullptr;
        summaries << QStringLiteral("· %1 %2")
                         .arg(QString::fromStdString(id).left(7),
                              commit ? QString::fromStdString(commit->summary) : QString());
    }

    const QString current = QString::fromStdString(m_session->info().headBranch);
    if (operation == SelectionOperation::Drop) {
        if (!askYes(QStringLiteral("Drop commits"),
                    QStringLiteral("Remove %1 commit(s) from “%2”?\n\n%3\n\nEverything above "
                                   "them is replayed.")
                        .arg(plan.branchCommits.size())
                        .arg(current, summaries.join(QLatin1Char('\n'))))) {
            return;
        }
    } else {
        if (!askYes(QStringLiteral("Squash commits"),
                    QStringLiteral("Fold %1 commit(s) into the one below them on “%2”?\n\n%3")
                        .arg(plan.branchCommits.size())
                        .arg(current, summaries.join(QLatin1Char('\n'))))) {
            return;
        }
    }

    std::vector<RewriteStep> steps;
    for (std::size_t index = 0; index < plan.branchCommits.size(); ++index) {
        RewriteStep step;
        step.commit = plan.branchCommits[index];
        if (operation == SelectionOperation::Drop) {
            step.action = RewriteAction::Drop;
        } else {
            // The oldest stays and the rest fold into it: a squash folds into the line
            // above, and the line above the oldest selected commit is the next one down,
            // which the reader did not select.
            step.action = index == 0 ? RewriteAction::Keep : RewriteAction::Squash;
        }
        steps.push_back(step);
    }

    bool done = true;
    runWrite(QStringLiteral("Cannot rewrite the history"),
             [&](QString* error) {
                 done = m_session->rewriteHistory(root, steps, error);
                 return done;
             },
             true);
    if (!done) {
        return;
    }
    if (m_session->rebaseInProgress()) {
        sweepConflictsAfterOperation(
            QStringLiteral("%1 conflict(s) while rewriting %2 commit(s)")
                .arg(m_session->conflicts().size())
                .arg(steps.size()));
        return;
    }
    setStatus(operation == SelectionOperation::Drop
                  ? QStringLiteral("Dropped %1 commit(s) from “%2”.").arg(steps.size()).arg(current)
                  : QStringLiteral("Folded %1 commit(s) on “%2”.").arg(steps.size()).arg(current));
}

void MainWindow::whenCommitsAreLoaded(const std::function<void()>& action, int attempt) {
    if (m_commitModel->rowCount() > 0 || attempt >= 60) {
        action();
        return;
    }
    QTimer::singleShot(100, this, [this, action, attempt] { whenCommitsAreLoaded(action, attempt + 1); });
}

bool MainWindow::commitsAreLoaded() const {
    return m_commitModel && m_commitModel->rowCount() > 0;
}

QStringList MainWindow::contextMenuForDiagnostics(const QVector<int>& rows, int menuRow) {
    m_commitView->selectRowsForDiagnostics(rows);
    const QVector<int> before = m_commitView->selectedRows();
    m_lastMenuReport.clear();
    m_commitView->openContextMenuAt(menuRow);
    const QVector<int> after = m_commitView->selectedRows();

    QStringList report;
    report << QStringLiteral("selection before: %1").arg(rowsToString(before));
    report << QStringLiteral("selection after:  %1").arg(rowsToString(after));
    report << QStringLiteral("current row: %1").arg(m_commitView->currentIndex().row());
    QStringList onOffer;
    QStringList off;
    for (const QString& entry : m_lastMenuReport) {
        (entry.startsWith(QStringLiteral("(off) ")) ? off : onOffer) << entry;
    }
    report << QStringLiteral("menu offers %1 entries: %2")
                  .arg(onOffer.size())
                  .arg(onOffer.join(QStringLiteral(" | ")));
    if (!off.isEmpty()) {
        report << QStringLiteral("menu refuses: %1").arg(off.join(QStringLiteral(" | ")));
    }
    return report;
}

QString MainWindow::rowsToString(const QVector<int>& rows) const {
    QStringList parts;
    for (const int row : rows) {
        const CommitInfo* commit = m_commitModel->commitAt(row);
        parts << QStringLiteral("%1(%2)")
                     .arg(row)
                     .arg(commit ? QString::fromStdString(commit->id).left(7) : QStringLiteral("?"));
    }
    return parts.join(QStringLiteral(", "));
}

QStringList MainWindow::describeSelection(const QVector<int>& rows) {
    m_commitView->selectRowsForDiagnostics(rows);
    const SelectionPlan plan = selectionPlan();
    QStringList report;
    report << QStringLiteral("selected %1 row(s)").arg(m_commitView->selectedRows().size());
    // Which rows, said in full: a check that only counted them could not tell a wrong row
    // from a missing one, and the rows a graph draws are not in the order one assumes.
    for (const int row : m_commitView->selectedRows()) {
        const CommitInfo* commit = m_commitModel->commitAt(row);
        report << QStringLiteral("  row %1: %2 %3")
                      .arg(row)
                      .arg(commit ? QString::fromStdString(commit->id).left(7) : QStringLiteral("?"),
                           commit ? QString::fromStdString(commit->summary) : QString());
    }
    report << QStringLiteral("on current branch: %1")
                  .arg(plan.allOnCurrentBranch ? QStringLiteral("yes") : QStringLiteral("no"));
    report << QStringLiteral("elsewhere: %1")
                  .arg(plan.allElsewhere ? QStringLiteral("yes") : QStringLiteral("no"));
    report << QStringLiteral("drop: %1  squash: %2  cherry-pick: %3")
                  .arg(plan.canDrop ? QStringLiteral("yes") : QStringLiteral("no"),
                       plan.canSquash ? QStringLiteral("yes") : QStringLiteral("no"),
                       plan.canCherryPick ? QStringLiteral("yes") : QStringLiteral("no"));
    if (!plan.squashRefusal.empty()) {
        report << QStringLiteral("squash refused: %1").arg(QString::fromStdString(plan.squashRefusal));
    }
    if (!plan.refusal.empty()) {
        report << QStringLiteral("refused: %1").arg(QString::fromStdString(plan.refusal));
    }
    return report;
}

QStringList MainWindow::runSelectionForDiagnostics(const QVector<int>& rows,
                                                   const QString& operation) {
    m_commitView->selectRowsForDiagnostics(rows);
    const SelectionPlan plan = selectionPlan();
    if (operation == QLatin1String("cherry-pick")) {
        runSelectionOperation(SelectionOperation::CherryPick);
    } else if (operation == QLatin1String("drop")) {
        runSelectionOperation(SelectionOperation::Drop);
    } else if (operation == QLatin1String("squash")) {
        runSelectionOperation(SelectionOperation::Squash);
    }
    QStringList report;
    report << QStringLiteral("status: %1").arg(statusBar()->currentMessage());
    report << QStringLiteral("head: %1")
                  .arg(m_session->resolveRevision(QStringLiteral("HEAD")).left(8));
    report << QStringLiteral("commits: %1")
                  .arg(m_session->resolveRevision(QStringLiteral("HEAD")));
    report << QStringLiteral("conflicts: %1").arg(m_session->conflicts().size());
    report << QStringLiteral("in progress: %1")
                  .arg(m_session->rebaseInProgress() ? QStringLiteral("rebase") : QStringLiteral("no"));
    report << QStringLiteral("planned drop=%1 squash=%2 pick=%3")
                  .arg(plan.canDrop ? 1 : 0)
                  .arg(plan.canSquash ? 1 : 0)
                  .arg(plan.canCherryPick ? 1 : 0);
    return report;
}

void MainWindow::cherryPickCommit(const QString& commitId) {
    QString error;
    QString created;
    if (!m_session->cherryPick({commitId}, &created, &error)) {
        QMessageBox::warning(this, QStringLiteral("Cherry pick"), error);
        // A failed cherry-pick leaves conflicts in the index, so go where they
        // can be resolved rather than leaving the user staring at the graph.
        m_commitModel->reset();
        refreshPanels();
        if (!m_session->conflicts().empty()) {
            showConflictsPage();
        }
        return;
    }
    m_commitModel->reset();
    refreshPanels();
    setStatus(QStringLiteral("Cherry-picked %1 as %2.").arg(commitId.left(7), created.left(7)));
}

void MainWindow::revertCommit(const QString& commitId) {
    QString error;
    QString created;
    if (!m_session->revertCommit(commitId, &created, &error)) {
        QMessageBox::warning(this, QStringLiteral("Revert"), error);
        m_commitModel->reset();
        refreshPanels();
        if (!m_session->conflicts().empty()) {
            showConflictsPage();
        }
        return;
    }
    m_commitModel->reset();
    refreshPanels();
    setStatus(QStringLiteral("Reverted %1 as %2.").arg(commitId.left(7), created.left(7)));
}

void MainWindow::rewriteCommitInHistory(const QString& commitId, RewriteAction action) {
    const QString current = QString::fromStdString(m_session->info().headBranch);
    const bool folding = action != RewriteAction::Reword;

    const QString parent = m_session->resolveRevision(commitId + QLatin1Char('^'));
    if (folding && parent.isEmpty()) {
        setStatus(QStringLiteral("“%1” is at the bottom of the history — there is nothing to fold it into.")
                      .arg(commitId.left(7)));
        return;
    }
    // Folding starts the replay one commit further back so the line to fold into is in it.
    const QString base = folding ? rebaseBaseBelow(parent) : rebaseBaseBelow(commitId);
    if (folding && !askYes(QStringLiteral("Rewrite history"),
                           QStringLiteral("Fold %1 into the commit below it on “%2”? Both are rewritten.")
                               .arg(commitId.left(7), current))) {
        return;
    }

    // The plan must name every commit the range holds, or git silently leaves out the rest.
    std::vector<RewriteStep> steps;
    bool named = false;
    for (const RebaseCandidate& candidate :
         m_session->rebaseCandidates(base, QStringLiteral("HEAD"))) {
        RewriteStep step;
        step.commit = candidate.id;
        if (candidate.id == commitId.toStdString()) {
            step.action = action;
            if (action == RewriteAction::Reword) {
                step.message = "commit message rewritten by GitMycelium";
            }
            named = true;
        } else {
            step.action = RewriteAction::Keep;
        }
        steps.push_back(std::move(step));
    }
    if (!named) {
        setStatus(QStringLiteral("“%1” is not in the range that would be replayed.")
                      .arg(commitId.left(7)));
        return;
    }

    bool done = true;
    runWrite(QStringLiteral("Cannot rewrite the history"),
             [&](QString* error) {
                 done = m_session->rewriteHistory(base, steps, error);
                 return done;
             },
             true);
    if (!done) {
        return;
    }
    if (m_session->rebaseInProgress()) {
        sweepConflictsAfterOperation(
            QStringLiteral("%1 conflict(s) while rewriting the history").arg(
                m_session->conflicts().size()));
        return;
    }
    setStatus(QStringLiteral("History rewritten on “%1”.").arg(current));
}

void MainWindow::dropCommitFromHistory(const QString& commitId) {
    const QString current = QString::fromStdString(m_session->info().headBranch);
    if (!askYes(QStringLiteral("Drop commit"),
                QStringLiteral("Remove %1 from the history of “%2”? Everything above it is "
                               "replayed onto its parent, which rewrites those commits.")
                    .arg(commitId.left(7), current))) {
        return;
    }

    bool dropped = true;
    runWrite(QStringLiteral("Cannot drop the commit"),
             [&](QString* error) {
                 dropped = m_session->dropCommit(commitId, error);
                 return dropped;
             },
             true);
    if (!dropped) {
        return;
    }

    // A replay that stopped is a conflict, not a failure: the same page a conflicting
    // rebase lands on, because it is the same operation underneath.
    if (m_session->rebaseInProgress()) {
        sweepConflictsAfterOperation(
            QStringLiteral("%1 conflict(s) from dropping %2").arg(m_session->conflicts().size())
                .arg(commitId.left(7)));
        return;
    }
    setStatus(QStringLiteral("%1 dropped from “%2”.").arg(commitId.left(7), current));
}

void MainWindow::interactiveRebaseFromDrop(const QString& source, const QString& target) {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    const QString base = m_session->mergeBase(source, target);
    if (base.isEmpty()) {
        setStatus(QStringLiteral("“%1” and “%2” share no commit to rebase from.")
                      .arg(source, target));
        return;
    }
    interactiveRebaseRange(base, source, target, source);
}

void MainWindow::interactiveRebaseFromCommit(const QString& commitId) {
    interactiveRebaseRange(rebaseBaseBelow(commitId));
}

void MainWindow::interactiveRebaseRange(const QString& base, const QString& tip,
                                        const QString& onto, const QString& branch) {
    if (!m_session || !m_session->isOpen() || base.isEmpty()) {
        return;
    }
    // An operation already open is not a reason to start another one: git would refuse,
    // and the work waiting for the reader is on the page this leads to instead.
    if (m_session->rebaseInProgress() || m_session->mergeInProgress() ||
        m_session->squashInProgress()) {
        showConflictsPage();
        refreshPanels();
        setStatus(QStringLiteral("A rebase or merge is already in progress — finish it or abort it "
                                 "before starting another."));
        return;
    }

    const std::vector<RebaseCandidate> candidates = m_session->rebaseCandidates(base, tip);
    if (candidates.empty()) {
        setStatus(QStringLiteral("Nothing to rebase: the range %1..%2 holds no commits.")
                      .arg(base == QLatin1String(kRebaseRoot) ? QStringLiteral("the root")
                                                              : base.left(7),
                           tip));
        return;
    }

    RebaseEditor editor(this);
    editor.setCandidates(candidates);
    if (editor.exec() != QDialog::Accepted) {
        return;
    }
    const std::vector<RewriteStep> steps = editor.steps();
    if (steps.empty()) {
        return;
    }
    if (!askYes(QStringLiteral("Interactive rebase"),
                QStringLiteral("Rewrite %1 commit(s) on “%2”?\n\nEvery commit above the base "
                               "gets a new id.")
                    .arg(steps.size())
                    .arg(branch.isEmpty() ? QString::fromStdString(m_session->info().headBranch)
                                          : branch))) {
        return;
    }

    bool done = true;
    runWrite(QStringLiteral("Cannot rewrite the history"),
             [&](QString* error) {
                 done = m_session->rewriteHistory(base, steps, error, onto, branch);
                 return done;
             },
             true);
    if (!done) {
        return;
    }
    // A rebase stopped for an `edit` has no conflicts and is still open — that is not a
    // failure, and the page it lands on is where the reader finishes it.
    if (m_session->rebaseInProgress()) {
        sweepConflictsAfterOperation(
            QStringLiteral("Rebase stopped — %1 conflict(s)").arg(m_session->conflicts().size()));
        return;
    }
    setStatus(QStringLiteral("Rebased %1 commit(s).").arg(steps.size()));
}

QStringList MainWindow::rebaseEditorForDiagnostics(const QString& base, const QStringList& args) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const QString upstream =
        (base == QLatin1String("root") || base == QLatin1String(kRebaseRoot))
            ? QString::fromLatin1(kRebaseRoot)
            : base;
    const std::vector<RebaseCandidate> candidates =
        m_session->rebaseCandidates(upstream, QStringLiteral("HEAD"));
    if (candidates.empty()) {
        report << QStringLiteral("no commits in the range");
        return report;
    }

    if (args.isEmpty()) {
        const std::string text = rebasePlanText(defaultRebasePlan(candidates));
        for (const QString& line :
             QString::fromStdString(text).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            report << line;
        }
        return report;
    }

    std::string error;
    const std::vector<RebasePlanRow> rows =
        parseRebaseSpec(args.join(QLatin1Char(' ')).toStdString(), candidates, &error);
    if (!error.empty()) {
        report << QStringLiteral("bad plan: %1").arg(QString::fromStdString(error));
        return report;
    }
    const std::string refusal = rebasePlanRefusal(rows);
    if (!refusal.empty()) {
        report << QStringLiteral("refused: %1").arg(QString::fromStdString(refusal));
        return report;
    }
    const std::vector<RewriteStep> steps = rebasePlanSteps(rows);
    QString runError;
    if (!m_session->rewriteHistory(upstream, steps, &runError)) {
        report << QStringLiteral("failed: %1").arg(runError.trimmed());
        return report;
    }
    refreshPanels();
    if (m_session->rebaseInProgress()) {
        report << QStringLiteral("stopped: %1 conflict(s)").arg(m_session->conflicts().size());
    } else {
        report << QStringLiteral("rebased %1 commit(s)").arg(steps.size());
    }
    report << QStringLiteral("head: %1")
                  .arg(QString::fromStdString(m_session->info().headId).left(8));
    report << QStringLiteral("branch: %1")
                  .arg(QString::fromStdString(m_session->info().headBranch));
    return report;
}

void MainWindow::amendStoppedRebaseCommit() {
    if (!m_session || !m_session->rebaseInProgress()) {
        setStatus(QStringLiteral("No rebase is stopped to amend."));
        return;
    }
    bool amended = true;
    runWrite(QStringLiteral("Cannot amend the stopped commit"),
             [&](QString* inner) {
                 amended = m_session->amendStoppedCommit(QString(), inner);
                 return amended;
             });
    if (!amended) {
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Stopped commit amended — continue the rebase when ready."));
}

QStringList MainWindow::rebaseAmendForDiagnostics(const QString& message) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    QString error;
    const bool okay = m_session->amendStoppedCommit(message, &error);
    report << (okay ? QStringLiteral("amended") : QStringLiteral("failed: %1").arg(error.trimmed()));
    report << QStringLiteral("rebase open: %1")
                  .arg(m_session->rebaseInProgress() ? QStringLiteral("yes")
                                                     : QStringLiteral("no"));
    return report;
}

QStringList MainWindow::compareForDiagnostics(const QString& base, const QString& target,
                                              const QString& path, bool mergeBase) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const QString baseId = m_session->resolveRevision(base);
    if (baseId.isEmpty()) {
        report << QStringLiteral("cannot resolve %1").arg(base);
        return report;
    }
    const bool workingTree = target == QStringLiteral("//wip") || target == QStringLiteral("WORKTREE");
    QString targetId;
    if (!workingTree) {
        targetId = m_session->resolveRevision(target);
        if (targetId.isEmpty()) {
            report << QStringLiteral("cannot resolve %1").arg(target);
            return report;
        }
    }

    if (workingTree) {
        if (!path.isEmpty()) {
            const FileDiff diff = m_session->workdirFileDiffAgainst(baseId, path);
            report << QStringLiteral("%1: +%2 -%3, %4 hunk(s) (working tree)")
                          .arg(path)
                          .arg(diff.added)
                          .arg(diff.removed)
                          .arg(diff.hunks.size());
            return report;
        }
        const std::vector<ChangedFile> files = m_session->workdirChangedFilesAgainst(baseId);
        report << QStringLiteral("files: %1 (working tree)").arg(files.size());
        for (const ChangedFile& file : files) {
            report << QStringLiteral("%1 | %2")
                          .arg(QString::fromStdString(file.path))
                          .arg(static_cast<int>(file.kind));
        }
        return report;
    }

    if (mergeBase) {
        const QString mergeBaseId = m_session->mergeBase(baseId, targetId);
        if (mergeBaseId.isEmpty()) {
            report << QStringLiteral("no merge base between %1 and %2").arg(base, target);
            return report;
        }
        report << QStringLiteral("merge-base: %1").arg(mergeBaseId.left(10));
        if (!path.isEmpty()) {
            const FileDiff diff = m_session->fileDiffBetween(mergeBaseId, targetId, path);
            report << QStringLiteral("%1: +%2 -%3, %4 hunk(s)")
                          .arg(path)
                          .arg(diff.added)
                          .arg(diff.removed)
                          .arg(diff.hunks.size());
            return report;
        }
        const std::vector<ChangedFile> files =
            m_session->changedFilesBetween(mergeBaseId, targetId);
        report << QStringLiteral("files: %1").arg(files.size());
        for (const ChangedFile& file : files) {
            report << QStringLiteral("%1 | %2")
                          .arg(QString::fromStdString(file.path))
                          .arg(static_cast<int>(file.kind));
        }
        return report;
    }

    if (!path.isEmpty()) {
        const FileDiff diff = m_session->fileDiffBetween(baseId, targetId, path);
        report << QStringLiteral("%1: +%2 -%3, %4 hunk(s)")
                      .arg(path)
                      .arg(diff.added)
                      .arg(diff.removed)
                      .arg(diff.hunks.size());
        return report;
    }
    const std::vector<ChangedFile> files = m_session->changedFilesBetween(baseId, targetId);
    report << QStringLiteral("files: %1").arg(files.size());
    for (const ChangedFile& file : files) {
        report << QStringLiteral("%1 | %2")
                      .arg(QString::fromStdString(file.path))
                      .arg(static_cast<int>(file.kind));
    }
    return report;
}

QStringList MainWindow::restoreFileForDiagnostics(const QString& revision, const QString& path,
                                                  bool staged) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    QString error;
    if (!m_session->restoreFile(revision, path, staged, &error)) {
        report << QStringLiteral("failed: %1").arg(error.trimmed());
        return report;
    }
    report << QStringLiteral("restored %1 from %2%3")
                  .arg(path, revision.left(10), staged ? QStringLiteral(" (staged)")
                                                       : QString());
    return report;
}

QStringList MainWindow::blameForDiagnostics(const QString& path, const QString& revision) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const std::vector<BlameLine> lines = m_session->blame(path, revision);
    report << QStringLiteral("lines: %1").arg(lines.size());
    for (const BlameLine& line : lines) {
        report << QStringLiteral("%1 | %2 | %3 | %4")
                      .arg(line.finalLine)
                      .arg(line.uncommitted ? QStringLiteral("(uncommitted)")
                                            : QString::fromStdString(line.commit).left(8),
                           QString::fromStdString(line.author),
                           QString::fromStdString(line.summary));
    }
    return report;
}

QStringList MainWindow::reflogForDiagnostics(int limit) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const std::vector<ReflogEntry> entries = m_session->reflog(static_cast<std::size_t>(limit));
    report << QStringLiteral("entries: %1").arg(entries.size());
    for (const ReflogEntry& entry : entries) {
        report << QStringLiteral("%1 | %2 | %3 | %4")
                      .arg(QString::fromStdString(entry.selector),
                           QString::fromStdString(entry.action),
                           QString::fromStdString(entry.shortId),
                           QString::fromStdString(entry.message));
    }
    return report;
}

QStringList MainWindow::fileHistoryForDiagnostics(const QString& path) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const std::vector<FileHistoryEntry> entries = m_session->fileHistory(path);
    report << QStringLiteral("commits: %1").arg(entries.size());
    for (const FileHistoryEntry& entry : entries) {
        report << QStringLiteral("%1 | %2 | %3")
                      .arg(QString::fromStdString(entry.id).left(8),
                           QString::fromStdString(entry.author),
                           QString::fromStdString(entry.summary));
    }
    return report;
}

void MainWindow::resetCurrentBranchTo(const QString& commitId, ResetMode mode) {
    const QString current = QString::fromStdString(m_session->info().headBranch);
    if (mode == ResetMode::Hard) {
        if (!askYes(QStringLiteral("Hard reset"),
                    QStringLiteral("Move “%1” to %2 and throw away all local changes?")
                        .arg(current, commitId.left(7)))) {
            return;
        }
    }
    runWrite(QStringLiteral("Cannot reset"),
             [&](QString* error) { return m_session->resetTo(commitId, mode, error); }, true);
    setStatus(QStringLiteral("“%1” reset to %2.").arg(current, commitId.left(7)));
}

void MainWindow::savePatchForCommit(const QString& commitId) {
    QString patch;
    QString error;
    if (!m_session->formatPatch(commitId, &patch, &error)) {
        QMessageBox::warning(this, QStringLiteral("Create patch"), error);
        return;
    }

    const QString suggested = QDir::current().filePath(
        QStringLiteral("%1.patch").arg(commitId.left(12)));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save patch"), suggested,
        QStringLiteral("Patch files (*.patch);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(patch.toUtf8()) < 0) {
        QMessageBox::warning(this, QStringLiteral("Create patch"),
                             QStringLiteral("Cannot write %1.").arg(path));
        return;
    }
    setStatus(QStringLiteral("Patch for %1 written to %2.").arg(commitId.left(7), path));
}

void MainWindow::applyPatchInteractive() {
    if (!m_session->isOpen()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Apply patch"), QDir::currentPath(),
        QStringLiteral("Patch files (*.patch *.diff *.mbox);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    bool mailbox = false;
    QString error;
    if (!m_session->applyPatch(path, &mailbox, &error)) {
        QMessageBox::warning(this, QStringLiteral("Apply patch"), error);
        return;
    }
    if (mailbox) {
        m_commitModel->reset();
        refreshPanels();
        setStatus(QStringLiteral("Applied %1 as commits.").arg(QFileInfo(path).fileName()));
    } else {
        refreshPanels();
        showWorkingTreeChanges();
        setStatus(QStringLiteral("Applied %1 to the working tree — review and commit.")
                      .arg(QFileInfo(path).fileName()));
    }
}

namespace {
QString maintenanceStatusText(MaintenanceTask task) {
    switch (task) {
    case MaintenanceTask::Gc:
        return QStringLiteral("Garbage collecting…");
    case MaintenanceTask::Fsck:
        return QStringLiteral("Checking repository integrity…");
    case MaintenanceTask::CommitGraph:
        return QStringLiteral("Writing the commit-graph…");
    case MaintenanceTask::MultiPackIndex:
        return QStringLiteral("Writing the multi-pack-index…");
    case MaintenanceTask::Maintenance:
        return QStringLiteral("Running scheduled maintenance…");
    case MaintenanceTask::Optimize:
        return QStringLiteral("Optimizing the repository…");
    }
    return QStringLiteral("Running repository maintenance…");
}
} // namespace

void MainWindow::runMaintenance(MaintenanceTask task) {
    if (!m_session->isOpen()) {
        return;
    }
    if (m_maintenanceRunning) {
        setStatus(QStringLiteral("Repository maintenance is already running."));
        return;
    }
    m_maintenanceRunning = true;
    if (m_commitView) {
        m_commitView->setBusy(true);
    }
    setStatus(maintenanceStatusText(task));
    m_session->requestMaintenance(task);
}

void MainWindow::onMaintenanceFinished(bool ok, const QString& task, const QString& output) {
    m_maintenanceRunning = false;
    if (m_commitView) {
        m_commitView->setBusy(false);
    }
    // fsck reports normal progress on stdout even when the repository is fine, so
    // "ok" is the exit code, not whether there was output. A failure names the task
    // and shows git's own words.
    if (!ok) {
        notify(NotificationLog::Error, QStringLiteral("git %1 failed.").arg(task));
        QMessageBox::warning(this, QStringLiteral("Repository maintenance"),
                             QStringLiteral("git %1 failed:\n%2").arg(task, output.trimmed()));
        return;
    }
    notify(NotificationLog::Success, QStringLiteral("git %1 finished.").arg(task));
}

void MainWindow::copyTextToClipboard(const QString& text, const QString& what) {
    QGuiApplication::clipboard()->setText(text);
    setStatus(QStringLiteral("%1 copied.").arg(what));
}

bool MainWindow::localBranchExists(const QString& shortName) const {
    for (const RefInfo& ref : m_session->refs()) {
        if (ref.kind == RefKind::LocalBranch &&
            QString::fromStdString(ref.shortName) == shortName) {
            return true;
        }
    }
    return false;
}

void MainWindow::checkoutBranchFromGraph(const QString& shortName, RefKind kind) {
    if (!m_session->isOpen() || shortName.isEmpty()) {
        return;
    }

    if (kind == RefKind::LocalBranch) {
        checkoutLocalBranch(shortName);
        return;
    }

    // A remote-tracking branch is not a place the working tree can stand: checking
    // one out means making a local branch that follows it. Asked, not assumed — it
    // creates a branch, and a branch nobody asked for outlives the click.
    const QString localName = shortName.section(QLatin1Char('/'), 1);
    if (localName.isEmpty()) {
        return;
    }
    if (localBranchExists(localName)) {
        checkoutLocalBranch(localName);
        return;
    }

    const auto answer = QMessageBox::question(
        this, QStringLiteral("Check out a remote branch"),
        QStringLiteral("Create the branch %1 here, following %2?").arg(localName, shortName),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes) {
        return;
    }
    runCheckout(
        QStringLiteral("Cannot check out branch"), localName,
        [localName, shortName](RepoSession& s, QString* error) {
            return s.createTrackingBranch(localName, shortName, error);
        },
        [this, localName, shortName] {
            setStatus(QStringLiteral("On branch %1, following %2.").arg(localName, shortName));
        });
}

void MainWindow::checkoutSelectedBranch() {
    if (!m_session->isOpen() || !selectedRefIsLocalBranch()) {
        return;
    }
    const QString name = selectedRefShortName();
    if (name.isEmpty()) {
        return;
    }
    checkoutLocalBranch(name);
}

void MainWindow::createBranchFromSelection() {
    if (!m_session->isOpen()) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("New branch"),
                                               QStringLiteral("Branch name:"), QLineEdit::Normal,
                                               QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    const QModelIndex current = m_commitView->currentIndex();
    const CommitInfo* commit = m_commitModel->commitAt(current.row());
    const QString target = commit ? QString::fromStdString(commit->id)
                                  : QString::fromStdString(m_session->info().headId);
    if (target.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("New branch"),
                                 QStringLiteral("The repository has no commits yet."));
        return;
    }
    runWrite(QStringLiteral("Cannot create branch"),
             [&](QString* error) { return m_session->createBranch(name.trimmed(), target, error); },
             false);
    refreshRefs();
}

void MainWindow::deleteSelectedBranch() {
    if (!m_session->isOpen() || !selectedRefIsLocalBranch()) {
        return;
    }
    const QString name = selectedRefShortName();
    if (name.isEmpty()) {
        return;
    }
    deleteBranchesWithConsent({name}, QString(), QStringLiteral("Delete branch"));
}

void MainWindow::deleteSelectedBranches() {
    if (!m_session->isOpen()) {
        return;
    }
    const QStringList names = selectedLocalBranches();
    if (names.isEmpty()) {
        return;
    }
    deleteBranchesWithConsent(names, QString(), QStringLiteral("Delete branches"));
}

void MainWindow::deleteMergedBranches() {
    if (!m_session->isOpen()) {
        return;
    }
    const QString target = m_session->targetBranch();
    const QStringList names = m_session->branchesMergedInto(target);
    if (names.isEmpty()) {
        const QString against = target.isEmpty()
                                    ? QStringLiteral("the current branch")
                                    : target.section(QLatin1Char('/'), -1);
        QMessageBox::information(
            this, QStringLiteral("Delete merged branches"),
            QStringLiteral("Every local branch carries commits that %1 does not have.")
                .arg(against));
        return;
    }
    deleteBranchesWithConsent(names, target, QStringLiteral("Delete merged branches"));
}

void MainWindow::deleteBranchesWithConsent(const QStringList& names, const QString& base,
                                           const QString& what) {
    if (!m_session->isOpen() || names.isEmpty()) {
        return;
    }
    // Asked before the dialog, not after each refusal: a reader about to lose work
    // deserves to see which branches carry it, and a modal appearing halfway through
    // a loop reads as a failure rather than as a question.
    const QStringList unmerged = m_session->unmergedAmong(names, base);

    QStringList shown = names;
    shown.sort(Qt::CaseInsensitive);

    QString text = names.size() == 1
                       ? QStringLiteral("Delete branch “%1”?").arg(shown.front())
                       : QStringLiteral("Delete %1 branches?").arg(names.size());
    if (!unmerged.isEmpty()) {
        text += QStringLiteral("\n\n%1 of them carry commits nothing else reaches; deleting "
                               "them loses that work.")
                    .arg(unmerged.size());
    }

    QMessageBox box(QMessageBox::Question, what, text, QMessageBox::Yes | QMessageBox::No, this);
    box.setDefaultButton(QMessageBox::No);
    if (unmerged.isEmpty()) {
        box.setDetailedText(shown.join(QLatin1Char('\n')));
    } else {
        QStringList lines;
        lines << QStringLiteral("Will lose commits:") << unmerged;
        box.setDetailedText(lines.join(QLatin1Char('\n')));
    }
    if (box.exec() != QMessageBox::Yes) {
        return;
    }

    // The question above was the consent, including for the branches that lose
    // commits. The session still applies force branch by branch, and only to the
    // ones that are actually unreachable — never as a retry after any failure.
    RepoSession::BranchBatchResult result;
    QString error;
    if (!m_session->deleteBranches(names, base, /*forceUnmerged=*/!unmerged.isEmpty(), &result,
                                   &error)) {
        QMessageBox::warning(this, what, error);
        return;
    }
    reportBranchBatch(what, result);
}

void MainWindow::reportBranchBatch(const QString& what,
                                   const RepoSession::BranchBatchResult& result) {
    if (!result.failed.isEmpty()) {
        QMessageBox::warning(this, what, result.failed.join(QLatin1Char('\n')));
    }
    if (result.done.isEmpty()) {
        setStatus(QStringLiteral("%1: nothing changed.").arg(what));
    } else {
        setStatus(QStringLiteral("%1: %2 done.").arg(what).arg(result.done.size()));
    }
    refreshRefs();
    refreshPanels();
}

void MainWindow::setUpstreamsForSelected() {
    if (!m_session->isOpen()) {
        return;
    }
    const QStringList names = selectedLocalBranches();
    if (names.isEmpty()) {
        return;
    }
    if (m_remotes.empty()) {
        QMessageBox::information(this, QStringLiteral("Set upstream"),
                                 QStringLiteral("This repository has no remote configured."));
        return;
    }
    QStringList remotes;
    remotes.reserve(static_cast<int>(m_remotes.size()));
    for (const RemoteInfo& remote : m_remotes) {
        remotes << QString::fromStdString(remote.name);
    }
    bool ok = false;
    const QString remote = QInputDialog::getItem(
        this, QStringLiteral("Set upstream"),
        QStringLiteral("Which remote should %1 branches follow?").arg(names.size()), remotes, 0,
        false, &ok);
    if (!ok || remote.isEmpty()) {
        return;
    }
    RepoSession::BranchBatchResult result;
    QString error;
    if (!m_session->setUpstreams(names, remote, &result, &error)) {
        QMessageBox::warning(this, QStringLiteral("Cannot set upstream"), error);
        return;
    }
    reportBranchBatch(QStringLiteral("Set upstream"), result);
}

void MainWindow::hideSelectedBranches(bool hidden) {
    const QStringList names = selectedLocalBranches();
    if (names.isEmpty()) {
        return;
    }
    for (const QString& name : names) {
        m_visibility.setHidden(name, hidden);
    }
    applyVisibility();
    refreshRefs();
}

void MainWindow::soloSelectedBranches(bool solo) {
    const QStringList names = selectedLocalBranches();
    if (names.isEmpty()) {
        return;
    }
    for (const QString& name : names) {
        m_visibility.setSolo(name, solo);
    }
    applyVisibility();
    refreshRefs();
}

void MainWindow::mergeBranchIntoCurrent() {
    if (!m_session->isOpen()) {
        return;
    }
    const RepoInfo info = m_session->info();
    const QString current = QString::fromStdString(info.headBranch);

    QStringList candidates;
    // Local branches first, then the remote-tracking ones: merging origin/main is the
    // merge a pull makes, and a reader who fetched may want to make it by hand.
    QStringList remoteCandidates;
    for (const RefInfo& ref : m_session->refs()) {
        const QString name = QString::fromStdString(ref.shortName);
        if (ref.kind == RefKind::LocalBranch && name != current) {
            candidates << name;
        } else if (ref.kind == RefKind::RemoteBranch && !name.endsWith(QStringLiteral("/HEAD"))) {
            remoteCandidates << name;
        }
    }
    candidates << remoteCandidates;
    if (candidates.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Merge"),
                                 QStringLiteral("There is no other branch to merge."));
        return;
    }

    bool ok = false;
    const QString source = QInputDialog::getItem(this, QStringLiteral("Merge"),
                                                 QStringLiteral("Merge branch into “%1”:").arg(current),
                                                 candidates, 0, false, &ok);
    if (!ok || source.isEmpty()) {
        return;
    }
    mergeBranchInto(source, current);
}

void MainWindow::mergeBranchInto(const QString& source, const QString& target,
                                 bool preflightShown) {
    logBadgeFlow(QStringLiteral("window: mergeBranchInto %1 into %2").arg(source, target));
    // An operation already open is not a reason to start another one. git refuses, the
    // reader gets a dialog about a rebase they cannot see, and the work actually waiting
    // for them — the conflicted files on the conflict page — stays hidden behind it.
    // Take them to that page instead, where the operation they started is asking.
    if (m_session->isOpen() && (m_session->rebaseInProgress() || m_session->mergeInProgress() ||
                                m_session->squashInProgress())) {
        const bool rebasing = m_session->rebaseInProgress();
        logBadgeFlow(QStringLiteral("window: REFUSED, a %1 is already open")
                         .arg(rebasing ? QStringLiteral("rebase") : QStringLiteral("merge")));
        showConflictsPage();
        refreshPanels();
        setStatus(QStringLiteral("A %1 is already in progress — finish it or abort it before starting another.")
                      .arg(rebasing ? QStringLiteral("rebase") : QStringLiteral("merge")));
        return;
    }


    // Predict the conflicts before switching anything: a reader who changes their
    // mind leaves the repository exactly as it was. Merging is symmetric, so the
    // pair can be named in either order. The drop list already showed the prediction,
    // so the modal is skipped there — the same question twice is a worse answer.
    if (!preflightShown && !confirmPreflight(source, target)) {
        setStatus(QStringLiteral("Merge cancelled — conflicts were predicted."));
        return;
    }

    const QString current = QString::fromStdString(m_session->info().headBranch);
    if (current != target) {
        QString error;
        if (!m_session->checkoutBranch(target, &error)) {
            QMessageBox::warning(this, QStringLiteral("Cannot switch to %1").arg(target), error);
            return;
        }
    }

    MergeResult result;
    QString error;
    if (!m_session->mergeBranch(source, &result, &error)) {
        QMessageBox::warning(this, QStringLiteral("Merge failed"), error);
        return;
    }

    m_commitModel->reset();
    refreshPanels();

    switch (result.status) {
    case MergeStatus::UpToDate:
        setStatus(QStringLiteral("Already up to date — nothing to merge from %1.").arg(source));
        break;
    case MergeStatus::FastForward:
        setStatus(QStringLiteral("Fast-forwarded %1 to %2.").arg(target, source));
        break;
    case MergeStatus::Clean:
        setStatus(QStringLiteral("Merged %1 into %2 — merge commit created.").arg(source, target));
        break;
    case MergeStatus::Conflicted:
        sweepConflictsAfterOperation(QStringLiteral("%1 conflict(s) from merging %2")
                                         .arg(result.conflictCount)
                                         .arg(source));
        break;
    }

    // A completed merge may advance the recorded gitlinks. One that stopped on a
    // conflict has not: the reader finishes it first.
    if (result.status == MergeStatus::FastForward || result.status == MergeStatus::Clean) {
        maybeAutoUpdateSubmodules();
    }
}

void MainWindow::sweepConflictsAfterOperation(const QString& what) {
    if (!m_session->isOpen()) {
        return;
    }

    const RepoSession::SweepOutcome sweep = m_session->sweepOperationConflicts();
    m_commitModel->reset();
    refreshPanels();

    if (sweep.finished) {
        setStatus(QStringLiteral("%1 — %2 disagreement(s) settled automatically, nothing left.")
                      .arg(what)
                      .arg(sweep.hunksResolved));
        if (!sweep.autoDetail.isEmpty() && statusBar()) {
            statusBar()->setToolTip(sweep.autoDetail.join(QLatin1Char('\n')));
        }
        return;
    }

    if (sweep.needsAPerson) {
        QStringList parts;
        if (!sweep.autoStaged.isEmpty()) {
            parts << QStringLiteral("%1 settled automatically").arg(sweep.autoStaged.join(QStringLiteral(", ")));
        }
        parts << QStringLiteral("%1 need you").arg(sweep.needsAPersonPaths.join(QStringLiteral(", ")));
        setStatus(QStringLiteral("%1 — %2.").arg(what, parts.join(QStringLiteral("; "))));
        if (!sweep.autoDetail.isEmpty() && statusBar()) {
            statusBar()->setToolTip(sweep.autoDetail.join(QLatin1Char('\n')));
        }
        return;
    }

    // Nothing was settled and nothing is blocked: an operation whose conflicts a
    // person has already resolved is theirs to finish.
    setStatus(what);
}

QStringList MainWindow::simulateBranchDrop(const QString& source, const QString& target,
                                           const QString& pick) {
    QStringList report = m_commitView->simulateBranchDrop(source, target, pick);
    // What the window shows once the dust settles: the operations run synchronously, so
    // by here the rebase, the automatic pass and the page switch have all happened.
    // The conflict page is a page of the outer stack, not of the centre column.
    report << QStringLiteral("page: %1")
                  .arg(m_stack && m_stack->currentWidget() == m_conflictView
                           ? QStringLiteral("CONFLICT")
                           : QStringLiteral("history"));
    report << QStringLiteral("rebase open: %1")
                  .arg(m_session->rebaseInProgress() ? QStringLiteral("yes")
                                                     : QStringLiteral("no"));
    report << QStringLiteral("conflicts: %1").arg(m_session->conflicts().size());
    report << QStringLiteral("status: %1")
                  .arg(statusBar() ? statusBar()->currentMessage() : QStringLiteral("?"));
    return report;
}

void MainWindow::setHeadless(bool headless) {
    m_headless = headless;
    if (!headless) {
        return;
    }
    // A frame caught mid-fade would make a headless check's output depend on the
    // clock. The graph stands still when nobody is watching it move.
    if (m_commitView) {
        m_commitView->setAnimationsEnabled(false);
    }
    Skeleton::setAnimationsEnabled(false);
    // A modal dialog is a hang in a headless run and tells the log nothing. Close what
    // appears and write down what it said.
    auto* watchdog = new QTimer(this);
    connect(watchdog, &QTimer::timeout, this, [this] {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal) {
            return;
        }
        QString title;
        QString text;
        if (auto* box = qobject_cast<QMessageBox*>(modal)) {
            title = box->windowTitle();
            text = box->text();
        }
        logBadgeFlow(QStringLiteral("dialog: %1 — %2").arg(title, text));
        modal->close();
    });
    watchdog->start(50);
}

bool MainWindow::askYes(const QString& title, const QString& text) {
    if (m_headless) {
        logBadgeFlow(QStringLiteral("confirm: yes — %1: %2").arg(title, text.left(80)));
        return true;
    }
    return QMessageBox::warning(this, title, text, QMessageBox::Yes | QMessageBox::No,
                                QMessageBox::No) == QMessageBox::Yes;
}

void MainWindow::reportFailure(const QString& title, const QString& text) {
    logBadgeFlow(QStringLiteral("failure: %1 — %2").arg(title, text.left(160)));
    // Every failure path routes through here, so this is the one place that has to
    // remember the notification centre.
    m_notifications.add(NotificationLog::Error, QStringLiteral("%1: %2").arg(title, text.left(200)));
    if (m_headless) {
        setStatus(QStringLiteral("%1: %2").arg(title, text));
        return;
    }
    QMessageBox::warning(this, title, text);
}

QStringList MainWindow::describeHoverRow(int row) {
    m_commitView->setHoverRowForDiagnostics(row);
    const QString ghost = m_commitView->ghostBranchAt(row);
    QStringList report{m_commitView->describeHover(row)};
    report << QStringLiteral("ghost: %1")
                  .arg(ghost.isEmpty() ? QStringLiteral("(none)") : ghost);
    report << QStringLiteral("highlight: %1")
                  .arg(m_commitModel->highlightActive() ? m_commitModel->highlightedBranch()
                                                        : QStringLiteral("(none)"));
    int dimmed = 0;
    for (int r = 0; r < m_commitModel->rowCount(); ++r) {
        if (m_commitModel->isDimmedRow(r)) {
            ++dimmed;
        }
    }
    report << QStringLiteral("dimmed: %1/%2").arg(dimmed).arg(m_commitModel->rowCount());
    return report;
}

QStringList MainWindow::hoverExpandForDiagnostics(int row) {
    QStringList report;
    m_commitView->setHoverExpansionForDiagnostics(row);
    const QStringList collapsed = m_commitView->hoverBranchesAt(row);
    report << QStringLiteral("collapsed: %1")
                  .arg(collapsed.isEmpty() ? QStringLiteral("(none)") : collapsed.join(QLatin1Char(',')));
    const QStringList revealed = m_commitView->expandedBranchesForDiagnostics();
    report << QStringLiteral("revealed: %1")
                  .arg(revealed.isEmpty() ? QStringLiteral("(none)") : revealed.join(QLatin1Char(',')));
    return report;
}

QStringList MainWindow::activateHoverGhostForDiagnostics(int row) {
    QStringList report;
    m_commitView->setHoverRowForDiagnostics(row);
    const QString ghost = m_commitView->ghostBranchAt(row);
    report << QStringLiteral("ghost: %1")
                  .arg(ghost.isEmpty() ? QStringLiteral("(none)") : ghost);
    if (ghost.isEmpty()) {
        return report;
    }
    m_commitView->activateGhostAt(row);
    report << QStringLiteral("branch: %1")
                  .arg(QString::fromStdString(m_session->info().headBranch));
    report << QStringLiteral("head: %1")
                  .arg(QString::fromStdString(m_session->info().headId).left(10));
    return report;
}

QString MainWindow::loadStateForDiagnostics() const {
    if (!m_commitModel) {
        return QStringLiteral("rows=0 loading=no more=no");
    }
    return QStringLiteral("rows=%1 loading=%2 more=%3")
        .arg(m_commitModel->rowCount())
        .arg(m_commitModel->isLoading() ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(m_commitModel->hasMore() ? QStringLiteral("yes") : QStringLiteral("no"));
}


QStringList MainWindow::columnsForDiagnostics(const QString& op) {
    if (op == QStringLiteral("rich")) {
        setColumnMode(GraphColumnMode::Rich, true);
    } else if (op == QStringLiteral("text")) {
        setColumnMode(GraphColumnMode::Text, true);
    } else if (op == QStringLiteral("compact")) {
        setColumnMode(GraphColumnMode::Compact, true);
    }

    QStringList report;
    if (!m_commitView || !m_commitModel) {
        report << QStringLiteral("no column view");
        return report;
    }
    report << QStringLiteral("mode: %1").arg(QString::fromLatin1(graphColumnModeKey(m_columnMode)));
    report << QStringLiteral("icons: %1")
                  .arg(m_commitModel->refIcons() ? QStringLiteral("yes") : QStringLiteral("no"));
    const auto describe = [this, &report](CommitModel::Column column, const char* name) {
        report << QStringLiteral("%1: w=%2 hidden=%3")
                      .arg(QString::fromLatin1(name))
                      .arg(m_commitView->columnWidth(column))
                      .arg(m_commitView->isColumnHidden(column) ? 1 : 0);
    };
    describe(CommitModel::Refs, "refs");
    describe(CommitModel::Graph, "graph");
    describe(CommitModel::Message, "message");
    describe(CommitModel::Author, "author");
    describe(CommitModel::Date, "date");
    describe(CommitModel::Sha, "sha");
    return report;
}

QStringList MainWindow::timelineForDiagnostics(const QString& op) {
    if (op != QStringLiteral("show")) {
        setTimelineGrouping(timelinePeriodFromKey(op.toStdString()));
        return {};
    }

    QStringList report;
    if (!m_commitModel) {
        report << QStringLiteral("no model");
        return report;
    }
    report << QStringLiteral("period: %1")
                  .arg(QString::fromLatin1(timelinePeriodKey(m_commitModel->timelinePeriod())));
    report << QStringLiteral("rows: %1")
                  .arg(m_commitModel->rowCount());
    int separators = 0;
    for (int row = 0; row < m_commitModel->rowCount(); ++row) {
        if (!m_commitModel->isTimelineRow(row)) {
            continue;
        }
        ++separators;
        report << QStringLiteral("separator row=%1 label=%2")
                      .arg(row)
                      .arg(m_commitModel
                               ->data(m_commitModel->index(row, CommitModel::Message),
                                      Qt::DisplayRole)
                               .toString());
    }
    report << QStringLiteral("separators: %1").arg(separators);
    return report;
}

QStringList MainWindow::sparseForDiagnostics(const QString& op, const QString& argument) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    if (op == QStringLiteral("set") || op == QStringLiteral("disable")) {
        QStringList dirs;
        if (op == QStringLiteral("set")) {
            for (const QString& part : argument.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                dirs << part.trimmed();
            }
        }
        QString error;
        if (!m_session->setSparseCheckout(dirs, &error)) {
            report << QStringLiteral("error: %1").arg(error);
        }
    }
    report << QStringLiteral("enabled: %1")
                  .arg(m_session->sparseCheckoutEnabled() ? QStringLiteral("yes") : QStringLiteral("no"));
    report << QStringLiteral("cone: %1")
                  .arg(m_session->sparseCheckoutDirectories().join(QLatin1Char(',')));
    report << QStringLiteral("top: %1")
                  .arg(m_session->sparseCheckoutTopLevelDirectories().join(QLatin1Char(',')));
    return report;
}

QStringList MainWindow::gitflowForDiagnostics(const QString& op, const QString& argument) {
    if (!m_session || !m_session->isOpen()) {
        return {QStringLiteral("no repository is open")};
    }
    const QStringList parts = argument.split(QLatin1Char('|'));
    QString error;
    if (op == QStringLiteral("init")) {
        if (!m_session->initGitflow(parts.value(0), parts.value(1), &error)) {
            return {QStringLiteral("error: %1").arg(error)};
        }
    } else if (op == QStringLiteral("start")) {
        if (!m_session->gitflowStart(parts.value(0), parts.value(1), &error)) {
            return {QStringLiteral("error: %1").arg(error)};
        }
    } else if (op == QStringLiteral("finish")) {
        if (!m_session->gitflowFinish(parts.value(0), parts.value(1), parts.value(2), &error)) {
            return {QStringLiteral("error: %1").arg(error)};
        }
    }

    QStringList report;
    report << QStringLiteral("initialized: %1")
                  .arg(m_session->gitflowInitialized() ? QStringLiteral("yes")
                                                       : QStringLiteral("no"));
    report << m_session->gitflowSummary();
    report << QStringLiteral("head: %1")
                  .arg(QString::fromStdString(m_session->info().headBranch));
    report << QStringLiteral("branches: %1")
                  .arg([this] {
                      QStringList names;
                      for (const RefInfo& ref : m_session->refs()) {
                          if (ref.kind == RefKind::LocalBranch) {
                              names << QString::fromStdString(ref.shortName);
                          }
                      }
                      return names.join(QLatin1Char(','));
                  }());
    return report;
}

QStringList MainWindow::profilesForDiagnostics(const QString& op, const QString& argument) {
    if (op == QStringLiteral("add")) {
        const QStringList parts = argument.split(QLatin1Char('|'));
        GitProfile profile;
        profile.name = parts.value(0).trimmed();
        profile.userName = parts.value(1).trimmed();
        profile.email = parts.value(2).trimmed();
        profile.signingKey = parts.value(3).trimmed();
        profile.sign = !profile.signingKey.isEmpty();
        if (!profile.name.isEmpty()) {
            profiles::save(profile);
        }
    } else if (op == QStringLiteral("remove")) {
        profiles::remove(argument.trimmed());
    } else if (op == QStringLiteral("use")) {
        const GitProfile profile = profiles::load(argument.trimmed());
        if (!profile.name.isEmpty()) {
            applyProfile(profile);
        }
    } else if (op == QStringLiteral("config")) {
        QStringList report;
        if (m_session && m_session->isOpen()) {
            report << QStringLiteral("local user.name: %1")
                          .arg(m_session->configValue(QStringLiteral("user.name")));
            report << QStringLiteral("local user.email: %1")
                          .arg(m_session->configValue(QStringLiteral("user.email")));
            report << QStringLiteral("local commit.gpgsign: %1")
                          .arg(m_session->configValue(QStringLiteral("commit.gpgsign")));
        }
        return report;
    }

    QStringList report;
    const QString active =
        m_session && m_session->isOpen() ? profiles::activeFor(repositorySettingsPath()) : QString();
    report << QStringLiteral("active: %1").arg(active);
    for (const QString& name : profiles::names()) {
        const GitProfile profile = profiles::load(name);
        report << QStringLiteral("profile: %1 <%2> key=%3 sign=%4")
                      .arg(name, profile.email, profile.signingKey,
                           profile.sign ? QStringLiteral("yes") : QStringLiteral("no"));
    }
    if (!m_session || !m_session->isOpen()) {
        return report;
    }
    report << QStringLiteral("local user.name: %1")
                  .arg(m_session->configValue(QStringLiteral("user.name")));
    report << QStringLiteral("local user.email: %1")
                  .arg(m_session->configValue(QStringLiteral("user.email")));
    return report;
}

QStringList MainWindow::worktreeForDiagnostics(const QString& operation, const QStringList& args) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }

    QString error;
    if (operation == QStringLiteral("list")) {
    } else if (operation == QStringLiteral("add")) {
        if (args.size() < 2) {
            report << QStringLiteral("add needs <path> <branch> [start]");
            return report;
        }
        const QString path = args.at(0);
        const QString branch = args.at(1);
        const QString start = args.size() >= 3 ? args.at(2) : QString();
        // A branch that does not exist yet is created (`-b`) at `start`; one that
        // does is checked out as it is. Deciding from the refs keeps the command
        // line to what a reader would type.
        bool exists = false;
        for (const RefInfo& ref : m_session->refs()) {
            if (ref.kind == RefKind::LocalBranch &&
                QString::fromStdString(ref.shortName) == branch) {
                exists = true;
                break;
            }
        }
        if (!m_session->addWorktree(path, branch, !exists, start, &error)) {
            report << QStringLiteral("error: %1").arg(error);
            return report;
        }
        refreshPanels();
    } else if (operation == QStringLiteral("remove")) {
        if (args.isEmpty()) {
            report << QStringLiteral("remove needs <path> [force]");
            return report;
        }
        if (!m_session->removeWorktree(args.at(0), args.contains(QStringLiteral("force")), &error)) {
            report << QStringLiteral("error: %1").arg(error);
            return report;
        }
        refreshPanels();
    } else if (operation == QStringLiteral("lock")) {
        if (args.isEmpty()) {
            report << QStringLiteral("lock needs <path> [reason]");
            return report;
        }
        const QString reason = args.size() >= 2 ? args.at(1) : QString();
        if (!m_session->lockWorktree(args.at(0), reason, &error)) {
            report << QStringLiteral("error: %1").arg(error);
            return report;
        }
        refreshPanels();
    } else if (operation == QStringLiteral("unlock")) {
        if (args.isEmpty()) {
            report << QStringLiteral("unlock needs <path>");
            return report;
        }
        if (!m_session->unlockWorktree(args.at(0), &error)) {
            report << QStringLiteral("error: %1").arg(error);
            return report;
        }
        refreshPanels();
    } else if (operation == QStringLiteral("prune")) {
        if (!m_session->pruneWorktrees(&error)) {
            report << QStringLiteral("error: %1").arg(error);
            return report;
        }
        refreshPanels();
    } else if (operation == QStringLiteral("open")) {
        if (args.isEmpty()) {
            report << QStringLiteral("open needs <path>");
            return report;
        }
        openWorktree(args.at(0));
    } else {
        report << QStringLiteral("unknown worktree operation: %1").arg(operation);
        return report;
    }

    const std::vector<WorktreeEntry> list = m_session->worktrees();
    report << QStringLiteral("worktrees: %1").arg(list.size());
    for (const WorktreeEntry& worktree : list) {
        const QString branch =
            worktree.branch.empty()
                ? (worktree.detached ? QStringLiteral("(detached)") : QStringLiteral("(none)"))
                : QString::fromStdString(worktree.branch);
        QString line = QStringLiteral("%1 | branch=%2 | %3")
                           .arg(QString::fromStdString(worktree.path), branch,
                                worktree.current ? QStringLiteral("current") : QStringLiteral("-"));
        if (worktree.locked) {
            line += QStringLiteral(" | locked");
        }
        if (worktree.prunable) {
            line += QStringLiteral(" | prunable");
        }
        report << line;
    }
    report << QStringLiteral("branch: %1")
                  .arg(QString::fromStdString(m_session->info().headBranch));
    return report;
}

QStringList MainWindow::submoduleForDiagnostics(const QString& operation,
                                                const QStringList& args) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    // `--recursive` is the default; `no-recursive` turns it off, which is the only
    // way to ask for the top level alone.
    const bool recursive = !args.contains(QStringLiteral("no-recursive"));
    const auto fail = [&](const QString& error) {
        report << QStringLiteral("error: %1").arg(error);
    };

    QString error;
    if (operation == QStringLiteral("list")) {
    } else if (operation == QStringLiteral("add")) {
        if (args.size() < 2) {
            report << QStringLiteral("add needs <url> <path> [branch]");
            return report;
        }
        const QString branch = args.size() >= 3 ? args.at(2) : QString();
        if (!m_session->addSubmodule(args.at(0), args.at(1), branch, false, &error)) {
            fail(error);
            return report;
        }
        refreshPanels();
    } else if (operation == QStringLiteral("init")) {
        if (args.isEmpty()) {
            report << QStringLiteral("init needs <path>");
            return report;
        }
        if (!m_session->updateSubmodules(QStringList{args.at(0)}, true, false, recursive, &error)) {
            fail(error);
            return report;
        }
        refreshPanels();
    } else if (operation == QStringLiteral("update")) {
        if (args.isEmpty()) {
            report << QStringLiteral("update needs <path> [remote] [no-recursive]");
            return report;
        }
        const bool remote = args.contains(QStringLiteral("remote"));
        if (!m_session->updateSubmodules(QStringList{args.at(0)}, true, remote, recursive,
                                         &error)) {
            fail(error);
            return report;
        }
        refreshPanels();
    } else if (operation == QStringLiteral("update-all")) {
        if (!m_session->updateSubmodules({}, true, false, recursive, &error)) {
            fail(error);
            return report;
        }
        refreshPanels();
    } else if (operation == QStringLiteral("sync")) {
        if (args.isEmpty()) {
            report << QStringLiteral("sync needs <path> [no-recursive]");
            return report;
        }
        if (!m_session->syncSubmodules(QStringList{args.at(0)}, recursive, &error)) {
            fail(error);
            return report;
        }
        refreshPanels();
    } else if (operation == QStringLiteral("sync-all")) {
        if (!m_session->syncSubmodules({}, recursive, &error)) {
            fail(error);
            return report;
        }
        refreshPanels();
    } else if (operation == QStringLiteral("autoupdate")) {
        const QString what = args.isEmpty() ? QStringLiteral("status") : args.at(0);
        if (what == QStringLiteral("enable")) {
            setSubmoduleAutoupdate(true);
        } else if (what == QStringLiteral("disable")) {
            setSubmoduleAutoupdate(false);
        } else if (what == QStringLiteral("remote-on")) {
            setSubmoduleAutoupdateRemote(true);
        } else if (what == QStringLiteral("remote-off")) {
            setSubmoduleAutoupdateRemote(false);
        } else if (what != QStringLiteral("status") && what != QStringLiteral("run")) {
            report << QStringLiteral("autoupdate takes enable|disable|remote-on|remote-off|status|run");
            return report;
        }
        report << QStringLiteral("autoupdate: %1, remote: %2")
                      .arg(m_submoduleAutoupdate ? QStringLiteral("on") : QStringLiteral("off"),
                           m_submoduleAutoupdateRemote ? QStringLiteral("on")
                                                       : QStringLiteral("off"));
        if (what == QStringLiteral("run")) {
            // The body of the hook, run synchronously so a headless check sees the
            // result; the hook itself runs the same call on a worker.
            if (!m_session->updateSubmodules({}, true, m_submoduleAutoupdateRemote, recursive,
                                             &error)) {
                fail(error);
                return report;
            }
            refreshSubmodules();
            refreshPanels();
            report << QStringLiteral("updated every submodule");
        }
    } else if (operation == QStringLiteral("deinit")) {
        if (args.isEmpty()) {
            report << QStringLiteral("deinit needs <path> [force]");
            return report;
        }
        if (!m_session->deinitSubmodule(args.at(0), args.contains(QStringLiteral("force")),
                                        &error)) {
            fail(error);
            return report;
        }
        refreshPanels();
    } else if (operation == QStringLiteral("open")) {
        if (args.isEmpty()) {
            report << QStringLiteral("open needs <path>");
            return report;
        }
        openSubmodule(args.at(0));
    } else {
        report << QStringLiteral("unknown submodule operation: %1").arg(operation);
        return report;
    }

    const std::vector<SubmoduleEntry> list = m_session->submodules();
    report << QStringLiteral("submodules: %1").arg(list.size());
    for (const SubmoduleEntry& submodule : list) {
        QString line =
            QStringLiteral("%1 | %2 | url=%3 | recorded=%4 | checkedout=%5")
                .arg(QString::fromStdString(submodule.path),
                     QString::fromUtf8(toString(submodule.state)),
                     submodule.url.empty() ? QStringLiteral("-")
                                           : QString::fromStdString(submodule.url),
                     QString::fromStdString(submodule.recordedOid).left(10),
                     submodule.checkedOutOid.empty()
                         ? QStringLiteral("-")
                         : QString::fromStdString(submodule.checkedOutOid).left(10));
        if (submodule.nested) {
            line += QStringLiteral(" | nested:%1").arg(QString::fromStdString(submodule.parentPath));
        }
        report << line;
    }
    return report;
}

QStringList MainWindow::navigateForDiagnostics(int row, const QString& direction) {
    selectCommitRow(row);
    const QString startedAt = m_commitView->navigate(QStringLiteral("start"));
    const QString landed = m_commitView->navigate(direction);
    QStringList report;
    report << QStringLiteral("from row %1: %2").arg(row).arg(startedAt.left(8));
    report << QStringLiteral("%1 -> %2")
                  .arg(direction, landed.isEmpty() ? QStringLiteral("nowhere") : landed.left(8));
    return report;
}

QStringList MainWindow::runCommitMenuEntry(const QString& key, const QString& revision) {
    logBadgeFlow(QStringLiteral("commit-op: %1 on %2").arg(key, revision));

    // Amending rewrites HEAD, so it takes the new message where the others take a
    // commit, and there is nothing to resolve first.
    if (key == QLatin1String("amend-message")) {
        amendSelectedCommitMessage(revision);
        QStringList amended;
        amended << QStringLiteral("status: %1")
                       .arg(statusBar() ? statusBar()->currentMessage() : QStringLiteral("?"));
        amended << QStringLiteral("head: %1")
                       .arg(QString::fromStdString(m_session->info().headId).left(8));
        bool hasMore = false;
        const std::vector<CommitInfo> head = m_session->commitsBlocking(0, 1, &hasMore, {});
        amended << QStringLiteral("message: %1")
                       .arg(head.empty() ? QStringLiteral("?")
                                         : QString::fromStdString(head.front().summary));
        return amended;
    }

    // Anything git understands: a branch, a tag, a short hash, HEAD~1. The window does
    // not spell out revisions itself, or it would disagree with git about them.
    const QString commitId = m_session->resolveRevision(revision);
    logBadgeFlow(QStringLiteral("commit-op: %1 resolved to %2").arg(key, commitId.left(8)));

    if (key == QLatin1String("checkout-detached")) {
        QString error;
        if (!m_session->checkoutCommit(commitId, &error)) {
            reportFailure(QStringLiteral("Cannot check out"), error);
        } else {
            refreshPanels();
            maybeAutoUpdateSubmodules();
        }
    } else if (key == QLatin1String("cherry-pick")) {
        cherryPickCommit(commitId);
    } else if (key == QLatin1String("reset-soft")) {
        resetCurrentBranchTo(commitId, ResetMode::Soft);
    } else if (key == QLatin1String("reset-mixed")) {
        resetCurrentBranchTo(commitId, ResetMode::Mixed);
    } else if (key == QLatin1String("reset-hard")) {
        resetCurrentBranchTo(commitId, ResetMode::Hard);
    } else if (key == QLatin1String("revert")) {
        revertCommit(commitId);
    } else if (key == QLatin1String("drop")) {
        dropCommitFromHistory(commitId);
    } else if (key == QLatin1String("squash")) {
        rewriteCommitInHistory(commitId, RewriteAction::Squash);
    } else if (key == QLatin1String("fixup")) {
        rewriteCommitInHistory(commitId, RewriteAction::Fixup);
    } else if (key == QLatin1String("reword")) {
        rewriteCommitInHistory(commitId, RewriteAction::Reword);
    } else if (key == QLatin1String("amend-message")) {
        amendSelectedCommitMessage(revision);
    } else {
        reportFailure(QStringLiteral("Unknown entry"), key);
    }

    QStringList report;
    report << QStringLiteral("status: %1")
                  .arg(statusBar() ? statusBar()->currentMessage() : QStringLiteral("?"));
    report << QStringLiteral("head: %1")
                  .arg(QString::fromStdString(m_session->info().headId).left(8));
    report << QStringLiteral("branch: %1@%2")
                  .arg(QString::fromStdString(m_session->info().headBranch),
                       QString::fromStdString(m_session->info().headId).left(8));
    report << QStringLiteral("operation open: %1")
                  .arg(m_session->rebaseInProgress() || m_session->mergeInProgress()
                           ? QStringLiteral("yes")
                           : QStringLiteral("no"));
    report << QStringLiteral("conflicts: %1").arg(m_session->conflicts().size());
    report << QStringLiteral("page: %1")
                  .arg(m_stack && m_stack->currentWidget() == m_conflictView
                           ? QStringLiteral("CONFLICT")
                           : QStringLiteral("history"));
    return report;
}

std::string MainWindow::currentBranchName() const {
    return m_session->isOpen() ? m_session->info().headBranch : std::string();
}

QStringList MainWindow::rebaseForDiagnostics(const QString& branch, const QString& onto) {
    QStringList report;
    if (!m_session->isOpen()) {
        report << QStringLiteral("no repository");
        return report;
    }

    rebaseBranchOnto(branch, onto);

    report << QStringLiteral("status: %1")
                  .arg(statusBar() ? statusBar()->currentMessage() : QStringLiteral("?"));
    const bool conflictPage = m_stack && m_stack->currentIndex() == kConflictsPage;
    report << QStringLiteral("centre page: %1")
                  .arg(conflictPage ? QStringLiteral("CONFLICT") : QStringLiteral("history"));
    report << QStringLiteral("rebase in progress: %1")
                  .arg(m_session->rebaseInProgress() ? QStringLiteral("yes")
                                                     : QStringLiteral("no"));
    report << QStringLiteral("conflicts: %1").arg(m_session->conflicts().size());
    for (const ConflictEntry& entry : m_session->conflicts()) {
        report << QStringLiteral("  conflicted: %1").arg(QString::fromStdString(entry.path));
    }
    report << QStringLiteral("centre is the graph: %1")
                  .arg(m_centerStack && m_centerStack->currentWidget() == m_commitView
                           ? QStringLiteral("yes")
                           : QStringLiteral("no"));
    return report;
}

QStringList MainWindow::commitPanelForDiagnostics(const QStringList& paths, const QString& message) {
    QStringList report;
    if (!m_session->isOpen()) {
        report << QStringLiteral("no repository");
        return report;
    }

    if (paths.isEmpty()) {
        QString error;
        if (!m_session->stageAll(&error)) {
            report << QStringLiteral("stage all failed: %1").arg(error);
            return report;
        }
        report << QStringLiteral("staged all changes");
    } else {
        applyToFiles(paths, StageOp::Stage);
        report << QStringLiteral("staged %1 path(s)").arg(paths.size());
    }

    commitStaged(message);
    report << QStringLiteral("status: %1")
                  .arg(statusBar() ? statusBar()->currentMessage() : QStringLiteral("?"));
    report << QStringLiteral("head: %1").arg(QString::fromStdString(m_session->info().headId));
    report << QStringLiteral("remaining changes: %1").arg(m_session->status().size());
    return report;
}

void MainWindow::rebaseBranchOnto(const QString& branch, const QString& onto,
                                  bool preflightShown) {
    logBadgeFlow(QStringLiteral("window: rebaseBranchOnto %1 onto %2").arg(branch, onto));
    // An operation already open is not a reason to start another one. git refuses, the
    // reader gets a dialog about a rebase they cannot see, and the work actually waiting
    // for them — the conflicted files on the conflict page — stays hidden behind it.
    // Take them to that page instead, where the operation they started is asking.
    if (m_session->isOpen() && (m_session->rebaseInProgress() || m_session->mergeInProgress() ||
                                m_session->squashInProgress())) {
        const bool rebasing = m_session->rebaseInProgress();
        showConflictsPage();
        refreshPanels();
        setStatus(QStringLiteral("A %1 is already in progress — finish it or abort it before starting another.")
                      .arg(rebasing ? QStringLiteral("rebase") : QStringLiteral("merge")));
        return;
    }


    // A rebase replays `branch` onto `onto`; the conflicts it would meet are the
    // ones merging the two would, so merge-tree predicts them without replaying. The
    // drop list already showed the prediction, so the modal is skipped there.
    if (!preflightShown && !confirmPreflight(onto, branch)) {
        setStatus(QStringLiteral("Rebase cancelled — conflicts were predicted."));
        return;
    }

    RebaseResult result;
    QString error;
    if (!m_session->rebaseOnto(branch, onto, &result, &error)) {
        QMessageBox::warning(this, QStringLiteral("Rebase failed"), error);
        return;
    }

    m_commitModel->reset();
    refreshPanels();

    switch (result.status) {
    case RebaseStatus::UpToDate:
        setStatus(QStringLiteral("%1 is already based on %2 — nothing to replay.")
                      .arg(branch, onto));
        break;
    case RebaseStatus::Completed:
        if (result.commitsSkipped > 0) {
            setStatus(QStringLiteral("Rebased %1 onto %2 — %3 replayed, %4 already in %2.")
                          .arg(branch, onto)
                          .arg(result.commitsApplied)
                          .arg(result.commitsSkipped));
        } else {
            setStatus(QStringLiteral("Rebased %1 onto %2 — %3 commit(s) replayed.")
                          .arg(branch, onto)
                          .arg(result.commitsApplied));
        }
        break;
    case RebaseStatus::Conflicted:
        sweepConflictsAfterOperation(QStringLiteral("%1 conflict(s) while rebasing %2 onto %3")
                                         .arg(result.conflictCount)
                                         .arg(branch, onto));
        break;
    }

    // A completed rebase may check out commits that record other submodule pointers.
    if (result.status == RebaseStatus::Completed) {
        maybeAutoUpdateSubmodules();
    }
}

void MainWindow::squashMergeBranch(const QString& source, const QString& target,
                                   bool preflightShown) {
    logBadgeFlow(QStringLiteral("window: squashMergeBranch %1 into %2").arg(source, target));
    if (!m_session->isOpen()) {
        return;
    }
    if (m_session->rebaseInProgress() || m_session->mergeInProgress() ||
        m_session->squashInProgress()) {
        const bool rebasing = m_session->rebaseInProgress();
        showConflictsPage();
        refreshPanels();
        setStatus(QStringLiteral("A %1 is already in progress — finish it or abort it before starting another.")
                      .arg(rebasing ? QStringLiteral("rebase") : QStringLiteral("operation")));
        return;
    }

    // A squash meets exactly the conflicts a merge would, so the same prediction applies.
    if (!preflightShown && !confirmPreflight(source, target)) {
        setStatus(QStringLiteral("Squash cancelled — conflicts were predicted."));
        return;
    }

    const QString current = QString::fromStdString(m_session->info().headBranch);
    if (current != target) {
        QString error;
        if (!m_session->checkoutBranch(target, &error)) {
            QMessageBox::warning(this, QStringLiteral("Cannot switch to %1").arg(target), error);
            return;
        }
    }

    SquashResult result;
    QString error;
    if (!m_session->squashMerge(source, &result, &error)) {
        QMessageBox::warning(this, QStringLiteral("Squash failed"), error);
        return;
    }

    m_commitModel->reset();
    refreshPanels();

    switch (result.status) {
    case SquashStatus::UpToDate:
        setStatus(QStringLiteral("Already up to date — nothing to squash from %1.").arg(source));
        break;
    case SquashStatus::Staged:
        // Deliberately no commit: a squash stages the result and lets the
        // reader write the message. The changes are in the commit panel.
        setStatus(QStringLiteral("Squashed %1 into %2 — changes are staged; commit when ready.")
                      .arg(source, target));
        break;
    case SquashStatus::Conflicted:
        sweepConflictsAfterOperation(
            QStringLiteral("%1 conflict(s) from squashing %2").arg(result.conflictCount).arg(source));
        break;
    }
}

void MainWindow::squashMergeIntoCurrent() {
    if (!m_session->isOpen()) {
        return;
    }
    const QString current = QString::fromStdString(m_session->info().headBranch);

    QStringList candidates;
    for (const RefInfo& ref : m_session->refs()) {
        if (ref.kind != RefKind::LocalBranch) {
            continue;
        }
        const QString name = QString::fromStdString(ref.shortName);
        if (name != current) {
            candidates << name;
        }
    }
    if (candidates.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Squash merge"),
                                 QStringLiteral("There is no other local branch to squash."));
        return;
    }

    bool ok = false;
    const QString source = QInputDialog::getItem(
        this, QStringLiteral("Squash merge"),
        QStringLiteral("Squash branch into “%1”:").arg(current), candidates, 0, false, &ok);
    if (!ok || source.isEmpty()) {
        return;
    }
    squashMergeBranch(source, current);
}

QPoint MainWindow::dropMenuPosition() const {
    // Where the drop was, not where the drag began: QCursor::pos() can still report
    // the starting point while a drag is in flight after the drop.
    const QPoint dropped = lastDropPoint();
    return dropped.isNull() ? QCursor::pos() : dropped;
}

void MainWindow::onRefDroppedOnRef(RefModel::DraggedRef source, QString target) {
    if (!m_session->isOpen()) {
        return;
    }
    if (source.kind != RefKind::LocalBranch) {
        QMessageBox::information(
            this, QStringLiteral("Drag"),
            QStringLiteral("Only local branches can be dragged onto another branch for now."));
        return;
    }
    if (source.shortName == target) {
        return;
    }

    // The drop point is where the question belongs: the drag already said what
    // the user wants to act on, and the menu only supplies the verb. A modal
    // dialog in the middle of the window loses that connection.
    QMenu menu(this);
    // The same prediction the badge list shows: dropping here and dropping on the
    // badge must not tell the reader different things.
    bool warns = false;
    const QString preview = dropPreviewText(source.shortName, target, &warns);
    if (!preview.isEmpty()) {
        QAction* note = menu.addAction(preview);
        note->setEnabled(false);
        menu.addSeparator();
    }
    const auto merge =
        menu.addAction(QStringLiteral("Merge %1 into %2").arg(source.shortName, target));
    const auto rebase =
        menu.addAction(QStringLiteral("Rebase %1 onto %2").arg(source.shortName, target));
    const auto squash =
        menu.addAction(QStringLiteral("Squash %1 into %2").arg(source.shortName, target));
    const auto interactive =
        menu.addAction(QStringLiteral("Interactive Rebase %1 onto %2").arg(source.shortName, target));
    const QString interactiveBase = m_session->mergeBase(source.shortName, target);
    interactive->setEnabled(!interactiveBase.isEmpty());
    if (interactiveBase.isEmpty()) {
        interactive->setToolTip(QStringLiteral("%1 and %2 share no commit to rebase from")
                                    .arg(source.shortName, target));
    }
    menu.addSeparator();
    const auto checkout = menu.addAction(QStringLiteral("Check out %1").arg(target));

    QAction* chosen = menu.exec(dropMenuPosition());
    if (!chosen) {
        return;
    }
    if (chosen == merge) {
        mergeBranchInto(source.shortName, target, true);
    } else if (chosen == rebase) {
        rebaseBranchOnto(source.shortName, target, true);
    } else if (chosen == squash) {
        squashMergeBranch(source.shortName, target, true);
    } else if (chosen == interactive) {
        interactiveRebaseRange(interactiveBase, source.shortName, target, source.shortName);
    } else if (chosen == checkout) {
        checkoutLocalBranch(target);
    }
}

void MainWindow::onRefDroppedOnCommit(RefModel::DraggedRef source, QString commitId) {
    if (!m_session->isOpen()) {
        return;
    }
    if (isStashCommit(commitId)) {
        setStatus(QStringLiteral("A stash is not a place for a branch — apply or pop it instead."));
        return;
    }

    const bool isBranch = source.kind == RefKind::LocalBranch;

    QMenu menu(this);
    const auto createHere = menu.addAction(QStringLiteral("Create a new branch here…"));
    QAction* moveHere = nullptr;
    if (isBranch) {
        moveHere = menu.addAction(QStringLiteral("Move %1 to this commit").arg(source.shortName));
    }
    menu.addSeparator();
    const auto checkoutHere =
        menu.addAction(QStringLiteral("Check out this commit (detached HEAD)"));

    QAction* chosen = menu.exec(dropMenuPosition());
    if (!chosen) {
        return;
    }

    if (chosen == createHere) {
        bool named = false;
        const QString name =
            QInputDialog::getText(this, QStringLiteral("New branch"),
                                  QStringLiteral("Branch name:"), QLineEdit::Normal,
                                  source.shortName == QStringLiteral("HEAD") ? QString() : source.shortName,
                                  &named);
        if (!named || name.trimmed().isEmpty()) {
            return;
        }
        runWrite(QStringLiteral("Cannot create branch"), [&](QString* error) {
            return m_session->createBranch(name.trimmed(), commitId, error);
        }, false);
        refreshRefs();
        return;
    }

    if (moveHere && chosen == moveHere) {
        const QString current = QString::fromStdString(m_session->info().headBranch);
        if (source.shortName == current) {
            const auto answer = QMessageBox::question(
                this, QStringLiteral("Move the current branch"),
                QStringLiteral("Moving “%1” while it is checked out resets the working tree to "
                               "that commit. Uncommitted changes will be lost. Continue?")
                    .arg(source.shortName));
            if (answer != QMessageBox::Yes) {
                return;
            }
            // Confirmed, so the move may discard the working tree's uncommitted
            // work: that is what the warning said it would do.
            runWrite(QStringLiteral("Cannot move branch"), [&](QString* error) {
                return m_session->moveBranchForced(source.shortName, commitId, error);
            });
            return;
        }
        runWrite(QStringLiteral("Cannot move branch"), [&](QString* error) {
            return m_session->moveBranch(source.shortName, commitId, error);
        });
        return;
    }

    if (chosen == checkoutHere) {
        runCheckout(QStringLiteral("Cannot check out the commit"), commitId.left(7),
                    [commitId](RepoSession& s, QString* error) {
                        return s.checkoutCommit(commitId, error);
                    },
                    [this] { maybeAutoUpdateSubmodules(); });
    }
}

void MainWindow::updateSyncActions() {
    const bool usable = m_session->isOpen() && !m_remotes.empty() && !m_syncRunning &&
                        !m_session->info().bare;
    if (m_fetchAction) {
        m_fetchAction->setEnabled(usable);
    }
    if (m_pullAction) {
        m_pullAction->setEnabled(usable);
    }
    if (m_pushAction) {
        m_pushAction->setEnabled(usable);
    }
    if (m_forcePushAction) {
        m_forcePushAction->setEnabled(usable);
    }
}

QString MainWindow::currentSyncRemote() const {
    if (m_remotePicker && m_remotePicker->count() > 0) {
        return m_remotePicker->currentText();
    }
    return m_remotes.empty() ? QString() : QString::fromStdString(m_remotes.front().name);
}

void MainWindow::updateRemotePicker() {
    if (!m_remotePicker) {
        return;
    }
    const QString previous = m_remotePicker->currentText();
    {
        const QSignalBlocker block(m_remotePicker);
        m_remotePicker->clear();
        for (const RemoteInfo& remote : m_remotes) {
            m_remotePicker->addItem(QString::fromStdString(remote.name));
        }
    }
    // Keep the reader's choice while it still exists; otherwise prefer the remote
    // the current branch already follows, then `origin`, then the first one.
    QString want = previous;
    if (want.isEmpty() || m_remotePicker->findText(want) < 0) {
        const UpstreamInfo upstream = m_session->upstreamInfo();
        const QString upstreamRemote =
            upstream.configured
                ? QString::fromStdString(upstream.name).section(QLatin1Char('/'), 0, 0)
                : QString();
        if (!upstreamRemote.isEmpty() && m_remotePicker->findText(upstreamRemote) >= 0) {
            want = upstreamRemote;
        } else if (m_remotePicker->findText(QStringLiteral("origin")) >= 0) {
            want = QStringLiteral("origin");
        } else if (m_remotePicker->count() > 0) {
            want = m_remotePicker->itemText(0);
        } else {
            want.clear();
        }
    }
    if (!want.isEmpty()) {
        m_remotePicker->setCurrentIndex(m_remotePicker->findText(want));
    }
    m_remotePicker->setVisible(!m_remotes.empty());
    updateAutoFetchMenu();
}

void MainWindow::setPullMode(PullMode mode) {
    m_pullMode = mode;
    if (m_pullButton && m_pullButton->menu()) {
        for (QAction* entry : m_pullButton->menu()->actions()) {
            entry->setChecked(entry->data().toInt() == static_cast<int>(mode));
        }
    }
    if (m_pullAction) {
        const QString modeName = mode == PullMode::FfOnly ? QStringLiteral("--ff-only")
                                 : mode == PullMode::Merge ? QStringLiteral("(merge)")
                                                           : QStringLiteral("--rebase");
        m_pullAction->setToolTip(
            QStringLiteral("git pull %1 (the arrow chooses the mode)").arg(modeName));
    }
}

void MainWindow::setUpstreamInteractive() {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    const RepoInfo info = m_session->info();
    if (info.headBranch.empty() || info.detached) {
        setStatus(QStringLiteral("Set Upstream needs a branch to be checked out."));
        return;
    }
    const QString current = QString::fromStdString(info.headBranch);
    QStringList candidates;
    for (const RefInfo& ref : m_session->refs()) {
        if (ref.kind == RefKind::RemoteBranch) {
            candidates << QString::fromStdString(ref.shortName);
        }
    }
    QString preselect;
    const QString remote = currentSyncRemote();
    if (!remote.isEmpty()) {
        const QString guess = remote + QLatin1Char('/') + current;
        if (candidates.contains(guess)) {
            preselect = guess;
        }
    }
    bool accepted = false;
    const QString upstream = QInputDialog::getItem(
        this, QStringLiteral("Set upstream"),
        QStringLiteral("Remote-tracking branch for “%1”:").arg(current), candidates,
        candidates.indexOf(preselect), true, &accepted);
    if (!accepted || upstream.trimmed().isEmpty()) {
        return;
    }
    runWrite(QStringLiteral("Cannot set upstream"),
             [&](QString* error) {
                 return m_session->setUpstream(current, upstream.trimmed(), error);
             },
             false);
    setStatus(QStringLiteral("%1 now follows %2.").arg(current, upstream.trimmed()));
}

void MainWindow::pushTagToRemote(const QString& tag) {
    if (!m_session || !m_session->isOpen() || tag.isEmpty()) {
        return;
    }
    if (m_syncRunning) {
        setStatus(QStringLiteral("A network operation is already running."));
        return;
    }
    const QString remote = currentSyncRemote();
    if (remote.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("No remote"),
                                 QStringLiteral("This repository has no remote configured."));
        return;
    }
    m_syncRunning = true;
    updateSyncActions();
    setStatus(QStringLiteral("Pushing tag %1 to %2…").arg(tag, remote));
    m_session->requestPushTag(remote, tag);
}

void MainWindow::pushAllTags() {
    startSync(SyncKind::PushTags);
}

void MainWindow::deleteTagInteractive(const QString& tag) {
    if (!m_session || !m_session->isOpen() || tag.isEmpty()) {
        return;
    }
    if (!askYes(QStringLiteral("Delete tag"),
                QStringLiteral("Delete tag “%1”?\nThe commit it points at stays.").arg(tag))) {
        return;
    }
    runWrite(QStringLiteral("Cannot delete tag"),
             [&](QString* error) { return m_session->deleteTag(tag, error); });
    setStatus(QStringLiteral("Tag %1 deleted.").arg(tag));
}

void MainWindow::deleteTagFromRemotesInteractive(const QString& tag) {
    if (!m_session || !m_session->isOpen() || tag.isEmpty()) {
        return;
    }
    const std::vector<RemoteInfo> remotes = m_session->remotes();
    if (remotes.empty()) {
        QMessageBox::information(this, QStringLiteral("No remote"),
                                 QStringLiteral("This repository has no remote configured."));
        return;
    }
    QStringList names;
    for (const RemoteInfo& remote : remotes) {
        names << QString::fromStdString(remote.name);
    }
    if (!askYes(QStringLiteral("Delete tag from all remotes"),
                QStringLiteral("Delete tag “%1” on every remote (%2)?\nThe local tag and the "
                               "commit it points at stay.")
                    .arg(tag, names.join(QStringLiteral(", "))))) {
        return;
    }

    QStringList failed;
    for (const QString& name : names) {
        const SyncResult result = m_session->deleteRemoteTagBlocking(name, tag);
        if (!result.ok) {
            failed << name;
        }
    }
    if (failed.isEmpty()) {
        setStatus(QStringLiteral("Tag %1 deleted on %2.").arg(tag, names.join(QStringLiteral(", "))));
    } else {
        reportFailure(QStringLiteral("Delete tag on remote"),
                      QStringLiteral("Could not delete “%1” on: %2")
                          .arg(tag, failed.join(QStringLiteral(", "))));
    }
}

void MainWindow::forcePushInteractive() {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    if (m_syncRunning) {
        setStatus(QStringLiteral("A network operation is already running."));
        return;
    }
    if (m_remotes.empty()) {
        QMessageBox::information(this, QStringLiteral("No remote"),
                                 QStringLiteral("This repository has no remote configured."));
        return;
    }
    const RepoInfo info = m_session->info();
    if (info.headBranch.empty() || info.detached) {
        setStatus(QStringLiteral("Force push needs a branch to be checked out."));
        return;
    }
    const QString branch = QString::fromStdString(info.headBranch);
    const QString remote = currentSyncRemote();
    // The lease is the point: this refuses the moment somebody else has pushed,
    // rather than discarding work we never read. Say so rather than just "force".
    if (!askYes(QStringLiteral("Force push"),
                QStringLiteral("Rewrite “%1” on %2 with --force-with-lease?\n"
                               "git will refuse if the remote has moved since the last fetch.")
                    .arg(branch, remote))) {
        return;
    }
    startSync(SyncKind::PushForce);
}

void MainWindow::onAutoFetchTick() {
    if (!m_session || !m_session->isOpen() || m_session->info().bare || m_remotes.empty() ||
        m_syncRunning) {
        return;
    }
    // A remote removed since the setting was written must not make every tick fail.
    QStringList remotes = m_autoFetchRemotes;
    remotes.removeIf([this](const QString& name) {
        return std::none_of(m_remotes.begin(), m_remotes.end(), [&](const RemoteInfo& remote) {
            return QString::fromStdString(remote.name) == name;
        });
    });
    m_silentSync = true;
    m_syncRunning = true;
    updateSyncActions();
    setStatus(QStringLiteral("Auto-fetch…"));
    m_session->requestFetch(remotes, /*prune=*/true);
}

void MainWindow::setAutoFetch(bool enabled) {
    m_autoFetchEnabled = enabled;
    if (m_autoFetchAction && m_autoFetchAction->isChecked() != enabled) {
        const QSignalBlocker block(m_autoFetchAction);
        m_autoFetchAction->setChecked(enabled);
    }
    saveAutoFetch();
    if (enabled && m_session->isOpen() && !m_remotes.empty()) {
        m_autoFetchTimer->start();
    } else {
        m_autoFetchTimer->stop();
    }
}

void MainWindow::setAutoFetchRemotes(const QStringList& remotes) {
    // Keep only remote names that exist. Selecting every one is the same thing as
    // "all", and is stored as the empty list so the default and the choice agree.
    QStringList kept;
    for (const RemoteInfo& remote : m_remotes) {
        const QString name = QString::fromStdString(remote.name);
        if (remotes.contains(name)) {
            kept << name;
        }
    }
    m_autoFetchRemotes =
        (kept.size() == static_cast<int>(m_remotes.size())) ? QStringList{} : kept;
    saveAutoFetch();
    updateAutoFetchMenu();
}

void MainWindow::setAutoFetchIntervalMs(int ms) {
    if (m_autoFetchTimer) {
        m_autoFetchTimer->setInterval(ms);
    }
}

void MainWindow::updateAutoFetchMenu() {
    if (!m_autoFetchMenu || !m_autoFetchSeparator) {
        return;
    }
    // Drop the previous dynamic entries: everything after the separator. deleteLater,
    // not delete, because this can run from inside one of those very actions.
    bool pastSeparator = false;
    for (QAction* action : m_autoFetchMenu->actions()) {
        if (action == m_autoFetchSeparator) {
            pastSeparator = true;
            continue;
        }
        if (pastSeparator) {
            m_autoFetchMenu->removeAction(action);
            action->deleteLater();
        }
    }

    const bool all = m_autoFetchRemotes.isEmpty();
    QAction* allAction = m_autoFetchMenu->addAction(QStringLiteral("All remotes"));
    allAction->setCheckable(true);
    allAction->setChecked(all);
    allAction->setEnabled(!m_remotes.empty());
    connect(allAction, &QAction::triggered, this, [this] { setAutoFetchRemotes({}); });

    for (const RemoteInfo& remote : m_remotes) {
        const QString name = QString::fromStdString(remote.name);
        QAction* entry = m_autoFetchMenu->addAction(name);
        entry->setCheckable(true);
        entry->setChecked(!all && m_autoFetchRemotes.contains(name));
        connect(entry, &QAction::triggered, this, [this, name] {
            QStringList next = m_autoFetchRemotes;
            if (next.contains(name)) {
                next.removeAll(name);
            } else {
                next << name;
            }
            setAutoFetchRemotes(next);
        });
    }
}

void MainWindow::loadAutoFetch() {
    QSettings settings;
    const QString key = autoFetchSettingsKey();
    m_autoFetchEnabled = settings.value(key + QStringLiteral("/enabled"), false).toBool();
    m_autoFetchRemotes = settings.value(key + QStringLiteral("/remotes")).toStringList();
    if (m_autoFetchAction) {
        const QSignalBlocker block(m_autoFetchAction);
        m_autoFetchAction->setChecked(m_autoFetchEnabled);
    }
    if (m_autoFetchEnabled && m_session->isOpen() && !m_remotes.empty()) {
        m_autoFetchTimer->start();
    } else {
        m_autoFetchTimer->stop();
    }
    updateAutoFetchMenu();
}

void MainWindow::saveAutoFetch() {
    QSettings settings;
    const QString key = autoFetchSettingsKey();
    settings.setValue(key + QStringLiteral("/enabled"), m_autoFetchEnabled);
    settings.setValue(key + QStringLiteral("/remotes"), m_autoFetchRemotes);
}

void MainWindow::setPreflightEnabled(bool enabled) {
    m_preflightEnabled = enabled;
    if (m_preflightAction && m_preflightAction->isChecked() != enabled) {
        const QSignalBlocker block(m_preflightAction);
        m_preflightAction->setChecked(enabled);
    }
    savePreflight();
}

void MainWindow::loadPreflight() {
    QSettings settings;
    m_preflightEnabled = settings.value(preflightSettingsKey(), false).toBool();
    if (m_preflightAction) {
        const QSignalBlocker block(m_preflightAction);
        m_preflightAction->setChecked(m_preflightEnabled);
    }
}

void MainWindow::savePreflight() {
    QSettings settings;
    settings.setValue(preflightSettingsKey(), m_preflightEnabled);
}

void MainWindow::setPreventionEnabled(bool enabled) {
    m_preventionEnabled = enabled;
    if (m_preventionAction && m_preventionAction->isChecked() != enabled) {
        const QSignalBlocker block(m_preventionAction);
        m_preventionAction->setChecked(enabled);
    }
    savePrevention();
}

void MainWindow::loadPrevention() {
    QSettings settings;
    m_preventionEnabled = settings.value(preventionSettingsKey(), false).toBool();
    if (m_preventionAction) {
        const QSignalBlocker block(m_preventionAction);
        m_preventionAction->setChecked(m_preventionEnabled);
    }
}

void MainWindow::savePrevention() {
    QSettings settings;
    settings.setValue(preventionSettingsKey(), m_preventionEnabled);
}

void MainWindow::checkConflictsWithBase(bool silent) {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    QString base;
    QString error;
    const MergePreview preview = m_session->previewAgainstBase(&base, &error);
    if (!error.isEmpty() || base.isEmpty()) {
        // No base to measure against is not a conflict; say nothing when the check
        // ran on its own.
        if (!silent) {
            setStatus(error.isEmpty() ? QStringLiteral("No base branch to compare with.")
                                      : QStringLiteral("Could not check for conflicts: %1")
                                            .arg(error));
        }
        return;
    }

    if (!preview.available || preview.clean) {
        if (!silent) {
            setStatus(QStringLiteral("No conflicts with %1.").arg(base));
        }
        if (statusBar() && !silent) {
            statusBar()->setToolTip(QString());
        }
        return;
    }

    QStringList names;
    for (const std::string& path : preview.conflictedPaths) {
        names << describePredictedConflict(preview, path);
    }
    setStatus(QStringLiteral("%1 potential conflict(s) with %2 — see the tooltip below.")
                  .arg(names.size())
                  .arg(base));
    if (statusBar()) {
        statusBar()->setToolTip(
            QStringLiteral("Merging %1 into this branch would conflict in:\n%2")
                .arg(base, names.join(QLatin1Char('\n'))));
    }
}

QStringList MainWindow::preventionForDiagnostics(const QString& op) {
    QStringList report;
    if (op == QStringLiteral("enable") || op == QStringLiteral("disable")) {
        setPreventionEnabled(op == QStringLiteral("enable"));
        report << QStringLiteral("prevention: %1")
                      .arg(m_preventionEnabled ? QStringLiteral("on") : QStringLiteral("off"));
        return report;
    }
    if (op != QStringLiteral("check")) {
        report << QStringLiteral("unknown prevention action: %1").arg(op);
        return report;
    }

    QString base;
    QString error;
    const MergePreview preview = m_session->previewAgainstBase(&base, &error);
    report << QStringLiteral("base: %1").arg(base.isEmpty() ? QStringLiteral("(none)") : base);
    report << QStringLiteral("error: %1").arg(error.isEmpty() ? QStringLiteral("none") : error);
    report << QStringLiteral("available: %1")
                  .arg(preview.available ? QStringLiteral("yes") : QStringLiteral("no"));
    report << QStringLiteral("clean: %1")
                  .arg(preview.clean ? QStringLiteral("yes") : QStringLiteral("no"));
    for (const std::string& path : preview.conflictedPaths) {
        report << QStringLiteral("conflict: %1").arg(QString::fromStdString(path));
        report << QStringLiteral("where: %1").arg(describePredictedConflict(preview, path));
    }
    return report;
}

void MainWindow::setSubmoduleAutoupdate(bool enabled) {
    m_submoduleAutoupdate = enabled;
    if (m_submoduleAutoupdateAction && m_submoduleAutoupdateAction->isChecked() != enabled) {
        const QSignalBlocker block(m_submoduleAutoupdateAction);
        m_submoduleAutoupdateAction->setChecked(enabled);
    }
    saveSubmoduleAutoupdate();
}

void MainWindow::setSubmoduleAutoupdateRemote(bool remote) {
    m_submoduleAutoupdateRemote = remote;
    if (m_submoduleAutoupdateRemoteAction &&
        m_submoduleAutoupdateRemoteAction->isChecked() != remote) {
        const QSignalBlocker block(m_submoduleAutoupdateRemoteAction);
        m_submoduleAutoupdateRemoteAction->setChecked(remote);
    }
    saveSubmoduleAutoupdate();
}

void MainWindow::loadSubmoduleAutoupdate() {
    SettingsStore store(repositorySettingsPath());
    const QString key = store.repositoryKey(QStringLiteral("submodules"));
    m_submoduleAutoupdate = store.boolValue(key + QStringLiteral("/enabled"), false);
    m_submoduleAutoupdateRemote = store.boolValue(key + QStringLiteral("/remote"), false);
    if (m_submoduleAutoupdateAction) {
        const QSignalBlocker block(m_submoduleAutoupdateAction);
        m_submoduleAutoupdateAction->setChecked(m_submoduleAutoupdate);
    }
    if (m_submoduleAutoupdateRemoteAction) {
        const QSignalBlocker block(m_submoduleAutoupdateRemoteAction);
        m_submoduleAutoupdateRemoteAction->setChecked(m_submoduleAutoupdateRemote);
    }
}

void MainWindow::saveSubmoduleAutoupdate() {
    SettingsStore store(repositorySettingsPath());
    const QString key = store.repositoryKey(QStringLiteral("submodules"));
    store.setBool(key + QStringLiteral("/enabled"), m_submoduleAutoupdate);
    store.setBool(key + QStringLiteral("/remote"), m_submoduleAutoupdateRemote);
}

void MainWindow::openPreferences() {
    QStringList remotes;
    remotes.reserve(static_cast<int>(m_remotes.size()));
    for (const RemoteInfo& remote : m_remotes) {
        remotes << QString::fromStdString(remote.name);
    }

    RererePreferences rerere;
    if (m_session && m_session->isOpen()) {
        rerere.available = true;
        rerere.enabled = m_session->rerereEnabled();
        rerere.autoupdate = m_session->rerereAutoupdate();
    }

    ExternalToolsPreferences externalTools;
    if (m_session && m_session->isOpen()) {
        externalTools.available = true;
        externalTools.diffTool = m_session->externalTool(false);
        externalTools.mergeTool = m_session->externalTool(true);
        for (const QString& name : m_session->availableExternalTools(false)) {
            externalTools.diffTools << name;
        }
        for (const QString& name : m_session->availableExternalTools(true)) {
            externalTools.mergeTools << name;
        }
    }

    SigningPreferences signing;
    if (m_session && m_session->isOpen()) {
        signing.available = true;
        signing.sign = m_session->commitSigningEnabled();
        signing.signingKey = m_session->configValue(QStringLiteral("user.signingkey"));
        signing.format = m_session->configValue(QStringLiteral("gpg.format"));
        signing.gpgProgram = m_session->configValue(QStringLiteral("gpg.program"));
    }

    // Every bindable command, with the sequence it has now. Per-entity commands
    // (stash.apply:N, branch.<name>) are rebuilt on every refresh and carry no
    // stable name to key a shortcut by, so they are left out.
    QList<KeymapRow> keymapRows;
    for (const Command& command : m_commands.commands()) {
        if (command.id.contains(QLatin1Char(':'))) {
            continue;
        }
        keymapRows.append(KeymapRow{command.id, command.title, command.category, command.shortcut,
                                    m_keymap.effective(command.id, command.shortcut)});
    }
    // The palette and the graph's Escape are the window's own, outside the registry.
    const QStringList reserved{QStringLiteral("Ctrl+P"), QStringLiteral("Esc")};

    PreferencesDialog dialog(repositorySettingsPath(), remotes, rerere, keymapRows, reserved,
                             externalTools, signing, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    // Re-read rather than trust the dialog: QSettings is the one source of truth,
    // and the same loaders run on every repository open.
    loadGraphAnimation();
    loadGraphViewOptions();
    loadSavedAppearance();
    loadCommitDateFormat();
    loadAutoFetch();
    loadPreflight();
    loadPrevention();
    loadSubmoduleAutoupdate();
    if (rerere.available) {
        if (dialog.rerereEnabled() != rerere.enabled) {
            setRerereEnabled(dialog.rerereEnabled());
        }
        if (dialog.rerereAutoupdate() != rerere.autoupdate) {
            setRerereAutoupdate(dialog.rerereAutoupdate());
        }
    }
    if (externalTools.available) {
        if (dialog.diffTool() != externalTools.diffTool) {
            m_session->setExternalTool(false, dialog.diffTool(), nullptr);
        }
        if (dialog.mergeTool() != externalTools.mergeTool) {
            m_session->setExternalTool(true, dialog.mergeTool(), nullptr);
        }
    }
    if (signing.available) {
        if (dialog.signCommits() != signing.sign) {
            m_session->setConfigValue(QStringLiteral("commit.gpgsign"),
                                      dialog.signCommits() ? QStringLiteral("true")
                                                           : QStringLiteral("false"),
                                      nullptr);
        }
        if (dialog.signingKey() != signing.signingKey) {
            m_session->setConfigValue(QStringLiteral("user.signingkey"), dialog.signingKey(),
                                      nullptr);
        }
        if (dialog.signingFormat() != signing.format) {
            m_session->setConfigValue(QStringLiteral("gpg.format"), dialog.signingFormat(), nullptr);
        }
        if (dialog.gpgProgram() != signing.gpgProgram) {
            m_session->setConfigValue(QStringLiteral("gpg.program"), dialog.gpgProgram(), nullptr);
        }
        if (m_details) {
            m_details->setSignCommit(m_session->commitSigningEnabled());
        }
    }
    // A sequence equal to the command's own default is the same as no override.
    for (const KeymapRow& row : dialog.keymapRows()) {
        if (row.sequence == row.defaultSequence) {
            m_keymap.reset(row.id);
        } else {
            m_keymap.setOverride(row.id, row.sequence);
        }
    }
    installCommandShortcuts();
    applyAppearance();
    setStatus(QStringLiteral("Preferences saved."));
}

void MainWindow::applyAppearance() {
    applyApplicationTheme(qApp);
    if (m_toolBar) {
        m_toolBar->setIconSize(QSize(theme::iconSize(), theme::iconSize()));
    }
    // Icons are rendered from the theme's colours, and an action keeps the QIcon
    // it was built with: the registry redraws every remembered one.
    retintThemedIcons();
    if (m_details) {
        m_details->refreshTheme();
    }
    if (m_diffView) {
        m_diffView->refreshTheme();
    }
    // The reference tree bakes a glyph icon per row, so the rows are rebuilt.
    if (m_session && m_session->isOpen()) {
        refreshPanels();
    }
    if (m_conflictView) {
        m_conflictView->refreshTheme();
    }
    update();
}

void MainWindow::externalDiffPair(const QString& path, QString* base, QString* target,
                                  bool* cached) const {
    Q_UNUSED(path);
    base->clear();
    target->clear();
    *cached = false;
    if (!m_compareBase.isEmpty()) {
        *base = m_compareBase;
        if (m_compareMode != CompareMode::WorkingTree) {
            *target = m_compareTarget;
        }
        return;
    }
    if (m_details && m_details->showingWorkingTree()) {
        *cached = m_renderedStaged;
        return;
    }
    if (!m_selectedCommitId.isEmpty()) {
        const int row = m_commitModel->rowForCommit(m_selectedCommitId);
        const CommitInfo* commit = row >= 0 ? m_commitModel->commitAt(row) : nullptr;
        if (commit && !commit->parents.empty()) {
            *base = QString::fromStdString(commit->parents.front());
        } else {
            // A root commit has no parent; the empty tree names the "before" side.
            *base = QStringLiteral("4b825dc642cb6eb9a060e54bf8d69288fbee4904");
        }
        *target = m_selectedCommitId;
    }
}

void MainWindow::openExternalDiff(const QString& path) {
    if (!m_session || !m_session->isOpen() || path.isEmpty()) {
        return;
    }
    QString base;
    QString target;
    bool cached = false;
    externalDiffPair(path, &base, &target, &cached);
    // The tool runs until the reader closes it, so it belongs on a worker; the grid
    // read is on the GUI thread's terms, the subprocess is not.
    const std::shared_ptr<std::atomic<bool>> alive = m_workerAlive;
    QThreadPool::globalInstance()->start([this, alive, base, target, path, cached] {
        if (!alive->load()) {
            return;
        }
        const SyncResult result = m_session->runExternalDiffTool(base, target, path, cached);
        if (!alive->load()) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, ok = result.ok, out = QString::fromStdString(result.output), path] {
                onExternalToolFinished(ok, out, path, false);
            },
            Qt::QueuedConnection);
    });
    setStatus(QStringLiteral("Opening %1 in the external diff tool…").arg(path));
}

void MainWindow::openExternalMerge(const QString& path) {
    if (!m_session || !m_session->isOpen() || path.isEmpty()) {
        return;
    }
    const std::shared_ptr<std::atomic<bool>> alive = m_workerAlive;
    QThreadPool::globalInstance()->start([this, alive, path] {
        if (!alive->load()) {
            return;
        }
        const SyncResult result = m_session->runExternalMergeTool(path);
        if (!alive->load()) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, ok = result.ok, out = QString::fromStdString(result.output), path] {
                onExternalToolFinished(ok, out, path, true);
            },
            Qt::QueuedConnection);
    });
    setStatus(QStringLiteral("Opening %1 in the external merge tool…").arg(path));
}

void MainWindow::onExternalToolFinished(bool ok, const QString& output, const QString& title,
                                        bool refresh) {
    if (refresh) {
        // A merge tool writes a resolution into the working tree; the panels and the
        // conflict page have to re-read before the reader touches them again.
        refreshPanels();
        if (m_conflictView && m_session && m_session->isOpen()) {
            m_conflictView->reload();
        }
    }
    if (!ok) {
        reportFailure(QStringLiteral("External tool failed"), output.trimmed());
        return;
    }
    setStatus(QStringLiteral("Opened %1 in the external tool.").arg(title));
}

void MainWindow::openTerminal() {
    if (!m_session || !m_session->isOpen()) {
        setStatus(QStringLiteral("Open a repository first."));
        return;
    }
    const std::string workDir = m_session->info().workDir;
    if (workDir.empty()) {
        setStatus(QStringLiteral("This repository has no working tree."));
        return;
    }
    const TerminalLaunch launch =
        buildTerminalLaunch(workDir, qgetenv("TERMINAL").toStdString(),
                            [](const std::string& name) -> std::string {
                                return QStandardPaths::findExecutable(QString::fromStdString(name))
                                    .toStdString();
                            });
    if (!launch.valid) {
        reportFailure(QStringLiteral("No terminal emulator found"),
                      QStringLiteral("Install one, or set $TERMINAL to the program to run."));
        return;
    }
    QStringList args;
    args.reserve(static_cast<int>(launch.args.size()));
    for (const std::string& arg : launch.args) {
        args << QString::fromStdString(arg);
    }
    if (!QProcess::startDetached(QString::fromStdString(launch.program), args,
                                 QString::fromStdString(workDir))) {
        reportFailure(QStringLiteral("Cannot open a terminal"),
                      QStringLiteral("Failed to start %1.")
                          .arg(QString::fromStdString(launch.program)));
        return;
    }
    setStatus(QStringLiteral("Opened a terminal in %1").arg(QString::fromStdString(workDir)));
}

void MainWindow::openHooks() {
    if (!m_session || !m_session->isOpen()) {
        setStatus(QStringLiteral("Open a repository first."));
        return;
    }

    const std::vector<HookEntry> hooks = m_session->hooks();
    const QString hooksPath = m_session->hooksPath();
    QHash<QString, QString> contents;
    QStringList originalEnabled;
    QStringList serverSide;
    for (const HookEntry& hook : hooks) {
        const QString name = QString::fromStdString(hook.name);
        if (hook.serverSide) {
            serverSide << name;
        }
        if (hook.state == HookState::Enabled) {
            originalEnabled << name;
        }
        if (hook.state == HookState::Enabled || hook.state == HookState::Disabled) {
            contents.insert(name, m_session->hookContent(name));
        }
    }

    HooksDialog dialog(hooks, contents, hooksPath, this);
    connect(&dialog, &HooksDialog::openHooksFolderRequested, this, [hooksPath] {
        if (!hooksPath.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(hooksPath));
        }
    });
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QStringList failures;
    bool wroteAnything = false;

    const QHash<QString, QString> edited = dialog.editedContents();
    for (auto it = edited.constBegin(); it != edited.constEnd(); ++it) {
        QString error;
        if (m_session->writeHook(it.key(), it.value(), &error)) {
            wroteAnything = true;
        } else {
            failures << QStringLiteral("%1: %2").arg(it.key(), error);
        }
    }

    const QStringList desired = dialog.enabledNames();
    for (const QString& name : dialog.knownNames()) {
        if (serverSide.contains(name)) {
            // A server-side hook runs on the remote; a local switch would lie.
            continue;
        }
        const bool want = desired.contains(name);
        const bool was = originalEnabled.contains(name);
        if (want == was) {
            continue;
        }
        QString error;
        const bool ok = want ? m_session->enableHook(name, &error)
                             : m_session->disableHook(name, &error);
        if (ok) {
            wroteAnything = true;
        } else {
            failures << QStringLiteral("%1: %2").arg(name, error);
        }
    }

    if (!failures.isEmpty()) {
        reportFailure(QStringLiteral("Hooks"), failures.join(QLatin1Char('\n')));
        return;
    }
    if (wroteAnything) {
        setStatus(QStringLiteral("Hooks updated."));
    }
}

void MainWindow::cloneFromDialog() {
    CloneDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    startClone(dialog.url(), dialog.destination(), dialog.partial(), dialog.branch(),
               dialog.depth(), dialog.shallowSince());
}

void MainWindow::startClone(const QString& url, const QString& destination, bool partial,
                            const QString& branch, int depth, const QString& shallowSince) {
    if (m_cloneRunning) {
        setStatus(QStringLiteral("A clone is already running."));
        return;
    }
    m_cloneRunning = true;
    if (m_commitView) {
        m_commitView->setBusy(true);
    }
    setStatus(QStringLiteral("Cloning %1 …").arg(url));

    gk::CloneOptions options;
    options.partial = partial;
    options.branch = branch.toStdString();
    options.depth = depth;
    options.shallowSince = shallowSince.toStdString();
    const std::string source = url.toStdString();
    const std::string target = destination.toStdString();
    // The clone runs on the pool because it is a network wait; the repository does
    // not exist yet, so it cannot go through RepoSession. `alive` lets the worker
    // stop before it touches a window that is closing.
    const std::shared_ptr<std::atomic<bool>> alive = m_workerAlive;
    QThreadPool::globalInstance()->start([this, alive, source, target, options] {
        const gk::CloneResult result = gk::cloneRepository(source, target, options);
        if (!alive->load()) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, ok = result.ok, dest = QString::fromStdString(target),
             output = QString::fromStdString(result.output)] {
                onCloneFinished(ok, dest, output);
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::onCloneFinished(bool ok, const QString& destination, const QString& output) {
    m_cloneRunning = false;
    if (m_commitView) {
        m_commitView->setBusy(false);
    }
    if (!ok) {
        reportFailure(QStringLiteral("Clone failed"), lastLines(output, 6));
        return;
    }
    if (openRepository(destination)) {
        notify(NotificationLog::Success, QStringLiteral("Cloned into %1").arg(destination));
    } else {
        notify(NotificationLog::Warning,
               QStringLiteral("Cloned into %1, but it could not be opened.").arg(destination));
    }
}

void MainWindow::refreshCommands() {
    buildCommands();
    installCommandShortcuts();
}
void MainWindow::installCommandShortcuts() {
    // Later, not now: a command run from its own shortcut refreshes the window, and
    // the refresh lands here while that shortcut is still emitting.
    for (QShortcut* shortcut : m_commandShortcuts) {
        shortcut->setEnabled(false);
        shortcut->deleteLater();
    }
    m_commandShortcuts.clear();

    for (const Command& command : m_commands.commands()) {
        if (command.id.contains(QLatin1Char(':'))) {
            continue;
        }
        const QKeySequence sequence = m_keymap.effective(command.id, command.shortcut);
        if (QAction* action = m_commandActions.value(command.id, nullptr)) {
            action->setShortcut(sequence);
            continue;
        }
        if (sequence.isEmpty()) {
            continue;
        }
        auto* shortcut = new QShortcut(sequence, this);
        const std::function<void()> run = command.run;
        connect(shortcut, &QShortcut::activated, this, [run] {
            if (run) {
                run();
            }
        });
        m_commandShortcuts.insert(command.id, shortcut);
    }
}

QString MainWindow::keymapConflict(const QString& id, const QKeySequence& sequence) const {
    if (sequence.isEmpty()) {
        return {};
    }
    const QString text = sequence.toString(QKeySequence::PortableText);
    if (text == QStringLiteral("Ctrl+P") || text == QStringLiteral("Esc")) {
        return QStringLiteral("(reserved)");
    }
    for (const Command& command : m_commands.commands()) {
        if (command.id == id || command.id.contains(QLatin1Char(':'))) {
            continue;
        }
        const QKeySequence other = m_keymap.effective(command.id, command.shortcut);
        if (!other.isEmpty() && other.toString(QKeySequence::PortableText) == text) {
            return command.id;
        }
    }
    return {};
}

QString MainWindow::commandSequence(const QString& id) const {
    if (QAction* action = m_commandActions.value(id, nullptr)) {
        return action->shortcut().toString(QKeySequence::PortableText);
    }
    if (QShortcut* shortcut = m_commandShortcuts.value(id, nullptr)) {
        return shortcut->key().toString(QKeySequence::PortableText);
    }
    return {};
}

QStringList MainWindow::keymapForDiagnostics(const QString& operation, const QStringList& args) {
    QStringList report;
    if (operation == QStringLiteral("list")) {
        for (const Command& command : m_commands.commands()) {
            if (command.id.contains(QLatin1Char(':'))) {
                continue;
            }
            const QKeySequence effective = m_keymap.effective(command.id, command.shortcut);
            report << QStringLiteral("%1=%2 default=%3")
                          .arg(command.id, effective.toString(QKeySequence::PortableText),
                               command.shortcut.toString(QKeySequence::PortableText));
        }
    } else if (operation == QStringLiteral("set") && args.size() >= 2) {
        const QString id = args.at(0);
        const QKeySequence sequence(args.at(1), QKeySequence::PortableText);
        const QString conflict = keymapConflict(id, sequence);
        if (!conflict.isEmpty()) {
            report << QStringLiteral("set: %1 already uses %2").arg(conflict, args.at(1));
        } else {
            m_keymap.setOverride(id, sequence);
            installCommandShortcuts();
            report << QStringLiteral("set: %1=%2").arg(id, commandSequence(id));
        }
    } else if (operation == QStringLiteral("reset") && !args.isEmpty()) {
        m_keymap.reset(args.at(0));
        installCommandShortcuts();
        report << QStringLiteral("reset: %1=%2").arg(args.at(0), commandSequence(args.at(0)));
    } else if (operation == QStringLiteral("reset-all")) {
        m_keymap.resetAll();
        installCommandShortcuts();
        report << QStringLiteral("reset-all: ok");
    } else {
        report << QStringLiteral("usage: --ui-keymap <list|set <id> <seq>|reset <id>|reset-all>");
    }
    return report;
}

void MainWindow::revealCommit(const QString& oid) {
    if (oid.isEmpty() || !m_commitModel) {
        return;
    }
    m_revealTarget = oid;
    m_revealPages = 0;
    tryRevealCommit();
}

void MainWindow::tryRevealCommit() {
    if (m_revealTarget.isEmpty() || !m_commitModel) {
        return;
    }
    const int row = m_commitModel->rowForCommit(m_revealTarget);
    if (row >= 0) {
        showGraphPage();
        selectCommitRow(row);
        centerCommitRow(row);
        setStatus(QStringLiteral("Found %1.").arg(m_revealTarget.left(8)));
        m_revealTarget.clear();
        return;
    }
    // A page in flight makes canLoadMore() false without meaning "no more"; only a
    // loaded model with nothing left is the end of the road.
    if (m_commitModel->isLoading()) {
        return; // the next rowsInserted retries
    }
    // A page cap keeps "reveal" from walking a million commits when the target is
    // hidden by a filter, or simply very old.
    constexpr int kRevealPageCap = 400;
    if (!m_commitModel->hasMore() || m_revealPages++ >= kRevealPageCap) {
        setStatus(
            QStringLiteral("Commit %1 is not in the loaded history — a branch filter may hide it.")
                .arg(m_revealTarget.left(8)));
        m_revealTarget.clear();
        return;
    }
    m_commitModel->fetchMore(QModelIndex());
}

QStringList MainWindow::ignoreForDiagnostics(const QString& path, const QString& scope,
                                             const QString& kind) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    report << QStringLiteral("pattern: %1").arg(ignorePatternFor(path, kind));
    onIgnoreRequested(QStringList{path}, scope, kind);
    bool listed = false;
    for (const FileChange& change : m_session->status()) {
        if (QString::fromStdString(change.path) == path) {
            listed = true;
            break;
        }
    }
    report << QStringLiteral("listed: %1").arg(listed ? QStringLiteral("yes")
                                                     : QStringLiteral("no"));
    report << QStringLiteral("status: %1")
                  .arg(statusBar() ? statusBar()->currentMessage() : QString());
    return report;
}

QStringList MainWindow::coauthorForDiagnostics(const QString& name, const QString& email) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const QString message = m_session->withCoAuthors(
        QStringLiteral("Subject line\n"), {CoAuthor{name.toStdString(), email.toStdString()}});
    for (const QString& line : message.split(QLatin1Char('\n'))) {
        report << line;
    }
    return report;
}

QStringList MainWindow::commitTemplateForDiagnostics() {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const QString text = m_session->commitTemplate();
    if (text.isEmpty()) {
        report << QStringLiteral("(no template)");
        return report;
    }
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        report << line;
    }
    return report;
}

QStringList MainWindow::externalToolForDiagnostics(const QString& op, const QString& kind,
                                                   const QString& name) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const bool merge = kind == QStringLiteral("merge");
    const QString key = merge ? QStringLiteral("merge.tool") : QStringLiteral("diff.tool");
    if (op == QStringLiteral("get")) {
        report << QStringLiteral("%1: %2").arg(key, m_session->externalTool(merge));
        report << QStringLiteral("available: %1")
                      .arg(m_session->availableExternalTools(merge).size());
        return report;
    }
    QString error;
    if (!m_session->setExternalTool(merge, name, &error)) {
        report << QStringLiteral("failed: %1").arg(error.trimmed());
        return report;
    }
    report << QStringLiteral("%1: %2").arg(key, m_session->externalTool(merge));
    return report;
}

QStringList MainWindow::runExternalToolForDiagnostics(const QString& kind, const QString& base,
                                                      const QString& target,
                                                      const QString& path) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const bool merge = kind == QStringLiteral("merge");
    const SyncResult result = merge
                                  ? m_session->runExternalMergeTool(path)
                                  : m_session->runExternalDiffTool(base, target, path);
    report << QStringLiteral("%1: %2").arg(kind, result.ok ? QStringLiteral("ok")
                                                           : QStringLiteral("failed"));
    report << QStringLiteral("exit: %1").arg(result.exitCode);
    for (const QString& line : QString::fromStdString(result.output)
                                   .split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        report << line.trimmed();
    }
    return report;
}

QStringList MainWindow::terminalForDiagnostics() {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const std::string workDir = m_session->info().workDir;
    if (workDir.empty()) {
        report << QStringLiteral("no working tree");
        return report;
    }
    const TerminalLaunch launch =
        buildTerminalLaunch(workDir, qgetenv("TERMINAL").toStdString(),
                            [](const std::string& name) -> std::string {
                                return QStandardPaths::findExecutable(QString::fromStdString(name))
                                    .toStdString();
                            });
    report << QStringLiteral("valid: %1").arg(launch.valid ? QStringLiteral("yes")
                                                           : QStringLiteral("no"));
    if (!launch.valid) {
        return report;
    }
    report << QStringLiteral("program: %1").arg(QString::fromStdString(launch.program));
    QStringList args;
    for (const std::string& arg : launch.args) {
        args << QString::fromStdString(arg);
    }
    report << QStringLiteral("args: %1").arg(args.join(QLatin1Char(' ')));
    return report;
}

QStringList MainWindow::tabsForDiagnostics(const QString& operation, const QStringList& args) {
    if (operation == QLatin1String("open") && !args.isEmpty()) {
        openRepository(args.front());
    } else if (operation == QLatin1String("activate") && !args.isEmpty()) {
        activateContext(args.front().toInt());
    } else if (operation == QLatin1String("close") && !args.isEmpty()) {
        closeContext(args.front().toInt());
    } else if (operation == QLatin1String("close-others") && !args.isEmpty()) {
        keepOnlyContext(args.front().toInt());
    } else if (operation == QLatin1String("close-all")) {
        closeAllContexts();
    } else if (operation == QLatin1String("new")) {
        onNewTabRequested();
    } else if (operation == QLatin1String("rename") && args.size() >= 2) {
        applyTabAlias(args.at(0).toInt(), args.at(1));
    }

    QStringList report;
    report << QStringLiteral("active: %1").arg(m_active);
    report << QStringLiteral("count: %1").arg(m_contexts.size());
    for (std::size_t i = 0; i < m_contexts.size(); ++i) {
        const RepoContext& context = *m_contexts[i];
        report << QStringLiteral("tab %1: %2 | path=%3 | opened=%4")
                      .arg(i)
                      .arg(contextLabel(context),
                           context.path.isEmpty() ? QStringLiteral("(none)") : context.path,
                           context.opened ? QStringLiteral("yes") : QStringLiteral("no"));
    }
    // What the strip actually shows, which is not the same claim as what the context
    // is called: the two disagreeing is the whole shape of the bug where a restored
    // tab wore its old label until the reader switched away and back.
    if (m_tabBar) {
        for (int i = 0; i < m_tabBar->count(); ++i) {
            report << QStringLiteral("bar %1: %2").arg(i).arg(m_tabBar->tabText(i));
        }
    }
    return report;
}

QStringList MainWindow::recentForDiagnostics(const QString& operation, const QStringList& args) {
    RecentRepositories recent;
    recent.load();
    if (operation == QLatin1String("add") && !args.isEmpty()) {
        recent.touch(args.at(0), QFileInfo(args.at(0)).fileName());
    } else if (operation == QLatin1String("named") && args.size() >= 2) {
        recent.touch(args.at(0), args.at(1));
    } else if (operation == QLatin1String("clear")) {
        recent.clear();
    }
    QStringList report;
    for (const RecentRepository& entry : recent.entries()) {
        report << QStringLiteral("%1 | %2 | %3")
                      .arg(entry.name, entry.path, entry.lastUsed.toString(Qt::ISODate));
    }
    report << QStringLiteral("count: %1").arg(recent.entries().size());
    return report;
}

QStringList MainWindow::startPageForDiagnostics() const {
    QStringList report;
    if (!m_startPage) {
        report << QStringLiteral("no start page");
        return report;
    }
    report << QStringLiteral("visible: %1")
                  .arg(m_stack && m_stack->currentWidget() == m_startPage ? QStringLiteral("yes")
                                                                          : QStringLiteral("no"));
    const auto* list = m_startPage->findChild<QTreeWidget*>(QStringLiteral("recentList"));
    if (!list) {
        report << QStringLiteral("no recent list");
        return report;
    }
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* item = list->topLevelItem(i);
        report << QStringLiteral("%1 | %2 | %3")
                      .arg(item->text(0), item->text(1), item->text(2));
    }
    report << QStringLiteral("count: %1").arg(list->topLevelItemCount());
    return report;
}

QStringList MainWindow::createRepositoryForDiagnostics(const QString& path, const QString& branch) {
    QStringList report;
    if (path.isEmpty()) {
        report << QStringLiteral("no path");
        return report;
    }
    QString error;
    if (!RepoSession::createRepository(path, branch.isEmpty() ? QStringLiteral("main") : branch,
                                       &error)) {
        report << QStringLiteral("error: %1").arg(error);
        return report;
    }
    report << QStringLiteral("created: %1").arg(path);
    // An unborn branch reports no branch name through libgit2 — `info()` says so
    // deliberately — so the requested initial branch is what the report names.
    report << QStringLiteral("branch: %1")
                  .arg(branch.isEmpty() ? QStringLiteral("main") : branch);
    if (!openRepository(path)) {
        report << QStringLiteral("could not open the new repository");
        return report;
    }
    report += tabsForDiagnostics(QStringLiteral("list"), {});
    return report;
}

QStringList MainWindow::cloneForDiagnostics(const QString& url, const QString& destination,
                                            const QStringList& args) {
    gk::CloneOptions options;
    for (const QString& arg : args) {
        if (arg == QStringLiteral("partial")) {
            options.partial = true;
        } else if (arg.startsWith(QStringLiteral("branch="))) {
            options.branch = arg.mid(7).toStdString();
        } else if (arg.startsWith(QStringLiteral("depth="))) {
            options.depth = arg.mid(6).toInt();
        } else if (arg.startsWith(QStringLiteral("since="))) {
            options.shallowSince = arg.mid(6).toStdString();
        }
    }
    const gk::CloneResult result =
        gk::cloneRepository(url.toStdString(), destination.toStdString(), options);

    QStringList report;
    report << QStringLiteral("clone: %1")
                  .arg(result.ok ? QStringLiteral("ok") : QStringLiteral("failed"));
    report << QStringLiteral("exit: %1").arg(result.exitCode);
    if (!result.ok) {
        report << lastLines(QString::fromStdString(result.output), 4);
        return report;
    }
    if (!openRepository(destination)) {
        report << QStringLiteral("opened: no");
        return report;
    }
    const RepoInfo info = m_session->info();
    report << QStringLiteral("opened: %1").arg(destination);
    report << QStringLiteral("branch: %1").arg(QString::fromStdString(info.headBranch));
    report << QStringLiteral("partial: %1").arg(info.partialClone ? 1 : 0);
    report << QStringLiteral("shallow: %1")
                  .arg(QFile::exists(destination + QStringLiteral("/.git/shallow")) ? 1 : 0);
    return report;
}

QStringList MainWindow::searchForDiagnostics(const QString& query, const QString& field,
                                             const QString& revealOid) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    refreshCommands();
    CommandPalette palette(m_commands);
    preparePalette(palette, true);
    if (field.startsWith(QLatin1String("dates:"))) {
        // dates:<from>..<to>, both yyyy-MM-dd
        const QStringList range = field.mid(6).split(QStringLiteral(".."));
        palette.setDateRange(QDate::fromString(range.value(0), Qt::ISODate),
                             QDate::fromString(range.value(1), Qt::ISODate));
    }
    palette.setQuery(query);
    report << palette.rowsForDiagnostics();
    report << QStringLiteral("current: %1").arg(palette.currentRowText());

    if (!revealOid.isEmpty()) {
        int row = m_commitModel->rowForCommit(revealOid);
        constexpr int kRevealPageCap = 400;
        for (int i = 0; row < 0 && i < kRevealPageCap; ++i) {
            // Let an in-flight page land before deciding anything: canLoadMore() is
            // false while loading, which is not the same as "no more history".
            QCoreApplication::processEvents(QEventLoop::AllEvents, 300);
            row = m_commitModel->rowForCommit(revealOid);
            if (row >= 0 || m_commitModel->isLoading()) {
                continue;
            }
            if (!m_commitModel->hasMore()) {
                break;
            }
            m_commitModel->fetchMore(QModelIndex());
        }
        report << (row >= 0 ? QStringLiteral("revealed row %1").arg(row)
                            : QStringLiteral("reveal: not found"));
    }
    return report;
}

QStringList MainWindow::preferencesForDiagnostics(const QString& operation, const QString& key,
                                                  const QString& value) {
    QStringList report;
    const bool set = operation == QStringLiteral("set");
    const auto asBool = [](const QString& text) {
        return text == QStringLiteral("true") || text == QStringLiteral("1") ||
               text == QStringLiteral("on");
    };
    SettingsStore store(repositorySettingsPath());

    if (key == QStringLiteral("graph.animate")) {
        const QString full = SettingsStore::globalKey(QStringLiteral("graph/animate"));
        if (set) {
            store.setBool(full, asBool(value));
            loadGraphAnimation();
        }
        report << QStringLiteral("graph.animate: %1")
                      .arg(SettingsStore().boolValue(full, true) ? 1 : 0);
    } else if (key == QStringLiteral("theme")) {
        if (set) {
            store.setString(SettingsStore::globalKey(QStringLiteral("theme")),
                            value.isEmpty() ? QStringLiteral("dark") : value);
            loadSavedAppearance();
            applyAppearance();
        }
        report << QStringLiteral("theme: %1")
                      .arg(theme::mode() == theme::ThemeMode::Light ? QStringLiteral("light")
                                                                    : QStringLiteral("dark"));
    } else if (key == QStringLiteral("zoom")) {
        if (set) {
            store.setInt(SettingsStore::globalKey(QStringLiteral("zoom")), value.toInt());
            loadSavedAppearance();
            applyAppearance();
        }
        report << QStringLiteral("zoom: %1").arg(theme::zoomPercent());
    } else if (key == QStringLiteral("preflight")) {
        const QString full = store.repositoryKey(QStringLiteral("preflight"));
        if (set) {
            store.setBool(full, asBool(value));
            loadPreflight();
    loadPrevention();
        }
        report << QStringLiteral("preflight: %1").arg(m_preflightEnabled ? 1 : 0);
    } else if (key == QStringLiteral("autofetch.enabled")) {
        const QString full =
            store.repositoryKey(QStringLiteral("autofetch")) + QStringLiteral("/enabled");
        if (set) {
            store.setBool(full, asBool(value));
            loadAutoFetch();
        }
        report << QStringLiteral("autofetch.enabled: %1").arg(m_autoFetchEnabled ? 1 : 0);
    } else if (key == QStringLiteral("autofetch.remotes")) {
        const QString full =
            store.repositoryKey(QStringLiteral("autofetch")) + QStringLiteral("/remotes");
        if (set) {
            store.setList(full, value.split(QLatin1Char(','), Qt::SkipEmptyParts));
            loadAutoFetch();
        }
        report << QStringLiteral("autofetch.remotes: %1").arg(m_autoFetchRemotes.join(QLatin1Char(',')));
    } else if (key == QStringLiteral("submodule.autoupdate")) {
        const QString full =
            store.repositoryKey(QStringLiteral("submodules")) + QStringLiteral("/enabled");
        if (set) {
            store.setBool(full, asBool(value));
            loadSubmoduleAutoupdate();
        }
        report << QStringLiteral("submodule.autoupdate: %1").arg(m_submoduleAutoupdate ? 1 : 0);
    } else if (key == QStringLiteral("submodule.autoupdate-remote")) {
        const QString full =
            store.repositoryKey(QStringLiteral("submodules")) + QStringLiteral("/remote");
        if (set) {
            store.setBool(full, asBool(value));
            loadSubmoduleAutoupdate();
        }
        report << QStringLiteral("submodule.autoupdate-remote: %1")
                      .arg(m_submoduleAutoupdateRemote ? 1 : 0);
    } else if (key == QStringLiteral("rerere.enabled") ||
               key == QStringLiteral("rerere.autoupdate")) {
        if (!m_session || !m_session->isOpen()) {
            report << QStringLiteral("%1: no repository is open").arg(key);
            return report;
        }
        const bool isEnabled = key == QStringLiteral("rerere.enabled");
        if (set) {
            if (isEnabled) {
                setRerereEnabled(asBool(value));
            } else {
                setRerereAutoupdate(asBool(value));
            }
        }
        const bool current =
            isEnabled ? m_session->rerereEnabled() : m_session->rerereAutoupdate();
        report << QStringLiteral("%1: %2").arg(key).arg(current ? 1 : 0);
    } else {
        report << QStringLiteral("unknown preference: %1").arg(key);
    }
    return report;
}

void MainWindow::maybeAutoUpdateSubmodules() {
    if (!m_submoduleAutoupdate || m_submoduleUpdateRunning || !m_session ||
        !m_session->isOpen()) {
        return;
    }
    if (m_submodules.empty()) {
        return;
    }
    m_submoduleUpdateRunning = true;
    if (m_commitView) {
        m_commitView->setBusy(true);
    }
    setStatus(QStringLiteral("Updating submodules…"));
    m_session->requestAutoUpdateSubmodules(m_submoduleAutoupdateRemote);
}

void MainWindow::onSubmodulesUpdated(bool ok, const QString& output) {
    m_submoduleUpdateRunning = false;
    if (m_commitView) {
        m_commitView->setBusy(false);
    }
    if (!ok) {
        // Silent, like the background auto-fetch: a failure here must not throw a
        // modal over whatever the reader is doing. The last line is git's own.
        notify(NotificationLog::Warning,
               QStringLiteral("Submodule update failed: %1")
                   .arg(output.trimmed().section(QLatin1Char('\n'), -1)));
        return;
    }
    refreshSubmodules();
    refreshPanels();
    notify(NotificationLog::Success, QStringLiteral("Submodules updated."));
}

bool MainWindow::confirmPreflight(const QString& ours, const QString& theirs) {
    if (!m_preflightEnabled || !m_session || !m_session->isOpen()) {
        return true;
    }
    QString error;
    const MergePreview preview = m_session->previewMerge(ours, theirs, &error);
    if (!error.isEmpty()) {
        // The prediction failed, not the operation: say so and let it run rather
        // than block work behind a check that was only ever advisory.
        setStatus(QStringLiteral("Could not check for conflicts: %1").arg(error));
        return true;
    }
    // Nothing to predict — one side is already reachable from the other — or the
    // merge is clean: there is nothing to warn about.
    if (!preview.available || preview.clean) {
        return true;
    }

    QStringList names;
    for (const std::string& path : preview.conflictedPaths) {
        names << describePredictedConflict(preview, path);
    }
    const QString listed = names.isEmpty() ? QStringLiteral("(git named no files)")
                                           : names.join(QLatin1Char('\n'));
    return askYes(QStringLiteral("Conflicts predicted"),
                  QStringLiteral("Merging %1 and %2 would conflict in %3 file(s):\n\n%4\n\n"
                                 "Continue?")
                      .arg(ours, theirs)
                      .arg(names.size())
                      .arg(listed));
}

QString MainWindow::dropPreviewText(const QString& source, const QString& target,
                                    bool* warns) const {
    if (!m_session || !m_session->isOpen()) {
        return QString();
    }
    QString error;
    const MergePreview preview = m_session->previewMerge(source, target, &error);
    // A prediction that could not be made says nothing rather than something wrong:
    // the list must not claim "no conflicts" when the check itself failed.
    if (!error.isEmpty()) {
        return QString();
    }
    if (warns) {
        *warns = preview.available && !preview.clean;
    }
    return describeMergePreview(preview);
}

void MainWindow::refreshRerereActions() {
    if (!m_rerereAction || !m_rerereAutoupdateAction) {
        return;
    }
    const bool open = m_session && m_session->isOpen();
    m_rerereAction->setEnabled(open);
    m_rerereAutoupdateAction->setEnabled(open);
    {
        const QSignalBlocker blockRerere(m_rerereAction);
        const QSignalBlocker blockAuto(m_rerereAutoupdateAction);
        m_rerereAction->setChecked(open && m_session->rerereEnabled());
        m_rerereAutoupdateAction->setChecked(open && m_session->rerereAutoupdate());
    }
}

bool MainWindow::rerereEnabled() const {
    return m_session && m_session->isOpen() && m_session->rerereEnabled();
}

void MainWindow::setRerereEnabled(bool on) {
    if (!m_session || !m_session->isOpen()) {
        refreshRerereActions();
        return;
    }
    QString error;
    if (!m_session->setRerereEnabled(on, &error)) {
        reportFailure(QStringLiteral("Cannot change rerere"), error);
        refreshRerereActions();
        return;
    }
    refreshRerereActions();
    setStatus(on ? QStringLiteral("rerere enabled — resolutions will be remembered.")
                 : QStringLiteral("rerere disabled."));
}

void MainWindow::setRerereAutoupdate(bool on) {
    if (!m_session || !m_session->isOpen()) {
        refreshRerereActions();
        return;
    }
    QString error;
    if (!m_session->setRerereAutoupdate(on, &error)) {
        reportFailure(QStringLiteral("Cannot change rerere"), error);
        refreshRerereActions();
        return;
    }
    refreshRerereActions();
}

void MainWindow::refreshStashes() {
    m_stashes = (m_session && m_session->isOpen()) ? m_session->stashList()
                                                   : std::vector<StashEntry>{};
    QStringList messages;
    QStringList oids;
    for (const StashEntry& entry : m_stashes) {
        messages << QString::fromStdString(entry.message);
        oids << QString::fromStdString(entry.oid);
    }
    if (m_refModel) {
        m_refModel->setStashes(messages, oids);
    }
    const bool open = m_session && m_session->isOpen() && !m_session->info().bare;
    if (m_stashAction) {
        m_stashAction->setEnabled(open && !m_session->status().empty());
    }
    if (m_popStashAction) {
        m_popStashAction->setEnabled(open && !m_stashes.empty());
    }
}

void MainWindow::refreshWorktrees() {
    m_worktrees = (m_session && m_session->isOpen()) ? m_session->worktrees()
                                                     : std::vector<WorktreeEntry>{};
    if (m_refModel) {
        m_refModel->setWorktrees(m_worktrees);
    }
    if (!m_session || !m_session->isOpen()) {
        return;
    }

    // Mark the window when it is on a linked worktree: two windows on the same
    // repository look identical otherwise, and the reader cannot tell which
    // checkout their edits are landing in. git lists the main worktree first, so
    // the first entry being current means this is it.
    const RepoInfo info = m_session->info();
    const QString path =
        QString::fromStdString(info.workDir.empty() ? info.gitDir : info.workDir);
    // The title names the repository the way its tab does — the alias, or the
    // directory's name — rather than the whole path, which is long enough to push the
    // worktree marker off the end of the title bar.
    const QString label = active() ? contextLabel(*active()) : path;
    const bool onMain = m_worktrees.empty() || m_worktrees.front().current;
    if (onMain) {
        setWindowTitle(QStringLiteral("GitMycelium — %1").arg(label));
        return;
    }
    QString name;
    for (const WorktreeEntry& worktree : m_worktrees) {
        if (worktree.current) {
            name = QString::fromStdString(worktree.name);
            break;
        }
    }
    setWindowTitle(QStringLiteral("GitMycelium — %1  [worktree: %2]").arg(label, name));
}

void MainWindow::openWorktree(const QString& path) {
    if (!m_session || !m_session->isOpen() || path.isEmpty()) {
        return;
    }
    const RepoInfo info = m_session->info();
    const QString current =
        QString::fromStdString(info.workDir.empty() ? info.gitDir : info.workDir);
    if (QDir::cleanPath(path) == QDir::cleanPath(current)) {
        setStatus(QStringLiteral("Already open: %1").arg(path));
        return;
    }
    if (!openRepository(path)) {
        return;
    }
    setStatus(QStringLiteral("Opened worktree %1").arg(path));
}

void MainWindow::createWorktree() {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    if (m_session->info().bare) {
        QMessageBox::information(this, QStringLiteral("New worktree"),
                                 QStringLiteral("A bare repository has no working tree."));
        return;
    }

    WorktreeDialog dialog(this);
    QStringList branches;
    for (const RefInfo& ref : m_session->refs()) {
        if (ref.kind == RefKind::LocalBranch) {
            branches << QString::fromStdString(ref.shortName);
        }
    }
    branches.sort();
    dialog.setBranches(branches);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString path = dialog.path();
    const QString branch = dialog.branch();
    if (path.isEmpty() || branch.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("New worktree"),
                                 QStringLiteral("A worktree needs a directory and a branch."));
        return;
    }

    QString error;
    if (!m_session->addWorktree(path, branch, dialog.createBranch(), dialog.startPoint(),
                                &error)) {
        reportFailure(QStringLiteral("Cannot create worktree"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Created worktree %1 for %2").arg(path, branch));
}

void MainWindow::removeWorktreeAt(const QModelIndex& index) {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    const QString path = index.data(RefModel::WorktreePathRole).toString();
    if (path.isEmpty()) {
        return;
    }
    if (index.data(RefModel::WorktreeCurrentRole).toBool()) {
        QMessageBox::information(
            this, QStringLiteral("Cannot remove worktree"),
            QStringLiteral("This is the worktree the window is open on. Open another one first."));
        return;
    }
    if (index.data(RefModel::WorktreeLockedRole).toBool()) {
        QMessageBox::information(this, QStringLiteral("Cannot remove worktree"),
                                 QStringLiteral("This worktree is locked. Unlock it first."));
        return;
    }
    if (!askYes(QStringLiteral("Remove worktree"),
                QStringLiteral("Remove the worktree at\n\n%1?\n\nThe branch it checks out is "
                               "kept; uncommitted work in it is not.")
                    .arg(path))) {
        return;
    }

    QString error;
    if (!m_session->removeWorktree(path, false, &error)) {
        // git refuses a worktree with modifications. That refusal is the safety
        // net, not the answer: the reader is asked once, explicitly, before the
        // changes are thrown away.
        if (!askYes(QStringLiteral("Worktree has changes"),
                    QStringLiteral("git refused:\n\n%1\n\nRemove it anyway and discard its "
                                   "changes?")
                        .arg(error))) {
            return;
        }
        if (!m_session->removeWorktree(path, true, &error)) {
            reportFailure(QStringLiteral("Cannot remove worktree"), error);
            return;
        }
    }
    refreshPanels();
    setStatus(QStringLiteral("Removed worktree %1").arg(path));
}

void MainWindow::lockWorktreeAt(const QModelIndex& index) {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    const QString path = index.data(RefModel::WorktreePathRole).toString();
    if (path.isEmpty() || index.data(RefModel::WorktreeLockedRole).toBool()) {
        return;
    }
    bool ok = false;
    const QString reason = QInputDialog::getText(this, QStringLiteral("Lock worktree"),
                                                 QStringLiteral("Reason (optional):"),
                                                 QLineEdit::Normal, QString(), &ok);
    if (!ok) {
        return;
    }
    QString error;
    if (!m_session->lockWorktree(path, reason, &error)) {
        reportFailure(QStringLiteral("Cannot lock worktree"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Locked worktree %1").arg(path));
}

void MainWindow::unlockWorktreeAt(const QModelIndex& index) {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    const QString path = index.data(RefModel::WorktreePathRole).toString();
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!m_session->unlockWorktree(path, &error)) {
        reportFailure(QStringLiteral("Cannot unlock worktree"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Unlocked worktree %1").arg(path));
}

void MainWindow::pruneWorktrees() {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    QString error;
    if (!m_session->pruneWorktrees(&error)) {
        reportFailure(QStringLiteral("Cannot prune worktrees"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Pruned stale worktree entries."));
}

void MainWindow::refreshSubmodules() {
    m_submodules = (m_session && m_session->isOpen()) ? m_session->submodules()
                                                      : std::vector<SubmoduleEntry>{};
    if (m_refModel) {
        m_refModel->setSubmodules(m_submodules);
    }
}

namespace {

QString submoduleDirectory(const RepoInfo& info, const QString& path) {
    const QString base =
        QString::fromStdString(info.workDir.empty() ? info.gitDir : info.workDir);
    return QDir(base).filePath(path);
}

} // namespace

void MainWindow::openSubmodule(const QString& path) {
    if (!m_session || !m_session->isOpen() || path.isEmpty()) {
        return;
    }
    const QString full = submoduleDirectory(m_session->info(), path);
    if (!QFileInfo(full).isDir()) {
        QMessageBox::information(
            this, QStringLiteral("Submodule"),
            QStringLiteral("The submodule is not checked out yet. Initialize it first."));
        return;
    }
    if (!openRepository(full)) {
        return;
    }
    setStatus(QStringLiteral("Opened submodule %1").arg(path));
}

void MainWindow::addSubmodule() {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    if (m_session->info().bare) {
        QMessageBox::information(this, QStringLiteral("Add submodule"),
                                 QStringLiteral("A bare repository has no working tree."));
        return;
    }

    SubmoduleDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString url = dialog.url();
    if (url.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Add submodule"),
                                 QStringLiteral("A submodule needs a repository URL."));
        return;
    }

    QString error;
    if (!m_session->addSubmodule(url, dialog.path(), dialog.branch(), false, &error)) {
        reportFailure(QStringLiteral("Cannot add submodule"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Added submodule %1").arg(dialog.path().isEmpty() ? url
                                                                            : dialog.path()));
}

void MainWindow::initSubmoduleAt(const QModelIndex& index) {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    const QString path = index.data(RefModel::SubmodulePathRole).toString();
    if (path.isEmpty()) {
        return;
    }
    QString error;
    // `init` alone only registers the URL; `update --init` is what materialises the
    // checkout, and `--recursive` reaches a submodule inside a submodule.
    if (!m_session->updateSubmodules(QStringList{path}, true, false, true, &error)) {
        reportFailure(QStringLiteral("Cannot initialize submodule"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Initialized submodule %1").arg(path));
}

void MainWindow::updateSubmoduleAt(const QModelIndex& index, bool remote) {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    const QString path = index.data(RefModel::SubmodulePathRole).toString();
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!m_session->updateSubmodules(QStringList{path}, true, remote, true, &error)) {
        reportFailure(remote ? QStringLiteral("Cannot update submodule to remote")
                             : QStringLiteral("Cannot update submodule"),
                      error);
        return;
    }
    refreshPanels();
    setStatus((remote ? QStringLiteral("Updated submodule %1 to its remote")
                      : QStringLiteral("Updated submodule %1"))
                  .arg(path));
}

void MainWindow::syncSubmoduleAt(const QModelIndex& index) {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    const QString path = index.data(RefModel::SubmodulePathRole).toString();
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!m_session->syncSubmodules(QStringList{path}, true, &error)) {
        reportFailure(QStringLiteral("Cannot sync submodule"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Synced submodule URL for %1").arg(path));
}

void MainWindow::deinitSubmoduleAt(const QModelIndex& index) {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    const QString path = index.data(RefModel::SubmodulePathRole).toString();
    if (path.isEmpty()) {
        return;
    }
    if (!askYes(QStringLiteral("Deinitialize submodule"),
                QStringLiteral("Remove the checkout at\n\n%1?\n\nThe submodule stays in "
                               ".gitmodules and can be initialized again.")
                    .arg(path))) {
        return;
    }
    QString error;
    if (!m_session->deinitSubmodule(path, false, &error)) {
        // git refuses a submodule with local changes; ask once before discarding them.
        if (!askYes(QStringLiteral("Submodule has changes"),
                    QStringLiteral("git refused:\n\n%1\n\nDeinitialize it anyway and discard "
                                   "its changes?")
                        .arg(error))) {
            return;
        }
        if (!m_session->deinitSubmodule(path, true, &error)) {
            reportFailure(QStringLiteral("Cannot deinitialize submodule"), error);
            return;
        }
    }
    refreshPanels();
    setStatus(QStringLiteral("Deinitialized submodule %1").arg(path));
}

void MainWindow::initializeAllSubmodules() {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    QString error;
    if (!m_session->updateSubmodules({}, true, false, true, &error)) {
        reportFailure(QStringLiteral("Cannot initialize submodules"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Initialized submodules."));
}

void MainWindow::updateAllSubmodules() {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    QString error;
    if (!m_session->updateSubmodules({}, true, false, true, &error)) {
        reportFailure(QStringLiteral("Cannot update submodules"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Updated submodules."));
}

void MainWindow::syncAllSubmodules() {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    QString error;
    if (!m_session->syncSubmodules({}, true, &error)) {
        reportFailure(QStringLiteral("Cannot sync submodules"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Synced submodule URLs."));
}

void MainWindow::stashWorkingTree() {
    if (!m_session || !m_session->isOpen()) {
        return;
    }
    StashDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const std::size_t before = m_stashes.size();
    QString error;
    if (!m_session->stashPush(dialog.message(), dialog.includeUntracked(), {}, &error)) {
        reportFailure(QStringLiteral("Cannot stash"), error);
        return;
    }
    refreshPanels();
    setStatus(m_stashes.size() > before ? QStringLiteral("Stashed the working changes.")
                                        : QStringLiteral("Nothing to stash."));
}

void MainWindow::stashFiles(const QStringList& paths) {
    if (!m_session || !m_session->isOpen() || paths.isEmpty()) {
        return;
    }
    StashDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Stash selected files"));
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    QString error;
    if (!m_session->stashPush(dialog.message(), dialog.includeUntracked(), paths, &error)) {
        reportFailure(QStringLiteral("Cannot stash the selected files"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Stashed %1 file(s).").arg(paths.size()));
}

void MainWindow::popTopStash() {
    if (m_stashes.empty()) {
        setStatus(QStringLiteral("No stash to pop."));
        return;
    }
    restoreStash(QString::fromStdString(m_stashes.front().oid), true,
                 QString::fromStdString(m_stashes.front().message));
}

void MainWindow::restoreStash(const QString& oid, bool drop, const QString& label) {
    if (!m_session || !m_session->isOpen() || oid.isEmpty()) {
        return;
    }
    QString error;
    const bool ok = drop ? m_session->stashPop(oid, &error) : m_session->stashApply(oid, &error);
    refreshPanels();
    if (!ok) {
        reportFailure(drop ? QStringLiteral("Cannot pop the stash")
                           : QStringLiteral("Cannot apply the stash"),
                      error);
        // A conflicting pop or apply leaves unmerged entries with no MERGE_HEAD or
        // REBASE_HEAD, so the conflict page is opened by looking at the conflicts
        // themselves — the operation-state check would find nothing to show.
        if (!m_session->conflicts().empty()) {
            showConflictsPage();
        }
        return;
    }
    setStatus(QStringLiteral("Restored %1.").arg(label));
}

void MainWindow::dropStash(const QString& oid, const QString& label) {
    if (!m_session || !m_session->isOpen() || oid.isEmpty()) {
        return;
    }
    if (!askYes(QStringLiteral("Drop stash"),
                QStringLiteral("Drop %1?\nThis cannot be undone.").arg(label))) {
        return;
    }
    QString error;
    if (!m_session->stashDrop(oid, &error)) {
        reportFailure(QStringLiteral("Cannot drop the stash"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Dropped %1.").arg(label));
}

void MainWindow::branchFromStash(const QString& oid, const QString& label) {
    if (!m_session || !m_session->isOpen() || oid.isEmpty()) {
        return;
    }
    bool accepted = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("Branch from stash"),
                              QStringLiteral("New branch name:"), QLineEdit::Normal, QString(),
                              &accepted);
    if (!accepted || name.trimmed().isEmpty()) {
        return;
    }
    QString error;
    if (!m_session->stashBranch(oid, name.trimmed(), &error)) {
        reportFailure(QStringLiteral("Cannot create the branch"), error);
        return;
    }
    refreshPanels();
    setStatus(QStringLiteral("Created %1 from %2.").arg(name.trimmed(), label));
}

QStringList MainWindow::stashForDiagnostics(const QString& operation, const QString& argument) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const std::vector<StashEntry> before = m_session->stashList();
    report << QStringLiteral("stashes before: %1").arg(before.size());
    for (const StashEntry& entry : before) {
        report << QStringLiteral("  %1 %2")
                      .arg(QString::fromStdString(entry.oid).left(10),
                           QString::fromStdString(entry.message));
    }

    const QString topOid = before.empty() ? QString() : QString::fromStdString(before.front().oid);
    const QString target = argument.isEmpty() ? topOid : argument;
    QString error;
    bool ok = false;
    if (operation == QStringLiteral("push")) {
        ok = m_session->stashPush(argument, false, {}, &error);
    } else if (operation == QStringLiteral("pop")) {
        ok = m_session->stashPop(target, &error);
    } else if (operation == QStringLiteral("apply")) {
        ok = m_session->stashApply(target, &error);
    } else if (operation == QStringLiteral("drop")) {
        ok = m_session->stashDrop(target, &error);
    } else if (operation == QStringLiteral("branch")) {
        ok = m_session->stashBranch(target, argument, &error);
    } else if (operation == QStringLiteral("list")) {
        ok = true;
    } else {
        report << QStringLiteral("unknown stash operation: %1").arg(operation);
        return report;
    }

    refreshPanels();
    report << QStringLiteral("result: %1").arg(ok ? QStringLiteral("ok") : error);
    for (const StashEntry& entry : m_stashes) {
        report << QStringLiteral("  %1 %2")
                      .arg(QString::fromStdString(entry.oid).left(10),
                           QString::fromStdString(entry.message));
    }
    report << QStringLiteral("stashes after: %1").arg(m_stashes.size());
    report << QStringLiteral("changes: %1").arg(m_session->status().size());
    report << QStringLiteral("conflicts: %1").arg(m_session->conflicts().size());
    return report;
}

QStringList MainWindow::syncForDiagnostics(const QString& operation, const QString& argument) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const QString branch = QString::fromStdString(m_session->info().headBranch);
    // For every operation but push-tag the optional argument names the remote; for
    // push-tag it names the tag, and the remote is the picker's choice.
    const QString selected = currentSyncRemote();
    const QString remote = argument.isEmpty() ? selected : argument;

    SyncResult result;
    if (operation == QStringLiteral("fetch")) {
        result = m_session->syncBlocking(SyncKind::Fetch, remote, branch);
    } else if (operation == QStringLiteral("fetch-all")) {
        result = m_session->syncBlocking(SyncKind::FetchAll, QString(), branch);
    } else if (operation == QStringLiteral("pull")) {
        result = m_session->syncBlocking(SyncKind::Pull, remote, branch, PullMode::FfOnly);
    } else if (operation == QStringLiteral("pull-merge")) {
        result = m_session->syncBlocking(SyncKind::Pull, remote, branch, PullMode::Merge);
    } else if (operation == QStringLiteral("pull-rebase")) {
        result = m_session->syncBlocking(SyncKind::Pull, remote, branch, PullMode::Rebase);
    } else if (operation == QStringLiteral("push")) {
        result = m_session->syncBlocking(SyncKind::Push, remote, branch);
    } else if (operation == QStringLiteral("push-force")) {
        result = m_session->syncBlocking(SyncKind::PushForce, remote, branch);
    } else if (operation == QStringLiteral("push-tags")) {
        result = m_session->syncBlocking(SyncKind::PushTags, remote, branch);
    } else if (operation == QStringLiteral("push-tag")) {
        result = m_session->pushTagBlocking(selected, argument);
    } else if (operation == QStringLiteral("delete-tag-all-remotes")) {
        const std::vector<RemoteInfo> remotes = m_session->remotes();
        QStringList touched;
        QStringList failed;
        result.ok = true;
        result.exitCode = 0;
        for (const RemoteInfo& remote : remotes) {
            const QString name = QString::fromStdString(remote.name);
            const SyncResult one = m_session->deleteRemoteTagBlocking(name, argument);
            result.output += one.output;
            if (one.ok) {
                touched << name;
            } else {
                failed << name;
                result.ok = false;
                result.exitCode = one.exitCode;
            }
        }
        report << QStringLiteral("tag: %1").arg(argument);
        report << QStringLiteral("remotes: %1").arg(touched.join(QLatin1Char(',')));
        report << QStringLiteral("failed: %1").arg(failed.join(QLatin1Char(',')));
    } else {
        report << QStringLiteral("unknown sync operation: %1").arg(operation);
        return report;
    }

    refreshPanels();
    report << QStringLiteral("operation: %1").arg(operation);
    report << QStringLiteral("remote: %1").arg(selected);
    report << QStringLiteral("result: %1")
                  .arg(result.ok ? QStringLiteral("ok") : QStringLiteral("failed"));
    report << QStringLiteral("exit: %1").arg(result.exitCode);
    const QString output = QString::fromStdString(result.output).trimmed();
    if (!output.isEmpty()) {
        report << QStringLiteral("output:");
        for (const QString& line : output.split(QLatin1Char('\n'))) {
            report << QStringLiteral("  %1").arg(line);
        }
    }
    const UpstreamInfo upstream = m_session->upstreamInfo();
    report << QStringLiteral("upstream: %1")
                  .arg(upstream.configured ? QString::fromStdString(upstream.name)
                                           : QStringLiteral("(none)"));
    report << QStringLiteral("ahead: %1 behind: %2").arg(upstream.ahead).arg(upstream.behind);
    return report;
}

QStringList MainWindow::setUpstreamForDiagnostics(const QString& upstream) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const QString branch = QString::fromStdString(m_session->info().headBranch);
    QString error;
    const bool ok = m_session->setUpstream(branch, upstream, &error);
    refreshPanels();
    report << QStringLiteral("branch: %1").arg(branch);
    report << QStringLiteral("upstream: %1").arg(upstream);
    report << QStringLiteral("result: %1").arg(ok ? QStringLiteral("ok") : error);
    const UpstreamInfo info = m_session->upstreamInfo();
    report << QStringLiteral("configured: %1")
                  .arg(info.configured ? QString::fromStdString(info.name)
                                       : QStringLiteral("(none)"));
    report << QStringLiteral("ahead: %1 behind: %2").arg(info.ahead).arg(info.behind);
    return report;
}

QStringList MainWindow::autoFetchForDiagnostics(const QString& operation,
                                                const QStringList& remotes) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    if (operation == QStringLiteral("enable")) {
        setAutoFetch(true);
    } else if (operation == QStringLiteral("disable")) {
        setAutoFetch(false);
    } else if (operation == QStringLiteral("select")) {
        setAutoFetchRemotes(remotes);
    } else if (operation != QStringLiteral("now")) {
        report << QStringLiteral("unknown auto-fetch operation: %1").arg(operation);
        return report;
    }

    if (operation == QStringLiteral("now")) {
        const QStringList targets = remotes.isEmpty() ? m_autoFetchRemotes : remotes;
        const SyncResult result = m_session->fetchBlocking(targets, /*prune=*/true);
        refreshPanels();
        report << QStringLiteral("fetch: %1")
                      .arg(result.ok ? QStringLiteral("ok") : QStringLiteral("failed"));
        report << QStringLiteral("exit: %1").arg(result.exitCode);
        const QString output = QString::fromStdString(result.output).trimmed();
        if (!output.isEmpty()) {
            for (const QString& line : output.split(QLatin1Char('\n'))) {
                report << QStringLiteral("  %1").arg(line);
            }
        }
    }

    report << QStringLiteral("enabled: %1")
                  .arg(m_autoFetchEnabled ? QStringLiteral("yes") : QStringLiteral("no"));
    report << QStringLiteral("remotes: %1")
                  .arg(m_autoFetchRemotes.isEmpty() ? QStringLiteral("(all)")
                                                    : m_autoFetchRemotes.join(QLatin1Char(',')));
    return report;
}

QStringList MainWindow::conflictForDiagnostics(const QString& action, const QStringList& args) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }

    // Load the page only when it has nothing yet: reload() re-selects the first
    // file and resets every choice, and a headless run makes several calls in a
    // row. The window reloads on its own when the repository changes.
    if (m_conflictView->conflictCount() == 0) {
        m_conflictView->reload();
    }

    if (action == QStringLiteral("close")) {
        onConflictPageClosed();
    } else if (action == QStringLiteral("refresh")) {
        // The window's own refresh, which is what decides the page. A close has to
        // survive the pass that used to drag the reader back to the conflict page.
        m_commitModel->reset();
        refreshPanels();
    } else if (action == QStringLiteral("select") && !args.isEmpty()) {
        const bool ok = m_conflictView->selectFile(args.at(0));
        report << QStringLiteral("selected: %1")
                      .arg(ok ? args.at(0) : QStringLiteral("(not conflicted)"));
    } else if (action == QStringLiteral("take") && args.size() >= 2) {
        bool ok = false;
        const int region = args.at(0).toInt(&ok);
        const bool set = ok && m_conflictView->setRegionChoice(region, args.at(1));
        report << QStringLiteral("take: %1").arg(set ? QStringLiteral("ok")
                                                     : QStringLiteral("failed"));
    } else if (action == QStringLiteral("next")) {
        m_conflictView->goToNextRegion();
    } else if (action == QStringLiteral("prev")) {
        m_conflictView->goToPreviousRegion();
    } else if (action == QStringLiteral("save")) {
        QString error;
        const bool ok = m_conflictView->trySaveResolution(&error);
        report << QStringLiteral("save: %1")
                      .arg(ok ? QStringLiteral("ok") : error);
        m_commitModel->reset();
        refreshPanels();
    } else if (action == QStringLiteral("rerere-stage")) {
        QString error;
        const bool ok = m_conflictView->stageRecordedResolution(&error);
        report << QStringLiteral("rerere-stage: %1").arg(ok ? QStringLiteral("ok") : error);
        m_commitModel->reset();
        refreshPanels();
    } else if (action == QStringLiteral("stage-worktree")) {
        QString error;
        const bool ok = m_conflictView->stageWorkingTree(&error);
        report << QStringLiteral("stage-worktree: %1").arg(ok ? QStringLiteral("ok") : error);
        m_commitModel->reset();
        refreshPanels();
    } else if (action == QStringLiteral("mark-all")) {
        QStringList skipped;
        QString error;
        const bool ok = m_conflictView->markAllResolved(&skipped, &error);
        if (!ok) {
            report << QStringLiteral("mark-all: %1").arg(error);
        } else if (skipped.isEmpty()) {
            report << QStringLiteral("mark-all: ok");
        } else {
            report << QStringLiteral("mark-all: ok, skipped %1")
                          .arg(skipped.join(QLatin1Char(',')));
        }
        m_commitModel->reset();
        refreshPanels();
    } else if (action == QStringLiteral("resolve") && !args.isEmpty()) {
        // One shot for a single headless run: settle every region with one side and
        // save, since choices do not survive between processes.
        for (int i = 0; i < m_conflictView->regionCount(); ++i) {
            m_conflictView->setRegionChoice(i, args.at(0));
        }
        QString error;
        const bool ok = m_conflictView->trySaveResolution(&error);
        report << QStringLiteral("resolve %1: %2")
                      .arg(args.at(0), ok ? QStringLiteral("ok") : error);
        m_commitModel->reset();
        refreshPanels();
    } else if (action == QStringLiteral("keep") && !args.isEmpty()) {
        QString error;
        const bool ok = m_conflictView->keepSide(args.at(0), &error);
        report << QStringLiteral("keep %1: %2")
                      .arg(args.at(0), ok ? QStringLiteral("ok") : error);
        m_commitModel->reset();
        refreshPanels();
    } else if (action == QStringLiteral("smart")) {
        QString error;
        const bool ok = m_conflictView->smartMerge(&error);
        if (!ok) {
            report << QStringLiteral("smart: %1").arg(error);
        } else if (m_conflictView->regionCount() == 0) {
            QString saveError;
            const bool saved = m_conflictView->trySaveResolution(&saveError);
            report << QStringLiteral("smart: %1")
                          .arg(saved ? QStringLiteral("staged") : saveError);
            m_commitModel->reset();
            refreshPanels();
        } else {
            report << QStringLiteral("smart: %1 region(s) left")
                          .arg(m_conflictView->regionCount());
        }
    } else if (action == QStringLiteral("line") && args.size() >= 3) {
        bool regionOk = false;
        bool lineOk = false;
        const int region = args.at(0).toInt(&regionOk);
        const int line = args.at(2).toInt(&lineOk);
        const bool take = args.size() < 4 || args.at(3) != QStringLiteral("off");
        const bool ok = regionOk && lineOk &&
                        m_conflictView->setRegionLine(region, args.at(1), line, take);
        report << QStringLiteral("line: %1")
                      .arg(ok ? QStringLiteral("ok") : QStringLiteral("failed"));
    } else if (action == QStringLiteral("save-draft")) {
        QString error;
        const bool ok = m_conflictView->saveDraft(&error);
        report << QStringLiteral("save-draft: %1").arg(ok ? QStringLiteral("ok") : error);
    } else if (action == QStringLiteral("delete")) {
        QString error;
        const bool ok = m_conflictView->deleteCurrentFile(&error);
        report << QStringLiteral("delete: %1").arg(ok ? QStringLiteral("ok") : error);
        m_commitModel->reset();
        refreshPanels();
    } else if (action == QStringLiteral("clear-choices")) {
        m_conflictView->clearChoices();
        report << QStringLiteral("clear-choices: ok");
    } else if (action == QStringLiteral("unresolve") && !args.isEmpty()) {
        QString error;
        const bool ok = m_conflictView->markConflicted(args.at(0), &error);
        report << QStringLiteral("unresolve %1: %2").arg(args.at(0), ok ? QStringLiteral("ok") : error);
        m_commitModel->reset();
        refreshPanels();
    } else if (action == QStringLiteral("deleted") && !args.isEmpty()) {
        QString error;
        const QString living = m_conflictView->deleteModifyLivingSide();
        const bool ok = m_conflictView->resolveDeleteModify(args.at(0), &error);
        report << QStringLiteral("deleted %1 (changed on %2): %3")
                      .arg(args.at(0), living.isEmpty() ? QStringLiteral("-") : living,
                           ok ? QStringLiteral("ok") : error);
        m_commitModel->reset();
        refreshPanels();
    } else if (action == QStringLiteral("takelines") && args.size() >= 2) {
        bool regionOk = false;
        const int region = args.at(0).toInt(&regionOk);
        const bool take = args.size() < 3 || args.at(2) != QStringLiteral("off");
        const bool ok = regionOk && m_conflictView->takeAllRegionLines(region, args.at(1), take);
        report << QStringLiteral("takelines: %1")
                      .arg(ok ? QStringLiteral("ok") : QStringLiteral("failed"));
    } else if (action == QStringLiteral("auto")) {
        const RepoSession::AutoResolveOutcome autoPass =
            m_session->autoResolveTrivialConflicts();
        for (const QString& line : autoPass.resolvedDetail) {
            report << QStringLiteral("auto: %1").arg(line);
        }
        for (const QString& file : autoPass.stagedFiles) {
            report << QStringLiteral("auto-staged: %1").arg(file);
        }
        for (const QString& file : autoPass.needsAPerson) {
            report << QStringLiteral("auto-needs-person: %1").arg(file);
        }
        m_commitModel->reset();
        refreshPanels();
    } else if (action == QStringLiteral("text")) {
        const QStringList lines = m_conflictView->assembledText().split(QLatin1Char('\n'));
        for (const QString& line : lines) {
            report << QStringLiteral("| %1").arg(line);
        }
        return report;
    } else if (action != QStringLiteral("regions")) {
        report << QStringLiteral("unknown conflict action: %1").arg(action);
        return report;
    }

    report << QStringLiteral("file: %1").arg(m_conflictView->currentPath());
    report << QStringLiteral("page: %1")
                  .arg(m_stack && m_stack->currentIndex() == kConflictsPage
                           ? QStringLiteral("CONFLICT")
                           : QStringLiteral("history"));
    report << QStringLiteral("kind: %1").arg(m_conflictView->currentKind());
    const QStringList regions = m_conflictView->describeRegions();
    for (const QString& line : regions) {
        report << line;
    }
    report << QStringLiteral("regions: %1").arg(m_conflictView->regionCount());
    report << QStringLiteral("unresolved: %1").arg(m_conflictView->unresolvedRegionCount());
    report << QStringLiteral("binary: %1")
                  .arg(m_conflictView->isBinary() ? QStringLiteral("yes") : QStringLiteral("no"));
    report << QStringLiteral("delete-modify: %1")
                  .arg(m_conflictView->isDeleteModify() ? QStringLiteral("yes")
                                                        : QStringLiteral("no"));
    report << QStringLiteral("current: %1").arg(m_conflictView->currentRegion());
    report << QStringLiteral("conflicts left: %1").arg(m_conflictView->conflictCount());
    report << QStringLiteral("resolved: %1")
                  .arg(m_conflictView->resolvedPaths().isEmpty()
                           ? QStringLiteral("-")
                           : m_conflictView->resolvedPaths().join(QStringLiteral(", ")));
    report << QStringLiteral("rerere: %1")
                  .arg(m_conflictView->rerereApplied()
                           ? QStringLiteral("applied")
                           : (m_conflictView->rerereRemembered() ? QStringLiteral("remembered")
                                                                 : QStringLiteral("none")));
    return report;
}

QStringList MainWindow::mergePreviewForDiagnostics(const QString& ours, const QString& theirs) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    QString error;
    const MergePreview preview = m_session->previewMerge(ours, theirs, &error);
    if (!error.isEmpty()) {
        report << QStringLiteral("error: %1").arg(error);
        return report;
    }
    report << QStringLiteral("available: %1")
                  .arg(preview.available ? QStringLiteral("yes") : QStringLiteral("no"));
    report << QStringLiteral("clean: %1")
                  .arg(preview.clean ? QStringLiteral("yes") : QStringLiteral("no"));
    report << QStringLiteral("conflicts: %1").arg(preview.conflictedPaths.size());
    for (const std::string& path : preview.conflictedPaths) {
        report << QStringLiteral("path: %1").arg(QString::fromStdString(path));
        report << QStringLiteral("where: %1").arg(describePredictedConflict(preview, path));
    }
    return report;
}

QStringList MainWindow::squashMergeForDiagnostics(const QString& source, const QString& target) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    squashMergeBranch(source, target, true);

    int staged = 0;
    for (const FileChange& change : m_session->status()) {
        if (change.staged) {
            ++staged;
        }
    }
    report << QStringLiteral("head: %1")
                  .arg(QString::fromStdString(m_session->info().headId).left(10));
    report << QStringLiteral("squash open: %1")
                  .arg(m_session->squashInProgress() ? QStringLiteral("yes") : QStringLiteral("no"));
    report << QStringLiteral("merge open: %1")
                  .arg(m_session->mergeInProgress() ? QStringLiteral("yes") : QStringLiteral("no"));
    report << QStringLiteral("conflicts: %1").arg(m_session->conflicts().size());
    report << QStringLiteral("staged: %1").arg(staged);
    report << QStringLiteral("page: %1")
                  .arg(m_stack && m_stack->currentWidget() == m_conflictView
                           ? QStringLiteral("CONFLICT")
                           : QStringLiteral("history"));
    report << QStringLiteral("status: %1")
                  .arg(statusBar() ? statusBar()->currentMessage() : QString());
    return report;
}

QStringList MainWindow::visibilityForDiagnostics(const QString& operation, const QString& branch) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }

    if (operation == QStringLiteral("hide")) {
        m_visibility.setHidden(branch, true);
    } else if (operation == QStringLiteral("unhide")) {
        m_visibility.setHidden(branch, false);
    } else if (operation == QStringLiteral("solo")) {
        m_visibility.setSolo(branch, true);
    } else if (operation == QStringLiteral("unsolo")) {
        m_visibility.setSolo(branch, false);
    } else if (operation == QStringLiteral("pin")) {
        m_pins.setPinned(branch, true);
    } else if (operation == QStringLiteral("unpin")) {
        m_pins.setPinned(branch, false);
    } else if (operation == QStringLiteral("show")) {
        m_visibility.clear();
    } else if (operation == QStringLiteral("smart")) {
        m_visibility.setSmart(true);
    } else if (operation == QStringLiteral("smart-off")) {
        m_visibility.setSmart(false);
    } else if (operation.startsWith(QLatin1String("preset:"))) {
        const QString id = operation.mid(QStringLiteral("preset:").size());
        VisibilityPreset preset = VisibilityPreset::None;
        if (id == QLatin1String("mine")) {
            preset = VisibilityPreset::Mine;
        } else if (id == QLatin1String("recent")) {
            preset = VisibilityPreset::Recent30;
        } else if (id == QLatin1String("prefix")) {
            preset = VisibilityPreset::Prefix;
        } else if (id == QLatin1String("no-upstream")) {
            preset = VisibilityPreset::NoUpstream;
        } else if (id == QLatin1String("ahead")) {
            preset = VisibilityPreset::Ahead;
        } else if (id == QLatin1String("behind")) {
            preset = VisibilityPreset::Behind;
        } else {
            report << QStringLiteral("unknown preset: %1").arg(id);
            return report;
        }
        m_visibility.setSmart(false);
        m_visibility.setPreset(preset);
    } else if (operation.startsWith(QLatin1String("preset-prefix:"))) {
        m_visibility.setPresetPrefix(operation.mid(QStringLiteral("preset-prefix:").size()));
        m_visibility.setSmart(false);
        m_visibility.setPreset(VisibilityPreset::Prefix);
    } else if (operation == QStringLiteral("preset-off")) {
        m_visibility.setPreset(VisibilityPreset::None);
    } else if (operation == QStringLiteral("merged-commits") ||
               operation == QStringLiteral("merged-commits-off")) {
        m_mergedCommitsAction->setChecked(operation == QStringLiteral("merged-commits"));
    } else if (operation != QStringLiteral("report")) {
        report << QStringLiteral("unknown visibility operation: %1").arg(operation);
        return report;
    }
    refreshPanels();

    // Reported through the filter's own path, synchronously: the model loads its
    // page on a worker, and a headless check must not race it.
    const HistoryFilter filter = historyFilter();
    bool hasMore = false;
    const auto rows = m_session->commitsBlocking(0, 200000, &hasMore, filter);
    const auto labels = m_session->refsByCommit(filter);
    std::vector<std::string> names;
    for (const auto& [oid, bucket] : labels) {
        for (const RefInfo& ref : bucket) {
            names.push_back(ref.shortName);
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    report << QStringLiteral("rows: %1").arg(rows.size());
    report << QStringLiteral("merged commits: %1")
                  .arg(filter.firstParentHistory ? QStringLiteral("merge only")
                                                 : QStringLiteral("shown"));
    report << QStringLiteral("labels: %1").arg(names.size());
    for (const std::string& name : names) {
        report << QStringLiteral("label: %1").arg(QString::fromStdString(name));
    }
    report << QStringLiteral("solo: %1")
                  .arg(m_visibility.soloActive() ? QStringLiteral("yes") : QStringLiteral("no"));
    report << QStringLiteral("hidden: %1").arg(m_visibility.hiddenNames().size());
    report << QStringLiteral("smart: %1")
                  .arg(m_visibility.smart() ? QStringLiteral("yes") : QStringLiteral("no"));
    const QString target = m_session->targetBranch();
    report << QStringLiteral("target: %1")
                  .arg(target.isEmpty() ? QStringLiteral("(none)") : target);
    report << QStringLiteral("preset: %1")
                  .arg(m_visibility.preset() == VisibilityPreset::None
                           ? QStringLiteral("(none)")
                           : presetLabel(m_visibility.preset()));
    report << QStringLiteral("pinned: %1").arg(m_pins.names().join(QStringLiteral(", ")));
    report << QStringLiteral("indicator: %1")
                  .arg(m_visibilityButton && m_visibilityButton->isVisible()
                           ? m_visibilityButton->text()
                           : QStringLiteral("(hidden)"));
    return report;
}

void MainWindow::loadGraphViewOptions() {
    const SettingsStore store;
    const auto apply = [](QAction* action, bool on) {
        if (action && action->isChecked() != on) {
            action->setChecked(on);
        }
    };
    apply(m_mergedCommitsAction,
          store.boolValue(SettingsStore::globalKey(QStringLiteral("showMergedCommits")), true));
    apply(m_mergedRemotesAction,
          store.boolValue(SettingsStore::globalKey(QStringLiteral("showMergedRemotes")), false));
}

void MainWindow::loadGraphAnimation() {
    const bool on =
        SettingsStore().boolValue(SettingsStore::globalKey(QStringLiteral("graph/animate")), true);
    if (m_animateAction && m_animateAction->isChecked() != on) {
        const QSignalBlocker block(m_animateAction);
        m_animateAction->setChecked(on);
    }
    if (m_commitView) {
        m_commitView->setAnimationsEnabled(on);
    }
}

void MainWindow::setGraphAnimationsEnabled(bool on) {
    if (m_commitView) {
        m_commitView->setAnimationsEnabled(on);
    }
    SettingsStore().setBool(SettingsStore::globalKey(QStringLiteral("graph/animate")), on);
}

QStringList MainWindow::runGraphAnimationOp(const QString& operation, const QString& argument) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    if (operation == QStringLiteral("commit")) {
        report += commitPanelForDiagnostics(QStringList(), argument);
    } else if (operation == QStringLiteral("checkout")) {
        checkoutBranchFromGraph(argument, RefKind::LocalBranch);
        report << QStringLiteral("checkout %1").arg(argument);
    } else if (operation == QStringLiteral("branch")) {
        QString error;
        const QString head = QString::fromStdString(m_session->info().headId);
        if (!m_session->createBranch(argument, head, &error)) {
            report << QStringLiteral("error: %1").arg(error);
        } else {
            m_commitModel->reset();
            refreshPanels();
            report << QStringLiteral("branch %1").arg(argument);
        }
    } else {
        report << QStringLiteral("unknown animation operation: %1").arg(operation);
    }
    return report;
}

QStringList MainWindow::graphAnimationReport() const {
    return m_commitView ? m_commitView->lastAnimationTrace() : QStringList{};
}

QStringList MainWindow::branchGroupForDiagnostics(const QString& operation,
                                                  const QStringList& args) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }

    const auto reportResult = [&report](const QString& verb,
                                        const RepoSession::BranchBatchResult& result) {
        report << QStringLiteral("%1: %2 done, %3 failed")
                      .arg(verb)
                      .arg(result.done.size())
                      .arg(result.failed.size());
        for (const QString& name : result.done) {
            report << QStringLiteral("  done %1").arg(name);
        }
        for (const QString& line : result.failed) {
            report << QStringLiteral("  failed %1").arg(line);
        }
    };

    if (operation == QStringLiteral("delete")) {
        RepoSession::BranchBatchResult result;
        QString error;
        // A headless run has nobody to show the consent dialog to; the caller named
        // the branches, which is the consent here.
        if (!m_session->deleteBranches(args, QString(), /*forceUnmerged=*/true, &result, &error)) {
            report << error;
            return report;
        }
        reportResult(QStringLiteral("delete"), result);
        refreshRefs();
        refreshPanels();
    } else if (operation == QStringLiteral("delete-merged")) {
        const QString target = m_session->targetBranch();
        const QStringList names = m_session->branchesMergedInto(target);
        report << QStringLiteral("target: %1")
                      .arg(target.isEmpty() ? QStringLiteral("(HEAD)") : target);
        RepoSession::BranchBatchResult result;
        QString error;
        if (!names.isEmpty() &&
            !m_session->deleteBranches(names, target, /*forceUnmerged=*/false, &result, &error)) {
            report << error;
            return report;
        }
        reportResult(QStringLiteral("delete-merged"), result);
        refreshRefs();
        refreshPanels();
    } else if (operation == QStringLiteral("set-upstream")) {
        if (args.size() < 2) {
            report << QStringLiteral("set-upstream needs <remote> <branch…>");
            return report;
        }
        RepoSession::BranchBatchResult result;
        QString error;
        if (!m_session->setUpstreams(args.mid(1), args.front(), &result, &error)) {
            report << error;
            return report;
        }
        reportResult(QStringLiteral("set-upstream"), result);
        refreshRefs();
    } else if (operation == QStringLiteral("unmerged")) {
        const QString base = args.isEmpty() ? QString() : args.front();
        QStringList all;
        for (const RefInfo& ref : m_session->refs()) {
            if (ref.kind == RefKind::LocalBranch) {
                all << QString::fromStdString(ref.shortName);
            }
        }
        report << QStringLiteral("base: %1")
                      .arg(base.isEmpty() ? QStringLiteral("(HEAD)") : base);
        const QStringList unmerged = m_session->unmergedAmong(all, base);
        if (unmerged.isEmpty()) {
            report << QStringLiteral("(none)");
        } else {
            report << unmerged;
        }
    } else if (operation == QStringLiteral("hide") || operation == QStringLiteral("solo")) {
        const bool hiding = operation == QStringLiteral("hide");
        for (const QString& name : args) {
            if (hiding) {
                m_visibility.setHidden(name, true);
            } else {
                m_visibility.setSolo(name, true);
            }
        }
        report << QStringLiteral("%1: %2").arg(operation, args.join(QLatin1Char(' ')));
        applyVisibility();
        refreshRefs();
    } else {
        report << QStringLiteral("unknown branch group operation: %1").arg(operation);
    }
    return report;
}

QStringList MainWindow::patchForDiagnostics(const QString& file) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    bool mailbox = false;
    QString error;
    if (!m_session->applyPatch(file, &mailbox, &error)) {
        report << QStringLiteral("failed: %1").arg(error.trimmed());
        return report;
    }
    report << QStringLiteral("applied %1 as %2")
                  .arg(file, mailbox ? QStringLiteral("commits (git am)")
                                     : QStringLiteral("a working-tree change (git apply)"));
    refreshPanels();
    return report;
}

QStringList MainWindow::maintenanceForDiagnostics(const QString& operation) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    MaintenanceTask task = MaintenanceTask::Gc;
    if (!maintenanceTaskFromName(operation.toStdString(), &task)) {
        report << QStringLiteral("unknown maintenance task: %1").arg(operation);
        return report;
    }
    const SyncResult result = m_session->maintenanceBlocking(task);
    report << QStringLiteral("%1: exit %2").arg(operation).arg(result.exitCode);
    const QString output = QString::fromStdString(result.output).trimmed();
    if (!output.isEmpty()) {
        report << output;
    } else {
        report << QStringLiteral("(no output)");
    }
    return report;
}

QStringList MainWindow::hooksForDiagnostics(const QString& operation, const QStringList& args) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }

    if (operation == QStringLiteral("list") || operation == QStringLiteral("path")) {
        if (operation == QStringLiteral("path")) {
            report << QStringLiteral("path: %1").arg(m_session->hooksPath());
        }
        for (const HookEntry& hook : m_session->hooks()) {
            report << QStringLiteral("%1: %2")
                          .arg(QString::fromStdString(hook.name),
                               QString::fromStdString(hookStateName(hook.state)));
        }
    } else if (operation == QStringLiteral("enable") || operation == QStringLiteral("disable")) {
        const QString name = args.value(0);
        if (name.isEmpty()) {
            report << QStringLiteral("a hook name is required");
            return report;
        }
        QString error;
        const bool ok = operation == QStringLiteral("enable")
                            ? m_session->enableHook(name, &error)
                            : m_session->disableHook(name, &error);
        report << QStringLiteral("%1 %2: %3")
                      .arg(operation, name, ok ? QStringLiteral("ok") : error);
    } else if (operation == QStringLiteral("show")) {
        const QString name = args.value(0);
        const QString content = m_session->hookContent(name).trimmed();
        report << (content.isEmpty() ? QStringLiteral("(empty)") : content);
    } else {
        report << QStringLiteral("unknown hooks operation: %1").arg(operation);
    }
    return report;
}

QStringList MainWindow::signingForDiagnostics(const QString& operation, const QStringList& args) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const QString value = args.value(0);
    if (operation == QStringLiteral("status")) {
        report << QStringLiteral("sign: %1")
                      .arg(m_session->commitSigningEnabled() ? QStringLiteral("true")
                                                             : QStringLiteral("false"));
        report << QStringLiteral("key: %1")
                      .arg(m_session->configValue(QStringLiteral("user.signingkey")));
        report << QStringLiteral("format: %1")
                      .arg(m_session->configValue(QStringLiteral("gpg.format")));
        report << QStringLiteral("program: %1")
                      .arg(m_session->configValue(QStringLiteral("gpg.program")));
    } else if (operation == QStringLiteral("enable") || operation == QStringLiteral("disable")) {
        QString error;
        const bool ok = m_session->setConfigValue(
            QStringLiteral("commit.gpgsign"),
            operation == QStringLiteral("enable") ? QStringLiteral("true") : QStringLiteral("false"),
            &error);
        report << QStringLiteral("%1: %2").arg(operation, ok ? QStringLiteral("ok") : error);
    } else if (operation == QStringLiteral("key") || operation == QStringLiteral("format")) {
        const QString key = operation == QStringLiteral("key") ? QStringLiteral("user.signingkey")
                                                               : QStringLiteral("gpg.format");
        QString error;
        const bool ok = m_session->setConfigValue(key, value, &error);
        report << QStringLiteral("%1: %2").arg(operation, ok ? QStringLiteral("ok") : error);
    } else {
        report << QStringLiteral("unknown signing operation: %1").arg(operation);
    }
    if (m_details) {
        m_details->setSignCommit(m_session->commitSigningEnabled());
    }
    return report;
}

QStringList MainWindow::verifySignatureForDiagnostics(const QString& revision) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }
    const CommitSignature signature = m_session->commitSignature(revision);
    report << QStringLiteral("status: %1").arg(QString::fromStdString(signature.status));
    report << QStringLiteral("signer: %1").arg(QString::fromStdString(signature.signer));
    report << QStringLiteral("key: %1").arg(QString::fromStdString(signature.key));
    return report;
}

QStringList MainWindow::rerereForDiagnostics(const QString& operation, const QString& path) {
    QStringList report;
    if (!m_session || !m_session->isOpen()) {
        report << QStringLiteral("no repository is open");
        return report;
    }

    if (operation == QStringLiteral("enable") || operation == QStringLiteral("disable")) {
        QString error;
        const bool ok = m_session->setRerereEnabled(operation == QStringLiteral("enable"), &error);
        report << QStringLiteral("%1: %2").arg(operation, ok ? QStringLiteral("ok") : error);
    } else if (operation == QStringLiteral("autoupdate-on") ||
               operation == QStringLiteral("autoupdate-off")) {
        QString error;
        const bool ok = m_session->setRerereAutoupdate(
            operation == QStringLiteral("autoupdate-on"), &error);
        report << QStringLiteral("%1: %2").arg(operation, ok ? QStringLiteral("ok") : error);
    } else if (operation == QStringLiteral("apply")) {
        QString error;
        const bool ok = m_session->applyRerere(&error);
        report << QStringLiteral("apply: %1").arg(ok ? QStringLiteral("ok") : error);
        m_commitModel->reset();
        refreshPanels();
    } else if (operation == QStringLiteral("forget") && !path.isEmpty()) {
        QString error;
        const bool ok = m_session->forgetRerere(path, &error);
        report << QStringLiteral("forget: %1").arg(ok ? QStringLiteral("ok") : error);
    } else if (operation != QStringLiteral("status")) {
        report << QStringLiteral("unknown rerere operation: %1").arg(operation);
        return report;
    }

    report << QStringLiteral("enabled: %1")
                  .arg(m_session->rerereEnabled() ? QStringLiteral("yes") : QStringLiteral("no"));
    report << QStringLiteral("autoupdate: %1")
                  .arg(m_session->rerereAutoupdate() ? QStringLiteral("yes") : QStringLiteral("no"));
    for (const QString& tracked : m_session->rerereTrackedPaths()) {
        report << QStringLiteral("tracked: %1").arg(tracked);
    }
    for (const QString& remaining : m_session->rerereRemainingPaths()) {
        report << QStringLiteral("remaining: %1").arg(remaining);
    }
    return report;
}

void MainWindow::startSync(SyncKind kind, bool silent) {
    if (m_syncRunning) {
        setStatus(QStringLiteral("A network operation is already running."));
        return;
    }
    if (m_remotes.empty()) {
        QMessageBox::information(this, QStringLiteral("No remote"),
                                 QStringLiteral("This repository has no remote configured."));
        return;
    }

    const QString remote = currentSyncRemote();
    const QString branch = QString::fromStdString(m_session->info().headBranch);
    QString label;
    switch (kind) {
    case SyncKind::Fetch: label = QStringLiteral("Fetch"); break;
    case SyncKind::FetchAll: label = QStringLiteral("Fetch all"); break;
    case SyncKind::Pull: label = QStringLiteral("Pull"); break;
    case SyncKind::Push: label = QStringLiteral("Push"); break;
    case SyncKind::PushForce: label = QStringLiteral("Force push"); break;
    case SyncKind::PushTags: label = QStringLiteral("Push tags"); break;
    }

    m_silentSync = silent;
    m_syncRunning = true;
    updateSyncActions();
    if (m_commitView) {
        m_commitView->setBusy(true);
    }
    if (kind == SyncKind::FetchAll) {
        setStatus(QStringLiteral("Fetching every remote…"));
    } else {
        setStatus(QStringLiteral("%1 from %2…").arg(label, remote));
    }
    m_session->requestSync(kind, remote, branch, m_pullMode);
}

void MainWindow::onSyncFinished(bool ok, QString summary, QString output) {
    const bool silent = m_silentSync;
    m_silentSync = false;
    m_syncRunning = false;
    if (m_commitView) {
        m_commitView->setBusy(false);
    }
    m_commitModel->reset();
    refreshPanels();

    const QString trimmed = output.trimmed();
    const QString lastLine = trimmed.isEmpty()
                                 ? QStringLiteral("done")
                                 : trimmed.section(QLatin1Char('\n'), -1).trimmed();
    if (!ok) {
        // A background auto-fetch that fails must not throw a modal over the work
        // the reader is doing; the status bar is where a background failure belongs.
        if (silent) {
            notify(NotificationLog::Warning, QStringLiteral("%1 failed: %2").arg(summary, lastLine));
            return;
        }
        notify(NotificationLog::Error, QStringLiteral("%1 failed.").arg(summary));
        QMessageBox::warning(this, QStringLiteral("%1 failed").arg(summary),
                             trimmed.isEmpty() ? QStringLiteral("git reported no output.") : trimmed);
        return;
    }

    // git prints progress on stderr, so a quiet success often has no output.
    notify(NotificationLog::Success, QStringLiteral("%1: %2").arg(summary, lastLine));

    // A fetch or pull is what moves a recorded gitlink under the reader's feet, so
    // the submodule auto-update hook runs here. Push and tags do not.
    if (summary == QStringLiteral("fetch") || summary == QStringLiteral("fetch all") ||
        summary == QStringLiteral("pull")) {
        maybeAutoUpdateSubmodules();
        // A fetch is exactly when the base branch may have moved under the reader.
        // The check stays quiet unless it finds something.
        if (m_preventionEnabled) {
            checkConflictsWithBase(true);
        }
    }
}

} // namespace gk
