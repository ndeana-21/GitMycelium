#pragma once

#include "core/selection_plan.h"
#include "core/types.h"
#include "model/commit_model.h"
#include "model/ref_pins.h"
#include "model/ref_visibility.h"
#include "ui/commit_details_panel.h"
#include "ui/command_palette.h"
#include "ui/diff_view.h"
#include "ui/graph_columns.h"
#include "ui/notification_log.h"
#include "ui/profiles.h"
#include "ui/keymap.h"
#include "ui/stash_dialog.h"
#include "model/ref_model.h"
#include "repo/repo_session.h"

#include <QHash>
#include <QMainWindow>

#include <functional>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class QAction;
class QLabel;
class QPoint;
class QShortcut;
class QStackedWidget;
class QTextEdit;
class QTreeView;
class QTreeWidget;

class QDockWidget;
class QLineEdit;
class QToolButton;
class QToolBar;
class QComboBox;
class QMenu;
class QTimer;
class QTabBar;
namespace gk {
class PanelSplitter;
}

namespace gk {
class Skeleton;
namespace update {
class Updater;
}


class CommitModel;
class CommitGraphView;
class ConflictView;
class RefModel;
class RefTreeView;
class RepoSession;
class RepoWatcher;
class StartPage;

/// Offered as a menu so the operation is always explicit — dragging must never
/// silently rewrite history.
enum class RefDropAction { MergeInto, RebaseOnto, Checkout };

struct CommitDetails {
    std::vector<ChangedFile> files;
    bool amendable = false;
    CommitSignature signature;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void setGraphColumnWidth(int width);

    bool openRepository(const QString& path);

    void selectFirstCommit();
    void selectCommitRow(int row);
    void showDiffPage();
    void showGraphPage();
    void setRefsPanelVisible(bool visible);
    bool diffPageVisible() const;
    bool refsPanelVisible() const;
    void setDiffFileView(bool on);
    void setDiffSplitView(bool on);
    void setDiffBlameView(bool on);
    void setDiffWrap(bool on);
    void openFirstFileDiff();

    /// One line per entry, in order, each prefixed `enabled`/`disabled` and followed by
    /// its action key; submenu entries are indented and their parent marked " ▸".
    QStringList commitMenuEntries(const QString& commitId) const;

    bool askYes(const QString& title, const QString& text);
    void reportFailure(const QString& title, const QString& text);

    QStringList commitMenuEntriesForFirstLabelledCommit() const;

    /// Runs the window's own rebase path and reports what the window shows afterwards:
    /// the status line, the centre page and the conflicted files. The engine agreeing is
    /// not the same thing as the window agreeing.
    QStringList rebaseForDiagnostics(const QString& branch, const QString& onto);

    /// Stages the given paths (every change when the list is empty) and commits
    /// `message` through the calls the panel's buttons reach.
    QStringList commitPanelForDiagnostics(const QStringList& paths, const QString& message);

    std::string currentBranchName() const;

    /// Confirmations answer themselves and modal dialogs are closed as they appear: a
    /// modal wait hangs a run forever, which is how a missing guard looked like
    /// "nothing happens".
    void setHeadless(bool headless);
    bool headless() const { return m_headless; }

    /// Off by default so a test or one-shot run does not rewrite a reader's profile;
    /// the application turns it on for an interactive launch.
    void setTabsPersistenceEnabled(bool on) { m_tabsPersistence = on; }
    void setBackgroundIndexingEnabled(bool on);
    /// Off by default: only an interactive launch asks the release page for a newer
    /// build (at most once a day, and only while the preference allows it).
    void setUpdateChecksEnabled(bool on);
    /// Off by default, so a test or a headless check reads synchronously and sees the
    /// result of what it just did. The interactive launch turns it on: refreshes read
    /// on a worker, and what is not read yet shows as skeleton rows.
    void setAsyncReadsEnabled(bool on);
    QStringList loadingStateForDiagnostics() const;
    void showUpdateDialog();
    /// Opens only the active one; a session with several monorepos must not read them all
    /// at startup.
    void restoreTabs();

    void setAutoFetch(bool enabled);
    bool autoFetchEnabled() const { return m_autoFetchEnabled; }
    void setAutoFetchRemotes(const QStringList& remotes);
    QStringList autoFetchRemotes() const { return m_autoFetchRemotes; }
    /// Test seam: the production interval is five minutes, which no test will wait.
    void setAutoFetchIntervalMs(int ms);

    /// Off by default: it is an extra git call, and the merge would find the same
    /// conflicts on its own.
    void setPreflightEnabled(bool enabled);
    bool preflightEnabled() const { return m_preflightEnabled; }
    /// `silent` keeps it to the status bar and its tooltip — the shape a check that ran
    /// on its own after a fetch must take.
    void checkConflictsWithBase(bool silent);
    void setPreventionEnabled(bool enabled);
    bool preventionEnabled() const { return m_preventionEnabled; }
    QStringList preventionForDiagnostics(const QString& op);

    /// Off by default: it can hit the network and move nested checkouts, which is not
    /// something to do unasked.
    void setSubmoduleAutoupdate(bool enabled);
    void setSubmoduleAutoupdateRemote(bool remote);
    bool submoduleAutoupdateEnabled() const { return m_submoduleAutoupdate; }
    bool submoduleAutoupdateRemote() const { return m_submoduleAutoupdateRemote; }

