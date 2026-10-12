#pragma once

#include "git/remote.h"
#include "git/repository.h"
#include "git/stash.h"
#include "git/worktree.h"
#include "git/submodule.h"
#include "git/patch_ops.h"
#include "git/maintenance.h"
#include "git/lfs.h"
#include "git/ref_meta.h"
#include "git/hooks.h"
#include "repo/oplog.h"

#include <QMutex>
#include <QThreadPool>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWaitCondition>

#include <atomic>
#include <memory>

namespace gk {

enum class SyncKind { Fetch, FetchAll, Pull, Push, PushForce, PushTags };

struct CommitPage {
    /// The requester's generation, handed back unchanged: a model that has reset since
    /// asking ignores the page, even one whose `skip` happens to match again.
    quint64 generation = 0;
    quint64 skip = 0;
    QVector<CommitInfo> commits;
    bool hasMore = false;
    /// Set when the page could not be read; `commits` is then empty.
    QString error;
};

/// The answer to a commit search, carried back from the worker with the query it
/// belongs to so a stale page can be ignored.
struct CommitSearchResults {
    QString query;
    int field = 0; ///< CommitSearchField, as an int to keep the metatype simple.
    QVector<CommitInfo> commits;
};

/// Branch tip metadata for every branch, read in one `for-each-ref`; on a
/// repository with thousands of branches that must not sit on the GUI thread.
struct RefMetaSnapshot {
    std::vector<RefMeta> meta;
};

/// Single point of access to one repository.
///
/// All libgit2 access is serialized through an internal mutex, because libgit2
/// forbids concurrent calls on the same repository handle. Heavy reads run on
/// the global thread pool and report back through queued signals, so the GUI
/// thread never blocks.
class RepoSession : public QObject {
    Q_OBJECT

public:
    static constexpr quint32 kPageSize = 500;

    explicit RepoSession(QObject* parent = nullptr);
    ~RepoSession() override;

    /// Lets go of a session whose owner is done with it — a closed tab. An idle
    /// session is deleted now; one with background work still running (a fetch, a
    /// page, a search) stays alive until the last of it finishes and then deletes
    /// itself, so no worker ever touches freed memory and its finished signal still
    /// reaches whoever has to clear a "running" flag.
    void retire();

    bool open(const QString& path, QString* errorOut = nullptr);
    /// Creates a repository at `path` with `initialBranch` as its unborn HEAD.
    /// Static because it touches no open session: "New Repository" may be asked for
    /// while another repository is on screen, and that tab must keep its own.
    static bool createRepository(const QString& path,
                                 const QString& initialBranch = QStringLiteral("main"),
                                 QString* errorOut = nullptr);
    void close();
    bool isOpen() const;

    /// Lets the session write the repository's commit-graph in the background when
    /// the history walk shows it is missing or far behind. Off by default: a test
    /// or a one-shot run must not change the repository it looks at.
    void setAutoCommitGraph(bool on) { m_autoCommitGraph = on; }

    RepoInfo info() const;
    std::vector<RefInfo> refs() const;
    std::vector<FileChange> status() const;
    std::vector<ChangedFile> changedFiles(const QString& commitId) const;
    FileDiff commitFileDiff(const QString& commitId, const QString& path) const;

    /// Files that differ between two revisions, and one path's line diff between them.
    /// Either revision may be a branch, a tag, a short hash — anything git resolves.
    std::vector<ChangedFile> changedFilesBetween(const QString& base,
                                                 const QString& target) const;
    FileDiff fileDiffBetween(const QString& base, const QString& target,
                             const QString& path) const;

    /// Files that differ between `revision` and the working tree, and one path's line
    /// diff between them. This is `git diff <rev>`: tracked changes only.
    std::vector<ChangedFile> workdirChangedFilesAgainst(const QString& revision) const;
    FileDiff workdirFileDiffAgainst(const QString& revision, const QString& path) const;

    /// Per-line provenance of a path, at `revision` (empty = the working tree).
    std::vector<BlameLine> blame(const QString& path, const QString& revision = {}) const;
    /// The commits that touched a path, newest first, following renames.
    std::vector<FileHistoryEntry> fileHistory(const QString& path) const;

    /// Puts `path` back the way `revision` has it: into the working tree, or into the
    /// index when `staged`. This is `git restore`.
    bool restoreFile(const QString& revision, const QString& path, bool staged, QString* errorOut);

    /// The bytes of a file, at a commit and as it is on disk. Raw bytes, not text:
    /// what encoding they are in is a display question, and git has no opinion.
    bool fileAtRevision(const QString& commitId, const QString& path, std::string* bytes,
                        QString* errorOut) const;
    bool workingFileContent(const QString& path, std::string* bytes, QString* errorOut) const;
    FileDiff workdirFileDiff(const QString& path) const;
    /// HEAD's tree against the index — the staged change, for the Staged side of
    /// the working tree.
    FileDiff stagedFileDiff(const QString& path) const;
    /// The bytes `path` holds in the index, or empty when it is not there.
    std::string indexFileContent(const QString& path) const;

    /// Stages a path, unstages it, stages everything, or discards its unstaged
    /// changes. None of them throw; the reason comes back through `errorOut`.
    bool stageFile(const QString& path, QString* errorOut);
    bool unstageFile(const QString& path, QString* errorOut);
    bool stageAll(QString* errorOut);
    bool discardFileChanges(const QString& path, QString* errorOut);
    /// Every uncommitted change, staged or not, untracked files included. Not
    /// undoable: the operation log holds pointers, not content.
    bool discardAllChanges(QString* errorOut);

    /// The hunk a reader acted on, as it was shown. A click can outlive the diff
    /// it was made against; the session recomputes and refuses when the hunk is no
    /// longer the one that was on screen, rather than stage the wrong change.
    struct HunkAnchor {
        int oldStart = 0;
        int oldLines = 0;
        int newStart = 0;
        int newLines = 0;
    };

    enum class HunkApply { Stage, Unstage, Discard };

    /// Moves a hunk, or a set of its lines, between the working tree and the index.
    /// `lineIndices` holds indices into the hunk's lines, empty meaning the whole
    /// hunk. Staging is not recorded in the operation log: it is not a pointer the log
    /// can put back, and a discard destroys content the log never had.
    bool applyHunkSelection(const QString& path, int hunkIndex, const QVector<int>& lineIndices,
                            HunkApply what, const HunkAnchor& expect, QString* errorOut);

    bool createCommit(const QString& message, QString* newCommitOut, QString* errorOut);

    /// The same, with signing decided by `sign` rather than by `commit.gpgsign`:
    /// the Commit Panel's per-commit switch. A signed commit is created by the git
    /// binary, so the reader's gpg/ssh configuration applies.
    bool createCommit(const QString& message, bool sign, QString* newCommitOut, QString* errorOut);

    /// Puts back the pointers as they were before the last recorded operation.
    /// `labelOut` receives what was undone, for the status line and the tooltip.
    /// Refuses while a merge or a rebase is open: an undo belongs to completed
    /// work, not to the middle of one.
    bool undo(QString* labelOut, QString* errorOut);
    bool redo(QString* labelOut, QString* errorOut);
    bool canUndo() const;
    bool canRedo() const;
    QString nextUndoLabel() const;
    QString nextRedoLabel() const;
    QStringList oplogDescription() const;

    std::vector<ConflictEntry> conflicts() const;
    QString conflictFileContent(const QString& oid) const;

    /// The conflicted file as git's own merge would mark it up, built from the
    /// index stages in diff3 form (ours, base, theirs). `textAvailable` is set
    /// false for a binary conflict, where there is no text to compare.
    QString conflictMarkedContent(const QString& path, bool* textAvailable = nullptr) const;

    /// Size of a stage blob in bytes, or -1 when it is missing. Answers for a
    /// binary blob, which `conflictFileContent` cannot.
    long long blobSize(const QString& oid) const;

    /// Write operations. They never throw: the repository error is reported
    /// through `errorOut` so callers can stay inside Qt signal/slot code.
    bool resolveConflict(const QString& path, const QString& content, QString* errorOut);
    /// Resolves a conflict by keeping one stage whole — the only honest answer for
    /// a binary file, where nothing can be compared as text.
    bool keepConflictSide(const QString& path, ConflictSide side, QString* errorOut);
    /// Settles a delete/modify conflict on the deleting side: removes the path from
    /// the index and the working tree and clears the conflict. This is the honest
    /// answer when the reader keeps the deletion — writing the empty side as an
    /// empty file would stage a file git never had.
    bool deleteConflictFile(const QString& path, QString* errorOut);
    QStringList resolvedConflictPaths() const;
    /// Reopens a resolved file's conflict; the working-tree file is left alone.
    bool markConflicted(const QString& path, QString* errorOut);
    /// Writes a partial resolution to the working tree, markers and all, and leaves
    /// the index conflicted. The reader's draft is saved without pretending the file
    /// is resolved.
    bool writeConflictDraft(const QString& path, const QString& content, QString* errorOut);

    struct SemanticJsonMerge {
        bool applicable = false;
        bool hasConflicts = false;
        std::size_t conflicts = 0;
        QString merged;
    };
    /// Merges the three index stages of `path` as JSON objects, key by key: a key
    /// only one side moved is taken from that side, a key both moved differently
    /// becomes a diff3 conflict block. Not applicable unless every side is a lone
    /// JSON object.
    SemanticJsonMerge semanticMergeJson(const QString& path) const;
    QString resolveRevision(const QString& revision) const;