    /// `rerere.enabled` and its automatic staging. The git config is the source of truth,
    /// read back on every repository open.
    void setRerereEnabled(bool on);
    void setRerereAutoupdate(bool on);
    bool rerereEnabled() const;

    enum class SelectionOperation { Drop, Squash, CherryPick };

    SelectionPlan selectionPlan() const;

    /// Runs `action` once the model has rows. A selection made before the first page
    /// lands is wiped by the reset that brings it in, which made a multi-row check
    /// report a single row.
    void whenCommitsAreLoaded(const std::function<void()>& action, int attempt = 0);

    bool commitsAreLoaded() const;

    QString rowsToString(const QVector<int>& rows) const;

    QStringList m_lastMenuReport;

    QString rootBelow(const std::vector<Oid>& commits) const;

    QString rebaseBaseBelow(const QString& commitId) const;

    void runSelectionOperation(SelectionOperation operation);

    QStringList runSelectionForDiagnostics(const QVector<int>& rows, const QString& operation);
    QStringList contextMenuForDiagnostics(const QVector<int>& rows, int menuRow);
    QStringList describeSelection(const QVector<int>& rows);
    QStringList describeHoverRow(int row);
    QStringList hoverExpandForDiagnostics(int row);
    QStringList activateHoverGhostForDiagnostics(int row);
    /// One line about the history model: rows held, a page in flight, more to come.
    /// `--ui-load-trace` samples it while the window idles.
    QString loadStateForDiagnostics() const;
    /// `show` reports the column mode and layout; `rich`/`text`/`compact` switch the mode.
    QStringList columnsForDiagnostics(const QString& op);
    /// `show` lists the date separators in the loaded graph; `none|hour|day|week|month|year`
    /// switches the grouping.
    QStringList timelineForDiagnostics(const QString& op);
    /// `show` reports the sparse-checkout state and cone; `set <dir,dir>` applies one
    /// (empty disables), `disable` clears it.
    QStringList sparseForDiagnostics(const QString& op, const QString& argument);
    /// `list` prints the profiles and the active one; `use <name>` applies one,
    /// `add <name>|<user>|<email>[|<key>]` defines one, `remove` drops one.
    QStringList profilesForDiagnostics(const QString& op, const QString& argument);
    /// `show` reports the gitflow config; `init|master|develop`, `start|kind|name` and
    /// `finish|kind|name[|version]` drive it.
    QStringList gitflowForDiagnostics(const QString& op, const QString& argument);
    QStringList notificationsForDiagnostics(const QString& op);
    QStringList dateFormatForDiagnostics(const QString& key);
    /// `show` reports the LFS state; `init|track|untrack|checkout|prune` act on it;
    /// `fetch|pull|push` run the network path.
    QStringList lfsForDiagnostics(const QString& op, const QString& argument);

    QStringList worktreeForDiagnostics(const QString& operation, const QStringList& args);

    QStringList submoduleForDiagnostics(const QString& operation, const QStringList& args);

    /// `--shot` uses it to keep the captured frame free of a mid-flight fade.
    void setGraphAnimationsEnabled(bool on);
    /// Performs one operation that changes the graph (`commit`, `checkout`), so the report
    /// that follows has transitions to show.
    QStringList runGraphAnimationOp(const QString& operation, const QString& argument);
    QStringList graphAnimationReport() const;
    /// A group branch operation (`delete`, `delete-merged`, `set-upstream`, `hide`,
    /// `solo`) on explicit names rather than on a selection nobody can click headlessly.
    QStringList branchGroupForDiagnostics(const QString& operation, const QStringList& args);

    QStringList patchForDiagnostics(const QString& file);
    /// Print (`args` empty) or run (`args` = a plan spec) an interactive rebase over
    /// `base..HEAD`; the `--onto`/branch form is not exposed here.
    QStringList rebaseEditorForDiagnostics(const QString& base, const QStringList& args);
    QStringList rebaseAmendForDiagnostics(const QString& message);
    /// `mergeBase` asks for the three-dot form; a `target` of `//wip` compares `base`
    /// with the working tree.
    QStringList compareForDiagnostics(const QString& base, const QString& target,
                                      const QString& path, bool mergeBase = false);
    QStringList restoreFileForDiagnostics(const QString& revision, const QString& path,
                                          bool staged);
    QStringList blameForDiagnostics(const QString& path, const QString& revision);
    QStringList fileHistoryForDiagnostics(const QString& path);
    QStringList reflogForDiagnostics(int limit);
    QStringList maintenanceForDiagnostics(const QString& operation);

    QStringList hooksForDiagnostics(const QString& operation, const QStringList& args);

    /// The configuration is `status`, `enable`, `disable`, `key <id>`, `format <openpgp|ssh>`.
    QStringList signingForDiagnostics(const QString& operation, const QStringList& args);
    QStringList verifySignatureForDiagnostics(const QString& revision);
    QStringList navigateForDiagnostics(int row, const QString& direction);