    /// The reflog, newest first, up to `limit` entries. Read-only; empty when the
    /// repository has no reflog yet.
    std::vector<ReflogEntry> reflog(std::size_t limit) const;

    struct AutoResolveOutcome {
        QStringList stagedFiles;  ///< conflicts gone without asking anyone
        QStringList needsAPerson; ///< files left untouched, markers and all
        /// One "path: reason x n, reason x n" line per settled file, so the reader
        /// learns why a conflict needed nobody rather than only how many were settled.
        QStringList resolvedDetail;
        std::size_t hunksResolved = 0;
        std::size_t hunksLeft = 0;
        bool allClear = false; ///< no conflict stages left in the index
    };

    /// Settles the conflicts that are a matter of comparing text rather than
    /// judging: both sides wrote the same thing, or one side is exactly what the
    /// base had. A file whose every hunk is settled is written and staged; a file
    /// with anything left is not touched at all, so the conflict editor and the
    /// index never disagree about what is in it.
    ///
    /// Deliberately narrow. Where both sides wrote different text, nothing happens.
    /// A delete/modify conflict is left alone as well: keeping the file and removing
    /// it are both defensible, and a program that picks one loses work quietly.
    AutoResolveOutcome autoResolveTrivialConflicts();

    bool rerereEnabled() const;
    bool rerereAutoupdate() const;
    /// Conflicted paths rerere knows about, and those it did not resolve from
    /// memory. A path in the first list but not the second was applied from a
    /// recorded resolution and is waiting only to be staged.
    QStringList rerereTrackedPaths() const;
    QStringList rerereRemainingPaths() const;

    /// Turns rerere on or off, and optionally its automatic staging. Configuration,
    /// not a pointer move: the operation log would have nothing to put back.
    bool setRerereEnabled(bool on, QString* errorOut);
    bool setRerereAutoupdate(bool on, QString* errorOut);
    bool applyRerere(QString* errorOut);
    bool forgetRerere(const QString& path, QString* errorOut);

    struct SweepOutcome {
        bool finished = false;
        bool needsAPerson = false;
        std::size_t hunksResolved = 0;
        QStringList autoStaged;
        /// Per-file reasons for what was settled, "path: reason x n".
        QStringList autoDetail;
        QStringList needsAPersonPaths;
        QString error;
    };

    /// Carries a stopped rebase or merge forward: settle what needs nobody, stage
    /// it, continue, and repeat while each round keeps making progress. Stops at the
    /// first conflict that needs a person, and does nothing at all when there is no
    /// conflict to settle — continuing an operation is never this function's
    /// decision to make on its own.
    SweepOutcome sweepOperationConflicts(std::size_t maxRounds = 200);
    bool createBranch(const QString& name, const QString& targetCommit, QString* errorOut);
    bool createTrackingBranch(const QString& name, const QString& remote, QString* errorOut);
    /// Makes `branch` follow `upstream` (e.g. "origin/main"). The remote-tracking
    /// branch must already exist — `push` with `--set-upstream` is the way to
    /// create tracking for a branch the remote has never seen.
    bool setUpstream(const QString& branch, const QString& upstream, QString* errorOut);
    bool checkoutBranch(const QString& name, QString* errorOut);
    /// Deletes a branch. Refuses when it carries commits nothing else reaches.
    bool deleteBranch(const QString& name, QString* errorOut);
    /// Deletes it whether or not those commits are reachable elsewhere. Only after
    /// the reader has been told what it costs.
    bool deleteBranchForced(const QString& name, QString* errorOut);

    /// `failed` holds "name: reason" lines so one failure does not hide the rest.
    struct BranchBatchResult {
        QStringList done;
        QStringList failed;
    };

    /// Local branches (short names) whose commits are all reachable from `base`
    /// (empty means HEAD), other than HEAD and `base` itself. This is the set
    /// "delete branches already merged into target" acts on.
    QStringList branchesMergedInto(const QString& base) const;

    /// Of `names`, the ones that carry commits `base` does not reach — the branches
    /// a mass delete would lose. Asked before the confirmation, so the dialog can
    /// name the risk instead of a modal appearing halfway through the work.
    QStringList unmergedAmong(const QStringList& names, const QString& base) const;

    /// Deletes every branch in `names` under one operation-log entry, so a single
    /// undo puts them all back. `base` is the revision "merged" is measured against
    /// (empty means HEAD) — a branch already merged into the target is not refused
    /// merely because HEAD is somewhere else. `forceUnmerged` is the reader's
    /// explicit consent to lose the commits of the branches that are not merged; it
    /// is applied only to those, never to a branch that failed for another reason.
    bool deleteBranches(const QStringList& names, const QString& base, bool forceUnmerged,
                        BranchBatchResult* resultOut, QString* errorOut);

    /// Points every branch in `names` at `<remote>/<name>`. Branches that have no
    /// such remote-tracking branch are reported rather than guessed at. Tracking is
    /// configuration, not a pointer, so this is guarded, not recorded.
    bool setUpstreams(const QStringList& names, const QString& remote, BranchBatchResult* resultOut,
                      QString* errorOut);

    bool mergeBranch(const QString& name, MergeResult* resultOut, QString* errorOut);

    /// Predicts the conflicts merging `theirs` into `ours` would hit, through
    /// `git merge-tree --write-tree`. Read-only: the working tree and the index
    /// are untouched, so asking before a merge costs nothing.
    MergePreview previewMerge(const QString& ours, const QString& theirs, QString* errorOut) const;

    /// The branch this one is measured against for conflict prevention: its own
    /// upstream when it has one, else `origin/HEAD`, else the first of
    /// `origin/main`, `origin/master`, `main`, `master` that resolves to a commit.
    /// Empty for a detached HEAD or when none of them exists — there is nothing to
    /// compare against, and inventing a base would warn about the wrong branch.
    QString baseBranchForCurrentBranch() const;

    /// Predicts the conflicts that merging the base branch into HEAD would hit.
    /// `baseOut` receives the base that was used, empty when none was found. Same
    /// read-only guarantees as `previewMerge`: neither the working tree nor the
    /// index is touched.
    MergePreview previewAgainstBase(QString* baseOut, QString* errorOut) const;
    bool commitMerge(const QString& message, QString* errorOut);
    bool abortMerge(QString* errorOut);

    /// True while MERGE_HEAD exists, i.e. a merge still needs committing.
    bool mergeInProgress() const;

    /// `git merge --squash`: stages the branch's change without committing. Content,
    /// not a pointer move — the oplog restores pointers — so it goes through guarded,
    /// and the reader's own commit is the recorded operation.
    bool squashMerge(const QString& name, SquashResult* resultOut, QString* errorOut);
    /// True while SQUASH_MSG exists, i.e. a squash is staged but not yet committed.
    /// A squash never writes MERGE_HEAD, so mergeInProgress() does not see it.
    bool squashInProgress() const;
    /// Commits a squash as an ordinary commit; an empty message uses git's own.
    bool commitSquash(const QString& message, QString* newCommitOut, QString* errorOut);

    bool rebaseOnto(const QString& branch, const QString& onto, RebaseResult* resultOut,
                    QString* errorOut);
    bool rebaseContinue(RebaseResult* resultOut, QString* errorOut);
    bool rebaseAbort(QString* errorOut);
    bool rebaseInProgress() const;

    bool checkoutCommit(const QString& commitId, QString* errorOut);
    bool moveBranch(const QString& name, const QString& targetCommit, QString* errorOut);
    /// Moves it even if that discards uncommitted work in the working tree.
    bool moveBranchForced(const QString& name, const QString& targetCommit, QString* errorOut);

    bool canFastForward(const QString& branch, const QString& target) const;
    /// The best common ancestor of two revisions, or empty when unrelated.
    QString mergeBase(const QString& a, const QString& b) const;
    bool fastForwardBranch(const QString& branch, const QString& target, QString* errorOut);
    bool renameBranch(const QString& from, const QString& to, QString* errorOut);
    /// On success `newCommitOut` (when not null) receives the created commit id.
    /// Copies the given commits onto the checked-out branch, oldest first.
    bool cherryPick(const std::vector<QString>& commitIds, QString* newCommitOut, QString* errorOut);

    /// True when `onto..tip` is one uninterrupted line of first parents.
    bool rangeIsLinear(const QString& onto, const QString& tip, QString* errorOut) const;

    /// The id the checked-out branch points at, empty when it cannot be resolved.
    QString headTipId() const;
    bool resetTo(const QString& commitId, ResetMode mode, QString* errorOut);