    /// Runs one commit-menu entry through the path the context menu and the keyboard
    /// both reach, returning the state afterwards so a run can be compared with git.
    QStringList runCommitMenuEntry(const QString& key, const QString& revision);

    /// One line per command the palette offers: `enabled/disabled id  title`. With
    /// `runFirst`, the best match is run and its id reported.
    QStringList commandPaletteForDiagnostics(const QString& query, bool runFirst);

    QStringList keymapForDiagnostics(const QString& operation, const QStringList& args);
    /// The combination a command is actually bound to, or empty. Reads the binding the
    /// window installed, not what the keymap would say.
    QString commandSequence(const QString& id) const;

    /// With `revealOid`, also pages the graph until the commit is loaded and reports
    /// the row it landed on.
    QStringList searchForDiagnostics(const QString& query, const QString& field,
                                     const QString& revealOid = QString());

    /// `args` carries `partial`, `branch=<name>` and `depth=<n>`.
    QStringList cloneForDiagnostics(const QString& url, const QString& destination,
                                    const QStringList& args);

    /// `scope` is `gitignore`/`exclude`, `kind` `file`/`ext`/`dir`.
    QStringList ignoreForDiagnostics(const QString& path, const QString& scope,
                                     const QString& kind);
    QStringList coauthorForDiagnostics(const QString& name, const QString& email);
    QStringList commitTemplateForDiagnostics();

    /// `get` reports the configured external tools; `set` writes one. `kind` is `diff`
    /// or `merge`.
    QStringList externalToolForDiagnostics(const QString& op, const QString& kind,
                                           const QString& name);
    QStringList runExternalToolForDiagnostics(const QString& kind, const QString& base,
                                              const QString& target, const QString& path);
    QStringList terminalForDiagnostics();

    QStringList tabsForDiagnostics(const QString& operation, const QStringList& args);

    QStringList recentForDiagnostics(const QString& operation, const QStringList& args);
    QStringList startPageForDiagnostics() const;
    QStringList createRepositoryForDiagnostics(const QString& path, const QString& branch);

    QStringList undoForDiagnostics();
    QStringList redoForDiagnostics();
    QStringList oplogForDiagnostics() const;

    QStringList stageHunkForDiagnostics(const QString& path, int hunkIndex, bool unstage);
    QStringList stageLineForDiagnostics(const QString& path, int hunkIndex, int lineIndex,
                                        bool unstage);
    QStringList discardHunkForDiagnostics(const QString& path, int hunkIndex, int lineIndex);

    QStringList stashForDiagnostics(const QString& operation, const QString& argument);

    /// One of `fetch`, `fetch-all`, `pull`, `pull-merge`, `pull-rebase`, `push`,
    /// `push-tags`, `push-tag`.
    QStringList syncForDiagnostics(const QString& operation, const QString& argument);
    bool syncRunningForDiagnostics() const { return m_syncRunning; }
    QStringList setUpstreamForDiagnostics(const QString& upstream);

    QStringList autoFetchForDiagnostics(const QString& operation, const QStringList& remotes);

    /// One preference by logical name (`preflight`, `autofetch.enabled`,
    /// `autofetch.remotes`, `submodule.autoupdate`, `submodule.autoupdate-remote`,
    /// `graph.animate`, `rerere.enabled`, `rerere.autoupdate`).
    QStringList preferencesForDiagnostics(const QString& operation, const QString& key,
                                          const QString& value);

    /// Drives the conflict editor — list the regions, take a side, navigate, select a
    /// file, print the assembled text, or save — through the page's own calls.
    QStringList conflictForDiagnostics(const QString& action, const QStringList& args);

    QStringList mergePreviewForDiagnostics(const QString& ours, const QString& theirs);

    QStringList squashMergeForDiagnostics(const QString& source, const QString& target);

    QStringList rerereForDiagnostics(const QString& operation, const QString& path);

    QStringList simulateBranchDrop(const QString& source, const QString& target,
                                   const QString& pick = QString());

    /// Hides, solos, pins or shows-all a branch, reporting what the graph is left showing.
    QStringList visibilityForDiagnostics(const QString& operation, const QString& branch);
    /// Writes the Local Ignore Rules dialog's edits (also its test seam).
    bool applyLocalIgnore(const QStringList& added, const QStringList& removed,
                          const QStringList& unhidden);

protected:
    /// The repository can be changed from outside — a commit in a terminal, a fetch
    /// from a script — and coming back into focus is the moment to look.
    bool event(QEvent* event) override;

    /// Saves the open tab set on the way out, so a session opened with a repository
    /// path before the window was visible is still remembered.
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onOpenRepositoryDialog();
    void onCommitSelected();
    void updateGraphColumnWidth();

    void showHistoryPage();
    void showConflictsPage();
    /// Leaves the page without touching the operation, and remembers the choice so a
    /// refresh does not undo it.
    void onConflictPageClosed();

    void onRefContextMenu(const QPoint& pos);
    void applyRefFilter();
    void togglePin(const QModelIndex& index);
    /// Sections open, folders closed, everything open while a filter is on — a match
    /// hidden behind a triangle is a match the user cannot see.
    void syncRefTreeExpansion();
    void onCommitContextMenu(const QString& commitId, const QPoint& globalPos);
    /// Keys the entries in QAction::data(); nothing is shown or executed here.
    void buildCommitMenu(QMenu& menu, const QString& commitId) const;
    bool isStashCommit(const QString& commitId) const;
    void dispatchCommitMenuAction(const QString& key, const QString& commitId);
    void refreshRefs();