    bool amendHeadMessage(const QString& message, QString* errorOut);
    /// The same, resigning when `sign` is set; the tree is kept.
    bool amendHeadMessage(const QString& message, bool sign, QString* errorOut);
    bool amendHead(const QString& message, bool sign, QString* newCommitOut, QString* errorOut);
    /// Amends the commit a stopped rebase is sitting on, with the staged index. An
    /// empty `message` keeps the commit's own; a non-empty one replaces it. Only valid
    /// while a rebase is open — this is how an `edit` step is finished.
    bool amendStoppedCommit(const QString& message, QString* errorOut);
    bool revertCommit(const QString& commitId, QString* newCommitOut, QString* errorOut);
    /// Rewrites history with squashes, fixups, rewords, edits, drops and reorders,
    /// through `git rebase -i`. `upstream` is the range's base; `onto` retargets the
    /// replay when non-empty, and `branch` replays that branch rather than the
    /// checked-out one.
    bool rewriteHistory(const QString& upstream, const std::vector<RewriteStep>& steps,
                        QString* errorOut, const QString& onto = {}, const QString& branch = {});

    /// The commits a rewrite would replay, oldest first — git's own todo for the range.
    std::vector<RebaseCandidate> rebaseCandidates(const QString& upstream,
                                                  const QString& tip) const;

    bool dropCommit(const QString& commitId, QString* errorOut);
    bool createTag(const QString& name, const QString& target, const QString& message,
                   QString* errorOut);
    /// Deletes a tag. Recorded in the operation log: deleting one is a ref move,
    /// and like any ref move it is undoable.
    bool deleteTag(const QString& name, QString* errorOut);
    bool formatPatch(const QString& commitId, QString* patchOut, QString* errorOut) const;

    /// Tracking relationship of a branch (ahead/behind upstream). An empty
    /// `branch` means HEAD.
    UpstreamInfo upstreamInfo(const QString& branch = QString{}) const;

    /// Full ref of the repository's target/base branch for Smart Visibility —
    /// the remote's default branch (its local counterpart when one exists), else a
    /// local `main`/`master`, else empty (HEAD is its own target).
    QString targetBranch() const;

    /// Branch tip metadata (author, date, upstream, ahead/behind) for the
    /// visibility presets, read in one `git for-each-ref` call.
    std::vector<RefMeta> refMeta() const;

    /// Reads the same metadata on a worker thread and reports it through
    /// refMetaLoaded(). The graph's labels want the ahead/behind relation for every
    /// branch, and reading it on the GUI thread would stall on a large repository.
    void requestRefMeta();

    /// One configuration value (repository + global), empty when unset.
    QString configValue(const QString& key) const;

    /// Writes a value into the repository's own config; empty removes the key.
    /// Configuration, not a pointer move: guarded, not recorded.
    bool setConfigValue(const QString& key, const QString& value, QString* errorOut);

    bool commitSigningEnabled() const;

    /// What git says about a revision's signature; `status` is git's `%G?` letter
    /// (`G` good, `N` none, `B` bad, …). Reading it runs the signature check.
    CommitSignature commitSignature(const QString& revision) const;

    /// Appends an ignore pattern to `.gitignore` or the shared `info/exclude`.
    /// Writes a file, so it is guarded, not recorded: the operation log restores
    /// pointers and cannot un-write a `.gitignore`.
    bool ignorePath(const QString& pattern, bool globalExclude, QString* errorOut);
    /// Removes every tracked path matching a pathspec from the index, leaving the
    /// files on disk.
    bool untrackPathspec(const QString& pathspec, QString* errorOut);
    /// The message with a `Co-authored-by:` trailer per author, through git itself.
    QString withCoAuthors(const QString& message, const std::vector<CoAuthor>& authors) const;
    QString commitTemplate() const;
    std::vector<CoAuthor> recentAuthors(int limit) const;

    /// The hooks git would run for this repository: name, path and state. Read-only.
    /// The directory is resolved by git, so `core.hooksPath` is honoured.
    std::vector<HookEntry> hooks() const;
    /// The resolved hooks directory, empty when git cannot answer.
    QString hooksPath() const;
    /// The hook's text, empty when the file does not exist.
    QString hookContent(const QString& name) const;

    /// Writes a hook's content. Files, not pointers: guarded, not recorded — the
    /// operation log restores refs and could not put a hook file back.
    bool writeHook(const QString& name, const QString& content, QString* errorOut);
    /// Makes a hook executable, copying git's `.sample` into place when there is no
    /// file yet. Unless the reader says otherwise, enabled is what `git` runs.
    bool enableHook(const QString& name, QString* errorOut);
    bool disableHook(const QString& name, QString* errorOut);

    /// The effective `diff.tool`/`merge.tool`, empty when unset (git's own default
    /// then applies). The choice lives in the git config, not in `QSettings`.
    QString externalTool(bool merge) const;
    bool setExternalTool(bool merge, const QString& name, QString* errorOut);
    std::vector<QString> availableExternalTools(bool merge) const;
    /// Opens the pair in the external diff tool. Blocking — the tool runs until the
    /// reader closes it — so the caller puts it on a worker. An empty `target` diffs
    /// the working tree against `base`; `cached` diffs HEAD against the index.
    SyncResult runExternalDiffTool(const QString& base, const QString& target,
                                   const QString& path, bool cached = false) const;
    /// Opens a conflicted path in the external merge tool. Blocking.
    SyncResult runExternalMergeTool(const QString& path) const;