    /// See `ui/drop_point.h` for why this is not simply the cursor position.
    QPoint dropMenuPosition() const;
    void onRefDroppedOnRef(gk::RefModel::DraggedRef source, QString target);
    void onRefDroppedOnCommit(gk::RefModel::DraggedRef source, QString commitId);
    void onSyncFinished(bool ok, QString summary, QString output);

private:
    void buildUi();
    void buildToolBar();
    /// Added before the main toolbar so it sits above it.
    void buildTabBar();
    void openPreferences();
    void cloneFromDialog();
    /// The repository does not exist yet, so this cannot go through RepoSession.
    void startClone(const QString& url, const QString& destination, bool partial,
                    const QString& branch, int depth, const QString& shallowSince);
    void onCloneFinished(bool ok, const QString& destination, const QString& output);
    void openExternalDiff(const QString& path);
    void openExternalMerge(const QString& path);
    void externalDiffPair(const QString& path, QString* base, QString* target, bool* cached) const;
    void onExternalToolFinished(bool ok, const QString& output, const QString& title, bool refresh);
    void openTerminal();
    void openHooks();
    void openLocalIgnore();
    void onCoauthorsRequested();
    void onIgnoreRequested(const QStringList& paths, const QString& scope, const QString& kind);
    static QString ignorePatternFor(const QString& path, const QString& kind);
    static QString ignorePathspecFor(const QString& path, const QString& kind);
    /// Applies the shared application look, then the pieces the stylesheet does not
    /// reach (icons, toolbar metrics, HTML with baked-in colours). The only place that
    /// applies the look.
    void applyAppearance();
    void loadGraphAnimation();
    void loadGraphViewOptions();
    void refreshCommands();
    /// A menu action keeps its binding, anything else gets a shortcut, and a command
    /// with no sequence gets none.
    void installCommandShortcuts();
    /// The id another command would share `sequence` with, or "(reserved)" when the
    /// window itself uses it. Empty when the combination is free.
    QString keymapConflict(const QString& id, const QKeySequence& sequence) const;
    /// Pages history until the commit is loaded, retrying as each page arrives so a
    /// deep commit does not spin the GUI.
    void revealCommit(const QString& oid);
    void tryRevealCommit();
    /// Re-reads the history and puts the selection back once its row is loaded again.
    void reloadHistory();
    void restoreSelectionAfterReload();
    /// True when the refs or HEAD moved since the last refresh (or nothing was known).
    bool syncHistoryWithRefs(const RepoInfo& info, const std::vector<RefInfo>& refs);
    QWidget* buildReferencePanel();
    void buildCommitPanel();
    void buildChangesPanel();

    void refreshPanels();
    /// What `refreshPanels` does with what it read; reads only from the session's
    /// snapshot when one is installed.
    void applyPanels();
    void onSnapshotReady(RepoSession* session);
    void setPanelsLoading(bool loading);
    void updateGraphSkeleton();
    void refreshStatusBar();
    void refreshStatusBar(const std::vector<FileChange>& changes);
    void showWorkingTreeChanges();
    void showWorkingTreeChanges(const std::vector<FileChange>& changes);
    void showCommitFiles(const QString& commitId);
    void showComparison(const QString& base, const QString& target);
    /// `base` is the merge base of the two revisions: what this branch has done on top
    /// of `target`, ignoring what `target` has done since — `git diff target...base`.
    void showComparisonMergeBase(const QString& base, const QString& target);
    /// `git diff <rev>`, the "what have I not committed yet against this" question.
    void showComparisonWorkingTree(const QString& revision);
    enum class CompareMode { TwoDot, MergeBase, WorkingTree };
    void applyComparison(const QString& baseId, const QString& targetId, CompareMode mode,
                         const QString& label);
    void restoreRenderedFileFromView();
    void showBlameFor(const QString& path);
    void showDocument(const DiffView::Document& document);
    void showCommitDetails(const QString& commitId, const CommitDetails& details);
    void showFileHistory(const QString& path);
    void showReflog();
    void editSparseCheckout();
    void editProfiles();
    void openGitFlow();
    void openLfs();
    void notify(int level, const QString& text);
    void showNotifications();
    void applyProfile(const gk::GitProfile& profile);
    void showCommitColumnMenu(const QPoint& pos);
    void setCommitColumnVisible(CommitModel::Column column, bool visible);
    void loadCommitColumns();
    void saveCommitColumns();
    /// The parts of the header that must be set after `restoreState` too, since a saved
    /// layout does not carry them.
    void applyCommitColumnModes();
    /// `setWidths` is false on restore, so reopening a repository does not undo a drag the
    /// reader made after choosing the mode.
    void setColumnMode(GraphColumnMode mode, bool setWidths);
    void applyColumnMode(bool setWidths);
    void loadCommitDateFormat();
    void setCommitDateFormat(CommitDateFormat format);
    void setTimelineGrouping(TimelinePeriod period);
    void loadTimeline();
    QString columnSettingsKey() const;
    void amendSelectedCommitMessage(const QString& message);
    void verifyCommitSignature(const QString& commitId);
    void selectCommit(const QString& commitId);
    void showDiff(const QString& path, const QString& commitId);
    /// `workingTree` says which of the two the file is read from; `staged` says which side,
    /// which decides the diff and whether the control offers Stage or Unstage.
    void renderFile(const QString& path, bool workingTree, bool staged = false);
    void applyStagingRequest(const QString& path, int hunkIndex, const QVector<int>& lines,
                             bool toIndex);
    /// Asks first: the content was never committed, so the operation log cannot bring it
    /// back.
    void applyDiscardRequest(const QString& path, int hunkIndex, const QVector<int>& lines);
    /// Re-selects `path` after a refresh, so a staged hunk does not make the open file
    /// disappear.
    void revealFileAfterRefresh(const QString& path, bool staged);
    void setStatus(const QString& text);

    void stashWorkingTree();
    void stashFiles(const QStringList& paths);
    void popTopStash();
    /// `drop` is true for `pop`, false for `apply`. On a conflict the conflict page opens:
    /// `git stash pop` leaves unmerged entries and keeps the stash.
    void restoreStash(const QString& oid, bool drop, const QString& label);
    void dropStash(const QString& oid, const QString& label);
    void branchFromStash(const QString& oid, const QString& label);
    void refreshStashes();

    void refreshWorktrees();
    void createWorktree();
    void openWorktree(const QString& path);
    void removeWorktreeAt(const QModelIndex& index);
    void lockWorktreeAt(const QModelIndex& index);
    void unlockWorktreeAt(const QModelIndex& index);
    void pruneWorktrees();

    void refreshSubmodules();
    void addSubmodule();
    void openSubmodule(const QString& path);
    void initSubmoduleAt(const QModelIndex& index);
    void updateSubmoduleAt(const QModelIndex& index, bool remote);
    void syncSubmoduleAt(const QModelIndex& index);
    /// Removes the checkout but keeps the entry; asks before forcing.
    void deinitSubmoduleAt(const QModelIndex& index);
    void initializeAllSubmodules();
    void updateAllSubmodules();
    void syncAllSubmodules();

    void runWrite(const QString& what, const std::function<bool(QString*)>& action,
                  bool resetHistory = true);
    /// A checkout, on a worker when reads are asynchronous: on a large work tree it
    /// rewrites thousands of files. `done` runs after a successful one.
    void runCheckout(const QString& what, const QString& target,
                     std::function<bool(RepoSession&, QString*)> action,
                     std::function<void()> done = {});
    void checkoutLocalBranch(const QString& name);

    enum class StageOp { Stage, Unstage, Discard };

    void checkoutSelectedBranch();
    void applyToFiles(const QStringList& paths, StageOp op);
    void commitStaged(const QString& message);
    QString headCommitMessage() const;
    QColor headBranchColor() const;
    void discardAllChanges();
    void stashAllChanges(const QString& message);
    void stageAllChanges();

    void undoLastOperation();
    void redoLastOperation();
    void updateUndoRedoActions();

    void buildCommands();
    void showCommandPalette(bool preferHistory);
    std::vector<PaletteRef> paletteRefs() const;
    void preparePalette(CommandPalette& palette, bool preferHistory);
    void actOnPaletteChoice(const CommandPalette& palette);
    void centerCommitRow(int row);
    void runCommand(const QString& id);
    /// A local branch is checked out; a remote-tracking one becomes a local branch that
    /// follows it, after asking.
    void checkoutBranchFromGraph(const QString& shortName, RefKind kind);
    bool localBranchExists(const QString& shortName) const;
    /// Runs the automatic conflict pass right after a rebase or a merge stops. A conflict
    /// where nothing is actually contested should never reach the reader.
    void sweepConflictsAfterOperation(const QString& what);
    /// HEAD, the index and the checked-out branch's file, by identity and time: re-walking
    /// a million-commit history on every alt-tab is not an option.
    QString repositoryFingerprint() const;
    void refreshIfRepositoryChanged();
    void createBranchFromSelection();
    void deleteSelectedBranch();
    void deleteSelectedBranches();
    void deleteMergedBranches();
    void setUpstreamsForSelected();
    void hideSelectedBranches(bool hidden);
    void soloSelectedBranches(bool solo);
    void mergeBranchIntoCurrent();
    /// `preflightShown` is set by the drop paths, where the prediction was already in front
    /// of the reader: re-asking in a modal would ask the same question twice.
    void mergeBranchInto(const QString& source, const QString& target,
                         bool preflightShown = false);
    void rebaseBranchOnto(const QString& branch, const QString& onto,
                          bool preflightShown = false);
    void squashMergeBranch(const QString& source, const QString& target,
                           bool preflightShown = false);
    void squashMergeIntoCurrent();