    /// The stash entries, newest first. Empty when there is no stash.
    std::vector<StashEntry> stashList() const;

    /// Stashes the working tree. `paths` empty means everything; otherwise only
    /// those paths. `message` may be empty (git writes the default subject).
    ///
    /// Stashing changes the working tree and the index but moves no pointers, so
    /// it is deliberately *not* recorded in the operation log: the log restores
    /// pointers, and restoring those would not bring the stashed content back.
    bool stashPush(const QString& message, bool includeUntracked, const QStringList& paths,
                   QString* errorOut);
    /// `stash` is an oid from `stashList()`, not `stash@{n}` — an index goes
    /// stale as soon as an earlier entry is dropped.
    bool stashApply(const QString& stash, QString* errorOut);
    bool stashPop(const QString& stash, QString* errorOut);
    bool stashDrop(const QString& stash, QString* errorOut);
    bool stashBranch(const QString& stash, const QString& branch, QString* errorOut);

    /// `git worktree list`. Read-only: no working tree is opened, so this is safe
    /// to call on any repository, bare included.
    std::vector<WorktreeEntry> worktrees() const;

    /// One `WorktreeWip` per worktree with uncommitted changes, the one this session
    /// was opened on first. The current checkout is read through libgit2; a linked
    /// one through `git status --porcelain`, because it is a different working tree
    /// and opening a second handle for the same repository is what this guards
    /// against. Read whenever the working tree is refreshed.
    std::vector<WorktreeWip> worktreeWip() const;
    /// The same, with the current checkout's status already in hand: on a large
    /// working tree a status is the most expensive read a refresh makes.
    std::vector<WorktreeWip> worktreeWip(const std::vector<FileChange>& currentStatus) const;

    /// `git worktree add`. `createBranch` makes `branch` first at `startPoint`
    /// (HEAD when empty); otherwise `branch` must already exist.
    ///
    /// Creating a worktree adds directories git will not remove through the
    /// operation log, and the log restores pointers only — so these go through
    /// `guarded`, not `recorded`. An undo that removed the branch but left the
    /// directory behind would be a worse answer than no undo at all.
    bool addWorktree(const QString& path, const QString& branch, bool createBranch,
                     const QString& startPoint, QString* errorOut);
    /// `git worktree remove`. git refuses a worktree with modifications unless
    /// `force` is set; the window asks first and passes the answer here.
    bool removeWorktree(const QString& path, bool force, QString* errorOut);
    bool lockWorktree(const QString& path, const QString& reason, QString* errorOut);
    bool unlockWorktree(const QString& path, QString* errorOut);
    bool pruneWorktrees(QString* errorOut);

    /// `git submodule status --recursive` joined with each `.gitmodules`. Read-only.
    std::vector<SubmoduleEntry> submodules() const;

    /// `git submodule add`. `branch` empty leaves the tracked branch unset; an
    /// empty `path` lets git derive it from the URL.
    ///
    /// Like the worktree operations these go through `guarded`, not `recorded`:
    /// the oplog snapshots the parent's refs and index tree, and undoing a
    /// submodule add/update would not restore the nested checkout or `.git/config`.
    bool addSubmodule(const QString& url, const QString& path, const QString& branch, bool force,
                      QString* errorOut);
    bool deinitSubmodule(const QString& path, bool force, QString* errorOut);
    /// `git submodule update`. `initialize` is git's `--init`, the way a submodule
    /// is registered after a clone; `remote` fetches the tracked branch instead of
    /// the recorded commit. An empty `paths` means every submodule.
    bool updateSubmodules(const QStringList& paths, bool initialize, bool remote, bool recursive,
                          QString* errorOut);

    /// `git submodule update --init --recursive [--remote]` for every submodule, on a
    /// worker thread. The auto-update hook runs after a fetch, pull, checkout or
    /// completed merge and must not block the window; reports through
    /// submodulesUpdated(). A failure is silent to the caller — the reader is told in
    /// the status bar, not over whatever they are doing.
    void requestAutoUpdateSubmodules(bool remote);

    bool syncSubmodules(const QStringList& paths, bool recursive, QString* errorOut);

    std::vector<RemoteInfo> remotes() const;

    /// Runs fetch/pull/push on a worker thread. Network work must never sit on
    /// the GUI thread: a slow or hanging remote would freeze the window.
    /// Reports back through syncFinished().
    void requestSync(SyncKind kind, const QString& remote, const QString& branch,
                     PullMode mode = PullMode::FfOnly);