    void createBranchAtCommit(const QString& commitId);
    void createTagAtCommit(const QString& commitId);
    void renameBranchInteractive(const QString& from);
    void removeBranchByName(const QString& name);
    void cherryPickCommit(const QString& commitId);
    void revertCommit(const QString& commitId);
    void resetCurrentBranchTo(const QString& commitId, ResetMode mode);
    void rewriteCommitInHistory(const QString& commitId, RewriteAction action);
    void dropCommitFromHistory(const QString& commitId);
    /// `onto` retargets the replay, `branch` replays that branch rather than the
    /// checked-out one.
    void interactiveRebaseRange(const QString& base, const QString& tip = QStringLiteral("HEAD"),
                                const QString& onto = {}, const QString& branch = {});
    void interactiveRebaseFromCommit(const QString& commitId);
    /// The base is the two branches' merge base, so the replay is exactly `source` onto
    /// `target`.
    void interactiveRebaseFromDrop(const QString& source, const QString& target);
    void amendStoppedRebaseCommit();
    void savePatchForCommit(const QString& commitId);
    void applyPatchInteractive();
    void runMaintenance(MaintenanceTask task);
    void onMaintenanceFinished(bool ok, const QString& task, const QString& output);
    void onRepositoryFilesChanged(bool gitState);
    void copyTextToClipboard(const QString& text, const QString& what);

    void startSync(SyncKind kind, bool silent = false);
    void updateSyncActions();

    QString currentSyncRemote() const;
    void updateRemotePicker();
    void setPullMode(PullMode mode);
    void setUpstreamInteractive();
    void forcePushInteractive();
    void pushTagToRemote(const QString& tag);
    void pushAllTags();
    void deleteTagInteractive(const QString& tag);
    void deleteTagFromRemotesInteractive(const QString& tag);

    void onAutoFetchTick();
    void updateAutoFetchMenu();
    void loadAutoFetch();
    void saveAutoFetch();
    /// The common git directory, so a repository and its linked worktrees share one; empty
    /// when no repo is open.
    QString repositorySettingsPath() const;
    /// The checkout path, where those settings used to live. Used only to migrate them once.
    QString repositoryLegacySettingsPath() const;
    QString repositoryLegacyStoreKey() const;
    void migrateRepositorySettings(const QString& fromPath, const QString& toPath);
    QString autoFetchSettingsKey() const;

    void loadPreflight();
    void savePreflight();
    QString preflightSettingsKey() const;
    void loadPrevention();
    void savePrevention();
    QString preventionSettingsKey() const;
    void loadSubmoduleAutoupdate();
    void saveSubmoduleAutoupdate();
    void maybeAutoUpdateSubmodules();
    void onSubmodulesUpdated(bool ok, const QString& output);
    /// Answers true when the operation should go ahead; without asking when there is
    /// nothing to warn about, when git cannot predict the pair, or when the prediction
    /// failed.
    bool confirmPreflight(const QString& ours, const QString& theirs);
    /// The one-line merge-tree prediction for a drop list, or empty when it could not be
    /// computed. `warns` is set when it names conflicts.
    QString dropPreviewText(const QString& source, const QString& target, bool* warns) const;
    void refreshRerereActions();

    /// From `m_visibility`, HEAD and the repository's own refs. An inactive visibility
    /// yields the "every ref" filter.
    HistoryFilter historyFilter() const;
    void applyVisibility();
    void clearVisibility();
    void toggleEye(const QModelIndex& index);
    QSet<QString> effectiveHiddenBranches() const;
    void toggleHidden(const QModelIndex& index);
    void toggleSolo(const QModelIndex& index);
    void toggleSmart();
    /// Turns smart off, since presets and smart answer the same question. `None` is the
    /// same as clearing.
    void applyPreset(VisibilityPreset preset);
    void applyPrefixPreset();

    /// The current branch, the target and their upstreams (and the local/remote
    /// counterpart of each).
    std::vector<std::string> smartVisibleRefs() const;
    /// `*active` is false when neither mode is on, i.e. the caller should fall back to
    /// hide/solo.
    std::vector<std::string> visibilityWantedRefs(bool* active) const;
    QStringList graphHiddenBranchNames() const;

    QString selectedRefShortName() const;
    bool selectedRefIsLocalBranch() const;
    /// Sections, folders and non-branch rows are left out: a group operation must not be
    /// handed a row it cannot act on. The order is the selection's, duplicates removed.
    QStringList selectedLocalBranches() const;
    /// Classify, confirm (naming the branches that would lose commits), then delete under
    /// one operation-log entry. `base` is what "merged" is measured against — empty means
    /// HEAD.
    void deleteBranchesWithConsent(const QStringList& names, const QString& base,
                                   const QString& what);
    void reportBranchBatch(const QString& what, const RepoSession::BranchBatchResult& result);

    /// One repository the window has open. The active context's session and models are
    /// mirrored in `m_session`/`m_commitModel`/`m_refModel`, so the thousands of call sites
    /// that name those members keep meaning "the repository on screen"; the context is where
    /// the rest of the per-repository state waits between switches, and `saveActiveState`/
    /// `adoptContext` copy it back and forth. It is why switching tabs cannot leak one
    /// repository's selection, filter or comparison into another. A context's session is
    /// *retired*, not deleted: a closed tab may still have a fetch or a page in flight.
    struct RetireSession {
        void operator()(RepoSession* session) const { session->retire(); }
    };