    SyncResult syncBlocking(SyncKind kind, const QString& remote, const QString& branch,
                            PullMode mode = PullMode::FfOnly) const;

    /// Fetches one or more remotes on a worker thread — the auto-fetch path, which
    /// may watch a subset rather than all of them. `remotes` empty means every
    /// configured remote (`git fetch --all`). Reports through syncFinished().
    void requestFetch(const QStringList& remotes, bool prune);

    SyncResult fetchBlocking(const QStringList& remotes, bool prune) const;

    /// Pushes one tag to `remote` on a worker thread, reporting through
    /// syncFinished(). A tag is not a branch, so it rides beside `requestSync`
    /// rather than through it.
    void requestPushTag(const QString& remote, const QString& tag);
    SyncResult pushTagBlocking(const QString& remote, const QString& tag) const;
    /// Deletes a tag on a remote (`git push <remote> :refs/tags/<tag>`). The local
    /// tag is untouched; deleting it here is a separate call.
    SyncResult deleteRemoteTagBlocking(const QString& remote, const QString& tag) const;

    /// Applies a patch file through the binary: a mailbox becomes commits
    /// (`git am`), a diff lands in the working tree (`git apply`). `mailboxOut`
    /// says which path was taken, so the caller can report it honestly.
    bool applyPatch(const QString& path, bool* mailboxOut, QString* errorOut);

    /// Runs `git gc` or `git fsck` on a worker thread and reports through
    /// maintenanceFinished(). Long, object-store work must never sit on the GUI
    /// thread, and a failure is git's own output, not a paraphrase.
    void requestMaintenance(MaintenanceTask task);
    SyncResult maintenanceBlocking(MaintenanceTask task) const;

    bool sparseCheckoutEnabled() const;
    QStringList sparseCheckoutDirectories() const;
    /// The repository's top-level directories at HEAD, for the picker. Read from
    /// the tree, so a directory the cone currently excludes still appears.
    QStringList sparseCheckoutTopLevelDirectories() const;
    /// Enables or changes the cone; an empty list disables it and restores the
    /// whole working tree. Files outside the cone are removed from the working
    /// tree, so this is `guarded`, never recorded: the oplog restores pointers and
    /// could not put a removed file back.
    bool setSparseCheckout(const QStringList& dirs, QString* errorOut);

    bool gitflowInitialized() const;
    QStringList gitflowSummary() const;
    bool initGitflow(const QString& master, const QString& develop, QString* errorOut);
    bool gitflowStart(const QString& kind, const QString& name, QString* errorOut);
    /// Merges the topic branch back `--no-ff`, tags it when `version` is set, and
    /// deletes it. A conflict stops it with MERGE_HEAD on disk, like any merge.
    bool gitflowFinish(const QString& kind, const QString& name, const QString& version,
                       QString* errorOut);

    /// True when the `git-lfs` binary is on PATH. Every LFS entry point checks this
    /// first and says so, rather than reporting a failed git command.
    bool lfsAvailable() const;
    QString lfsVersion() const;
    bool lfsConfigured() const;
    QStringList lfsTrackedPatterns() const;
    /// `*|- <path>` lines, one per file under LFS: `*` when the object is present
    /// locally, `-` when only the pointer is.
    QStringList lfsFiles() const;
    QString lfsStatusReport() const;

    /// `git lfs install --local`: the filter config and hooks, this repository only.
    bool lfsInstall(QString* errorOut);
    /// Appends the pattern to `.gitattributes`; the file is left unstaged.
    bool lfsTrack(const QString& pattern, QString* errorOut);
    bool lfsUntrack(const QString& pattern, QString* errorOut);
    bool lfsCheckout(QString* errorOut);

    /// Runs an LFS operation that may reach the network, on a worker thread;
    /// reports through lfsFinished(). An empty `remote` means the default one.
    void requestLfs(LfsTask task, const QString& remote, const QString& ref);
    SyncResult lfsBlocking(LfsTask task, const QString& remote, const QString& ref) const;

    /// Asynchronously loads a history page. Emits commitsLoaded() on the receiver's
    /// thread, with `error` set when the read failed. The graph view wants every ref,
    /// which is the default here.
    void requestCommits(quint64 generation, quint64 skip, quint32 limit,
                        const HistoryFilter& filter = HistoryFilter{HistoryScope::AllRefs, {}});

    std::vector<CommitInfo> commitsBlocking(quint64 skip, quint32 limit, bool* hasMore,
                                            const HistoryFilter& filter = HistoryFilter{}) const;

    /// Asynchronously searches every ref for `query`. Emits commitSearchFinished()
    /// on the receiver's thread. Long enough on a large repository that it must not
    /// sit on the GUI thread.
    void requestCommitSearch(const QString& query, CommitSearchField field, quint32 limit);