    struct RepoContext {
        QString path;  ///< canonical checkout path — the tab's identity
        QString alias; ///< display name when the reader renames the tab
        bool opened = false;
        bool primed = false; ///< the first page has been requested already
        bool panelsShown = false; ///< a refresh has been applied to the window at least once
        /// The refs and HEAD the history was last read for, and the model generation
        /// that read had: a refresh that finds them moved reloads the graph, unless the
        /// model was reset since (the operation that moved them already did).
        std::uint64_t historyDigest = 0;
        bool historyDigestKnown = false;
        bool diffPageShown = false; ///< the file view was up when the tab was left
        quint64 historyGeneration = 0;
        /// The page is sticky while an operation is unfinished, so leaving it has to be
        /// remembered or the next refresh drags the reader back.
        bool conflictPageDismissed = false;

        std::unique_ptr<RepoSession, RetireSession> session;
        std::unique_ptr<CommitModel> commitModel;
        std::unique_ptr<RefModel> refModel;

        RefPins pins;
        RefVisibility visibility;

        QString selectedCommitId;
        QString renderedPath;
        QString renderedContext;
        bool renderedStaged = false;
        QString lastFingerprint;
        QString revealTarget;
        int revealPages = 0;
        QString diffCodec;
        QString compareBase;
        QString compareTarget;
        CompareMode compareMode = CompareMode::TwoDot;
        QString compareLabel;
        QString refFilter;
        bool refFilterRegex = false;
        bool graphColumnUserSized = false;
        GraphColumnMode columnMode = kDefaultGraphColumnMode;
        TimelinePeriod timelinePeriod = TimelinePeriod::None;
    };

    RepoContext* active();
    RepoContext* contextOf(const RepoSession* session);
    const RepoContext* active() const;

    std::unique_ptr<RepoContext> makeContext();
    /// A background tab's page load or network finish must not redraw the repository on
    /// screen, so every connection is filtered by "is this the active context".
    void wireContext(RepoContext* context);
    bool openIntoContext(RepoContext* context, const QString& path);
    void activateContext(int index);
    void saveActiveState();
    void adoptContext(RepoContext* context);
    void bindViews();
    /// The last one is emptied rather than removed.
    void closeContext(int index);
    void rebuildTabBar();
    QString contextLabel(const RepoContext& context) const;
    void onTabChanged(int index);
    void onTabCloseRequested(int index);
    void onTabContextMenu(const QPoint& pos);
    void onNewTabRequested();
    void keepOnlyContext(int keep);
    void closeAllContexts();
    void renameContext(int index);
    void applyTabAlias(int index, const QString& alias);
    void showEmptyTab();
    void refreshStartPage();
    void noteRecentRepository(const RepoContext& context);
    void createRepositoryFromDialog();
    int contextForPath(const QString& key) const;
    /// Writes the open tab set and the active tab, unless persistence is off or this is a
    /// headless run.
    void saveTabs();
    QString loadTabAlias(const QString& commonDir) const;
    void saveTabAlias(const QString& commonDir, const QString& alias);

    QWidget* m_refPanel = nullptr;
    QLineEdit* m_refFilter = nullptr;
    QToolButton* m_refFilterRegex = nullptr;
    /// Outside the model because pins outlive it: it is rebuilt on every fetch and every
    /// keystroke.
    RefPins m_pins;
    RefVisibility m_visibility;

    RepoSession* m_session = nullptr;
    CommitModel* m_commitModel = nullptr;
    RefModel* m_refModel = nullptr;

    /// The active context's session and models are the ones `m_session` and friends point
    /// at; the rest wait here.
    std::vector<std::unique_ptr<RepoContext>> m_contexts;
    int m_active = -1;
    QTabBar* m_tabBar = nullptr;
    /// Off unless the interactive application asks, so a test cannot overwrite a reader's
    /// session.
    bool m_tabsPersistence = false;
    bool m_backgroundIndexing = false;
    bool m_asyncReads = false;
    /// Moves on with every file render; a document read for an older one is dropped.
    quint64 m_documentToken = 0;
    bool m_documentPending = false;
    quint64 m_commitFilesToken = 0;
    Skeleton* m_refSkeleton = nullptr;
    Skeleton* m_graphSkeleton = nullptr;
    Skeleton* m_branchSkeleton = nullptr;
    update::Updater* m_updater = nullptr;
    QToolButton* m_updateButton = nullptr;
    /// Set while restoring so the restore does not immediately re-save (and, if a saved path
    /// has gone, does not write the shortened set back).
    bool m_restoringTabs = false;

    QStackedWidget* m_stack = nullptr;
    StartPage* m_startPage = nullptr;
    bool m_headless = false;
    QStackedWidget* m_centerStack = nullptr;
    QAction* m_refsAction = nullptr;
    QAction* m_detailsAction = nullptr;
    QAction* m_mergedRemotesAction = nullptr;
    QAction* m_mergedCommitsAction = nullptr;
    PanelSplitter* m_history = nullptr;
    /// Set while the window folds or unfolds a panel itself (the diff page hides the
    /// branch rail), so that is not mistaken for the reader's choice.
    bool m_foldingPanel = false;
    /// Whether the reader wants the rail, as opposed to whether it is showing: opening a
    /// diff hides it without changing the preference.
    bool m_refsVisible = true;
    bool m_refsHiddenForDiff = false;
    /// What the diff pane is already showing, so a click does not re-render what the
    /// selection already rendered.
    QString m_renderedPath;
    QString m_renderedContext;
    bool m_renderedStaged = false;
    QString m_lastFingerprint;
    ConflictView* m_conflictView = nullptr;
    /// The active tab's copy of `RepoContext::conflictPageDismissed`; it lives here too
    /// because the refresh that decides the page reads it on every pass.
    bool m_conflictPageDismissed = false;
    QAction* m_searchAction = nullptr;
    QString m_revealTarget;
    /// What `reloadHistory` is waiting to select again: a commit id, or the
    /// working-tree row when `m_restoreWip` is set.
    QString m_restoreCommit;
    bool m_restoreWip = false;
    int m_revealPages = 0;

    RefTreeView* m_refView = nullptr;
    CommitGraphView* m_commitView = nullptr;
    RepoWatcher* m_watcher = nullptr;
    CommitDetailsPanel* m_details = nullptr;
    DiffView* m_diffView = nullptr;
    /// Empty means it detects per file. Kept here because it outlives any one file: a
    /// CP1251 project is CP1251 throughout.
    QString m_diffCodec;
    QLabel* m_branchLabel = nullptr;
    QToolButton* m_visibilityButton = nullptr;

    QAction* m_fetchAction = nullptr;
    QAction* m_pullAction = nullptr;
    QAction* m_pushAction = nullptr;
    QAction* m_forcePushAction = nullptr;
    QAction* m_stashAction = nullptr;
    QAction* m_popStashAction = nullptr;
    /// The stash list read on the last refresh, newest first: the palette and the toolbar
    /// do not each spawn `git stash list`.
    std::vector<StashEntry> m_stashes;
    std::vector<WorktreeEntry> m_worktrees;
    std::vector<SubmoduleEntry> m_submodules;
    /// Shared between the Edit menu and the toolbar so one enable decision drives both.
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QToolBar* m_toolBar = nullptr;
    bool m_syncRunning = false;
    bool m_checkoutRunning = false;
    /// True while gc/fsck runs on its worker, so a second invocation is refused and the busy
    /// sweep has one owner.
    bool m_maintenanceRunning = false;
    bool m_graphColumnUserSized = false;
    GraphColumnMode m_columnMode = kDefaultGraphColumnMode;
    CommitDateFormat m_commitDateFormat = CommitDateFormat::Iso;
    TimelinePeriod m_timelinePeriod = TimelinePeriod::None;
    NotificationLog m_notifications{this};
    QAction* m_notificationsAction = nullptr;
    /// Cached because `git remote -v` is a subprocess, and the status bar refreshes often.
    std::vector<RemoteInfo> m_remotes;
    QComboBox* m_remotePicker = nullptr;
    QToolButton* m_pullButton = nullptr;
    QToolButton* m_pushButton = nullptr;
    PullMode m_pullMode = PullMode::FfOnly;

    QMenu* m_remoteMenu = nullptr;
    QMenu* m_autoFetchMenu = nullptr;
    QAction* m_autoFetchAction = nullptr;
    QAction* m_autoFetchSeparator = nullptr;
    QTimer* m_autoFetchTimer = nullptr;
    QTimer* m_columnSaveTimer = nullptr;
    /// True while a clone runs, so a second one is refused rather than racing into the same
    /// directory.
    bool m_cloneRunning = false;
    /// Lets a background worker (clone, external tool) stop before touching a window being
    /// destroyed.
    std::shared_ptr<std::atomic<bool>> m_workerAlive = std::make_shared<std::atomic<bool>>(true);
    bool m_autoFetchEnabled = false;
    QStringList m_autoFetchRemotes;
    /// Set while a background auto-fetch runs: its failures go to the status bar, not a modal.
    bool m_silentSync = false;

    QAction* m_preflightAction = nullptr;
    bool m_preflightEnabled = false;
    QAction* m_preventionAction = nullptr;
    bool m_preventionEnabled = false;
    QAction* m_submoduleAutoupdateAction = nullptr;
    QAction* m_submoduleAutoupdateRemoteAction = nullptr;
    bool m_submoduleAutoupdate = false;
    bool m_submoduleAutoupdateRemote = false;
    bool m_submoduleUpdateRunning = false;
    bool m_lfsRunning = false;
    QAction* m_rerereAction = nullptr;
    QAction* m_rerereAutoupdateAction = nullptr;
    QAction* m_smartAction = nullptr;
    QAction* m_animateAction = nullptr;

    QString m_selectedCommitId;
    /// Set while the diff viewer is showing a comparison: the two revisions whose difference
    /// each rendered file belongs to. Empty otherwise.
    QString m_compareBase;
    QString m_compareTarget;
    CompareMode m_compareMode = CompareMode::TwoDot;
    QString m_compareLabel;

    CommandRegistry m_commands;

    Keymap m_keymap;
    /// Menu actions a command already has, so its binding updates the action rather than
    /// adding a second shortcut beside it.
    QHash<QString, QAction*> m_commandActions;
    QHash<QString, QShortcut*> m_commandShortcuts;
};

} // namespace gk