    /// Synchronous helper for the headless path and tests. `skip` pages through a
    /// query that matches more commits than one call returns.
    std::vector<CommitInfo> searchCommitsBlocking(const QString& query, CommitSearchField field,
                                                  quint32 limit, std::size_t skip = 0) const;

    /// References grouped by the commit they point at, for the graph labels.
    /// `filter`'s `visibleRefs` limits which labels come back.
    std::unordered_map<std::string, std::vector<RefInfo>> refsByCommit(
        const HistoryFilter& filter = HistoryFilter{}) const;

    /// Drops the history walk cached between pages. The session calls it after every
    /// operation; the window calls it when the repository may have changed without one
    /// (the watcher, window activation), so the next page is walked from the new state.
    void invalidateCommitWalk() const;

signals:
    void commitsLoaded(gk::CommitPage page);
    void commitSearchFinished(gk::CommitSearchResults results);
    void refMetaLoaded(gk::RefMetaSnapshot snapshot);
    void requestFailed(QString message);
    /// A read the window asked for synchronously failed; the caller got an empty
    /// answer, and this carries why.
    void readFailed(QString message);
    /// `output` is git's own output, shown verbatim in the log panel.
    void syncFinished(bool ok, QString summary, QString output);
    /// gc/fsck finished: `task` is "gc" or "fsck", `output` git's own.
    void maintenanceFinished(bool ok, QString task, QString output);
    /// The auto-update hook finished; `output` is git's own message on failure.
    void submodulesUpdated(bool ok, QString output);
    /// An LFS network/checkout operation finished; `task` is its name, `output`
    /// git's own.
    void lfsFinished(bool ok, QString task, QString output);

private:
    /// RAII hold on the in-flight count a local worker keeps while it runs. The
    /// destructor drains the count, so a worker that returns early — or unwinds —
    /// must not leak it, or the next `~RepoSession` waits forever.
    struct WorkerSlot {
        explicit WorkerSlot(RepoSession* session);
        ~WorkerSlot();
        WorkerSlot(const WorkerSlot&) = delete;
        WorkerSlot& operator=(const WorkerSlot&) = delete;

    private:
        RepoSession* m_session;
    };
    /// Registers an in-flight worker. False means the session is going away and the
    /// caller must not start anything.
    bool claimWorkerSlot();
    /// Runs `work` on the global pool as a counted worker. Every background task goes
    /// through here: the count is what lets the destructor and `retire` know nothing
    /// is still using the session.
    template <typename Fn>
    bool startWorker(Fn&& work) {
        if (!claimWorkerSlot()) {
            return false;
        }
        QThreadPool::globalInstance()->start([this, work = std::forward<Fn>(work)]() mutable {
            const WorkerSlot slot(this);
            work();
        });
        return true;
    }

    /// Runs a read under the session lock and turns a failure into `fallback` plus
    /// `readFailed`: an exception thrown into the window's event loop ends the
    /// process. The signal goes out after the lock is released, so a receiver may
    /// read the session again.
    template <typename T, typename Fn>
    T readOr(T fallback, Fn&& read) const {
        QString failure;
        {
            QMutexLocker lock(&m_mutex);
            if (!m_repo) {
                return fallback;
            }
            try {
                return read(*m_repo);
            } catch (const std::exception& e) {
                failure = QString::fromUtf8(e.what());
            }
        }
        emit const_cast<RepoSession*>(this)->readFailed(failure);
        return fallback;
    }

    /// Starts `git commit-graph write` once, in the background, when a history walk
    /// had to read enough commits outside the graph to be worth indexing.
    void indexHistoryIfWorthIt(std::size_t readOutsideGraph, const std::string& dir);

    mutable QMutex m_mutex;
    std::unique_ptr<Repository> m_repo;
    std::atomic<bool> m_autoCommitGraph{false};
    std::atomic<bool> m_commitGraphWriteStarted{false};
    std::atomic<bool> m_alive{true};
    /// Every background worker counts itself here (`startWorker`). `~RepoSession`
    /// waits for the count to drain — a worker that outlives the session reads freed
    /// memory — and a retired session deletes itself when it does, which is how a
    /// closed tab avoids waiting on a fetch in the GUI thread.
    QMutex m_workerMutex;
    QWaitCondition m_workersDone;
    int m_workersInFlight = 0;
    bool m_retired = false;
    Oplog m_oplog;
};

} // namespace gk

Q_DECLARE_METATYPE(gk::CommitPage)
Q_DECLARE_METATYPE(gk::CommitSearchResults)
Q_DECLARE_METATYPE(gk::RefMetaSnapshot)
