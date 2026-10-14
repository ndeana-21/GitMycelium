#pragma once

#include "core/types.h"

#include <array>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

struct git_repository;
struct git_rebase;

namespace gk {

class CommitGraph;
class TopoWalk;

class GitError : public std::runtime_error {
public:
    explicit GitError(const std::string& msg) : std::runtime_error(msg) {}
};

/// RAII wrapper over a libgit2 repository handle.
///
/// NOTE: libgit2 does not allow concurrent calls on the same repository
/// handle. All callers must serialize access (see repo::RepoSession).
class Repository {
public:
    /// Initializes the libgit2 global state exactly once. Safe to call from
    /// main() and from tests; must run before any other libgit2 entry point.
    static void initLibrary();

    static std::unique_ptr<Repository> open(const std::string& path);

    static std::unique_ptr<Repository> init(const std::string& path,
                                            const std::string& initialBranch = "main");

    ~Repository();
    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    RepoInfo info() const;
    std::vector<RefInfo> refs() const;

    /// Every direct ref, where HEAD points, and the tree the index holds: what the
    /// operation log records around a completed operation.
    RefState snapshotRefs() const;

    /// Applies the part of `target` that `from` differs from, leaving references the
    /// operation did not touch alone (undoing a commit must not disturb a branch a
    /// fetch added). A change of HEAD's place brings the working tree along with a
    /// safe checkout, which refuses rather than overwrite local changes.
    void applyRefDelta(const RefState& target, const RefState& from, bool checkoutWorkingTree);

    /// Resolves anything git understands — a branch, a tag, a short or full hash,
    /// HEAD~1 — to a commit id. Empty when it names nothing.
    std::string resolveRevision(const std::string& revision) const;
    std::vector<FileChange> status() const;
    /// The two halves of `status()`. The work tree's comes from the binary and needs
    /// no handle, so a caller holding the session lock can run it after releasing it.
    static std::vector<FileChange> worktreeStatus(const std::string& workDir);
    /// What git ignores in the work tree, relative to it; a directory ignored whole
    /// is one entry ending in '/'. Empty when git cannot say.
    static std::vector<std::string> ignoredPaths(const std::string& workDir);
    std::vector<FileChange> submoduleStatus() const;

    /// Walks history newest-first. `skip` commits are dropped first.
    /// `hasMore` is set to true when at least one more commit exists.
    std::vector<CommitInfo> commits(std::size_t skip, std::size_t limit, bool* hasMore,
                                    const HistoryFilter& filter = HistoryFilter{}) const;

    /// Every reference grouped by the commit it points at, which is what puts the
    /// branch labels next to their commits in the graph. `filter`'s `visibleRefs`
    /// limits which labels are returned.
    std::unordered_map<std::string, std::vector<RefInfo>> refsByCommit(
        const HistoryFilter& filter = HistoryFilter{}) const;

    /// Files changed by `commitId` relative to its first parent (an empty tree
    /// for a root commit).
    std::vector<ChangedFile> changedFiles(const Oid& commitId) const;

    FileDiff commitFileDiff(const Oid& commitId, const std::string& path) const;

    /// Files that differ between two revisions. An empty `base` diffs against
    /// nothing, so every path reads as an addition.
    std::vector<ChangedFile> changedFilesBetween(const Oid& base, const Oid& target) const;

    FileDiff fileDiffBetween(const Oid& base, const Oid& target, const std::string& path) const;

    /// Files that differ between `revision` and the working tree — `git diff <rev>`.
    /// Untracked files are new to the tree rather than differences from it, so they
    /// are not listed; that is what the binary answers too.
    std::vector<ChangedFile> workdirChangedFilesAgainst(const Oid& revision) const;
    FileDiff workdirFileDiffAgainst(const Oid& revision, const std::string& path) const;

    std::vector<BlameLine> blame(const std::string& path, const Oid& revision = {}) const;

    /// The commits that touched `path`, newest first, following renames
    /// (`git log --follow`); libgit2's revwalk has no equivalent.
    std::vector<FileHistoryEntry> fileHistory(const std::string& path) const;

    std::vector<ReflogEntry> reflog(std::size_t limit) const;

    /// Commits from every ref matching `query`, newest first, paged. Message and
    /// author are literal, case-insensitive substrings (`git log -F -i`); a hash is
    /// resolved with `git rev-parse`, so a full or unambiguous abbreviation returns
    /// the one commit and an ambiguous one returns nothing.
    std::vector<CommitInfo> searchCommits(const std::string& query, CommitSearchField field,
                                          std::size_t skip, std::size_t limit) const;
    /// The same search in the work tree at `workDir`, with no repository handle and
    /// so no lock held for the duration of a large `git log --all`.
    static std::vector<CommitInfo> searchCommitsIn(const std::string& workDir,
                                                   const std::string& query,
                                                   CommitSearchField field, std::size_t skip,
                                                   std::size_t limit);

    /// Restores `path` from `revision` into `base`: into the working tree when
    /// `staged` is false, into the index when it is. This is `git restore`.
    void restoreFile(const Oid& revision, const std::string& path, bool staged);

    FileDiff workdirFileDiff(const std::string& path) const;

    /// One path between HEAD's tree and the index — what is staged. An unborn HEAD
    /// diffs against an empty tree.
    FileDiff stagedFileDiff(const std::string& path) const;

    std::string indexFileContent(const std::string& path) const;

    void applyPatchToIndex(const std::string& patch);

    /// Applies a unified patch to the working tree and nothing else. The reverse of
    /// a working-tree diff discards that change.
    void applyPatchToWorkdir(const std::string& patch);

    /// Stages `path` as it is on disk: a new or modified file is added, a tracked
    /// file that is gone is removed. A path that no longer exists and was never
    /// tracked has nothing to stage and is left alone.
    void stageFile(const std::string& path);

    /// Restores `path`'s index entry to HEAD's version; a newly added file leaves
    /// the index while the working-tree file stays. This is `git reset -- <path>`.
    void unstageFile(const std::string& path);

    /// Stages every change in the working tree: new, modified, deleted and
    /// untracked (`git add -A`).
    void stageAll();

    /// Discards `path`'s unstaged changes, restoring the index version on disk.
    /// An untracked file is deleted. Anything already staged is not touched.
    void discardFileChanges(const std::string& path);
    /// Throws away every uncommitted change: the index and the tracked files go back
    /// to HEAD and untracked files are deleted (ignored files stay). Through the
    /// binary, so filters and hooks apply. Refuses while an operation is open.
    void discardAllChanges();

    Oid createCommit(const std::string& message);

    /// The same, signing the commit when `sign` is set. Signing is done by the
    /// `git` binary — `git commit -S` — because only it knows the reader's
    /// `gpg.format`, `user.signingkey` and `gpg.program`; libgit2 would write an
    /// unsigned commit while reporting success.
    Oid createCommit(const std::string& message, bool sign);

    /// Appends `pattern` to the work tree's `.gitignore`, or to the shared
    /// `.git/info/exclude` when `globalExclude` is set. A pattern already present
    /// is not written twice. This does not touch the index; `untrackFile` is the
    /// other half of "ignore this".
    void addIgnorePattern(const std::string& pattern, bool globalExclude);

    /// Removes every tracked path matching `pathspec` from the index while leaving
    /// the files on disk — the `git rm --cached` that makes an already-tracked file
    /// ignorable. A pathspec that matches nothing is not an error: ignoring an
    /// untracked file is the ordinary case.
    void untrackPathspec(const std::string& pathspec);
    /// The patterns in `.git/info/exclude`, the ignore file that is the reader's own
    /// (never committed, never shared); comments and blank lines left out.
    std::vector<std::string> localExcludePatterns() const;
    void removeLocalExcludePattern(const std::string& pattern);
    /// Tracked files whose changes are hidden here only (`--skip-worktree`): an
    /// ignore rule does nothing for a tracked file, this is the local way to stop
    /// seeing one.
    std::vector<std::string> skipWorktreePaths() const;
    void setSkipWorktree(const std::vector<std::string>& paths, bool skip);

    /// Removes a path from the index *and* the working tree (`git rm -f`). This is
    /// how a delete/modify conflict is settled on the deleting side: `git rm` on a
    /// conflicted path stages the removal and clears the conflict, where writing an
    /// empty file over it would leave an empty tracked file behind instead.
    void removeFile(const std::string& path);

    /// Applies a `Co-authored-by:` trailer per author to `message`, through git's own
    /// `interpret-trailers`, and returns the result. An author already in the trailer
    /// block is not added again.
    std::string addCoAuthors(const std::string& message,
                             const std::vector<CoAuthor>& authors) const;

    /// The contents of `commit.template`, with the configured path expanded, or empty
    /// when none is set or the file cannot be read.
    std::string commitTemplate() const;

    /// Authors from the recent log, newest first and deduplicated, for the co-author
    /// suggestions. Bounded, because a monorepo's log is not a list to walk.
    std::vector<CoAuthor> recentAuthors(std::size_t limit) const;

    std::vector<ConflictEntry> conflicts() const;
    /// Paths already resolved in the open operation: git's resolve-undo record,
    /// less anything conflicted again. Only meaningful while an operation is open —
    /// the record outlives the commit that ends one, and the next operation clears it.
    std::vector<std::string> resolvedConflictPaths() const;
    /// `git update-index --unresolve`: puts back the conflict the resolve-undo record
    /// remembers for `path`, leaving the working-tree file exactly as it is.
    void markConflicted(const std::string& path);

    /// Blob content as text; empty for a missing oid or a binary blob.
    std::string blobText(const Oid& blobId) const;

    /// Content of `path` at `commitId` ("" when the path does not exist there).
    std::string fileAt(const Oid& commitId, const std::string& path) const;

    std::string workdirFileContent(const std::string& path) const;

    /// Writes `content` to the working tree, drops the path's conflict stages
    /// and stages the result.
    void resolveConflict(const std::string& path, const std::string& content);

    /// Writes `content` to the working tree and touches nothing else: the index
    /// keeps its conflict stages. This saves a partial resolution as a draft with its
    /// markers still in it, without pretending the file is resolved.
    void writeWorkdirFile(const std::string& path, const std::string& content);

    /// `path`'s conflict stages merged into one text carrying diff3-style markers —
    /// ours, the base, theirs. `textAvailable` is set false when the file is binary
    /// or the stages produce no text.
    ///
    /// The base section is the point: without it there is no telling "our side is
    /// what the base had" from "our side wrote this".
    std::string conflictMarkedContent(const std::string& path, bool* textAvailable) const;

    /// Size of a blob in bytes, or -1 when it is missing. Unlike `blobText` this
    /// answers for a binary blob.
    long long blobSize(const Oid& blobId) const;

    /// The raw bytes of one index stage of a conflicted path, binary included.
    /// `present` is set false when that side is absent (a deletion).
    std::string conflictStageBytes(const std::string& path, ConflictSide side,
                                   bool* present) const;

    /// Resolves a conflict by keeping one stage whole — the only honest answer for
    /// a binary file. Written and staged like any other resolution.
    void keepConflictSide(const std::string& path, ConflictSide side);

    void createBranch(const std::string& name, const Oid& target);

    /// Creates `name` at the tip of the remote-tracking branch `remote`, and makes
    /// it follow that branch. A remote-tracking branch is not a place the working
    /// tree can stand: checking one out means making a local branch that stands for
    /// it, and without the upstream that branch would push somewhere by accident.
    void createTrackingBranch(const std::string& name, const std::string& remote);
    /// Points the local branch `branch` at the remote-tracking branch `upstream`
    /// ("origin/main"). Refuses when either does not exist: a branch that looks
    /// like it follows something it does not is worse than no tracking at all.
    void setUpstream(const std::string& branch, const std::string& upstream);
    void checkoutBranch(const std::string& name);
    /// Deletes `name`. Refuses when the branch carries commits nothing else
    /// reaches, unless `force` is set. `base` is the revision reachability is
    /// measured against; empty means HEAD, so a branch known to be merged into the
    /// target is not refused merely because HEAD is elsewhere.
    void deleteBranch(const std::string& name, bool force = false,
                      const std::string& base = std::string{});

    bool branchMergedIntoHead(const std::string& name) const;
    /// True when every commit on `name` is already reachable from `base`. An empty
    /// `base` means HEAD: the question "would deleting this branch lose work".
    bool branchMergedInto(const std::string& name, const std::string& base) const;

    /// Merges `branch` into HEAD. On conflicts the merge is left in progress
    /// with MERGE_HEAD written, exactly like `git merge`.
    MergeResult mergeBranch(const std::string& branch);

    /// Predicts what merging `theirs` into `ours` would do, through
    /// `git merge-tree --write-tree`, without touching the working tree or the
    /// index. `available` is false when git refuses the pair outright — one side
    /// already reachable from the other.
    MergePreview previewMerge(const std::string& ours, const std::string& theirs) const;
    /// Where the conflicts sit in our side of each file, read from the tree
    /// `git merge-tree --write-tree` left behind with its markers.
    std::map<std::string, std::vector<LineSpan>> predictedConflictLines(
        const std::string& treeId, const std::vector<std::string>& paths) const;

    Oid commitMerge(const std::string& message);

    /// Discards the in-progress merge and resets to HEAD. Like `git merge --abort`
    /// without a saved pre-merge state, local working-tree changes are lost.
    void abortMerge();

    bool mergeInProgress() const;

    /// `git merge --squash <branch>`: stages the branch's change without committing,
    /// so the reader writes the commit. Through the git binary: the state it leaves
    /// is the binary's own — `SQUASH_MSG`, and deliberately no `MERGE_HEAD`.
    SquashResult squashMerge(const std::string& branch);

    /// True while a `git merge --squash` is unfinished (`SQUASH_MSG` present).
    /// Distinct from mergeInProgress(), which looks at `MERGE_HEAD`: a squash never
    /// writes one, so the two must not be conflated.
    bool squashInProgress() const;

    /// Commits a squash's staged index as an ordinary single-parent commit and clears
    /// `SQUASH_MSG`. An empty `message` uses git's own combined message.
    Oid commitSquash(const std::string& message);

    /// Rebases `branch` onto `onto` ("Rebase X onto Y"): the branch
    /// is checked out first, then its commits are replayed on top of `onto`.
    /// On conflicts the rebase is left open for rebaseContinue()/rebaseAbort().
    RebaseResult rebaseOnto(const std::string& branch, const std::string& onto);

    RebaseResult rebaseContinue();

    void rebaseAbort();
    bool rebaseInProgress() const;

    /// True when the index carries nothing that HEAD does not already have — an
    /// empty rebase step, which has no reason to become a commit.
    bool indexMatchesHead() const;

    UpstreamInfo upstreamInfo(const std::string& branch = std::string{}) const;

    /// Full ref of the branch this repository is based on — the "target" of Smart
    /// Branch Visibility. The remote's default branch (its *local* counterpart when
    /// one exists), else a local `main`/`master`, else empty, which means HEAD is
    /// its own target.
    std::string targetBranch() const;

    /// One value from the merged configuration (repository, global, system); empty
    /// when unset.
    std::string configValue(const std::string& key) const;

    /// Writes a value into the repository's own config. An empty `value` removes the
    /// key. This is configuration, not a pointer move, so callers go through
    /// `guarded`: the operation log restores refs and would have nothing to put back.
    void setConfigValue(const std::string& key, const std::string& value);

    /// What git says about a revision's signature, through `git log --format=%G?`.
    /// Reading this runs the signature check, so it is asked for one commit at a
    /// time and never while listing history.
    CommitSignature commitSignature(const std::string& revision) const;

    void checkoutCommit(const Oid& commitId);

    /// Moves `name` to `target`. Moving the branch you are on also moves the working
    /// tree with it, and refuses when that would discard uncommitted work unless
    /// `force` is set.
    void moveBranch(const std::string& name, const Oid& target, bool force = false);

    Oid mergeBase(const Oid& a, const Oid& b) const;

    /// True when `branch` is an ancestor of `target`, i.e. moving the branch
    /// forward to it needs no merge commit.
    bool canFastForward(const std::string& branch, const Oid& target) const;

    /// Moves `branch` forward to `target`. Refuses anything that is not a
    /// fast-forward, so it can never lose a commit.
    void fastForwardBranch(const std::string& branch, const Oid& target);

    void renameBranch(const std::string& from, const std::string& to);

    /// Copies commits onto the checked-out branch, oldest first. Throws when the
    /// change does not apply cleanly; the index is then left with conflict stages,
    /// which is what the conflict editor reads.
    Oid cherryPick(const std::vector<Oid>& commits);

    /// True when the stretch `onto..tip` is a single line of first parents, with no merge
    /// and no commit reachable by a second path inside it. `onto` may be `kRebaseRoot`.
    bool rangeIsLinear(const Oid& onto, const Oid& tip) const;

    /// Moves HEAD (and the branch it points at) to `commitId`. `Hard` also
    /// overwrites the working tree.
    void resetTo(const Oid& commitId, ResetMode mode);

    /// Rewrites the HEAD commit's message. Only HEAD: amending an older commit means
    /// rebuilding everything after it, which is a rebase. The tree and the author are
    /// taken from the commit being amended.
    void amendHeadMessage(const std::string& message);

    /// The same, resigning the commit when `sign` is set. The tree is kept: this
    /// rewords, it does not fold the index in. The new signature is git's own
    /// (`git commit --amend --only -S`).
    void amendHeadMessage(const std::string& message, bool sign);

    /// Replaces HEAD with a commit of the current index and `message` — the
    /// Commit Panel's "Amend previous commit". Unlike `amendHeadMessage` the staged
    /// changes are folded in. Always `git commit --amend`, so hooks and signing apply.
    Oid amendHead(const std::string& message, bool sign);

    /// Amends the commit a stopped rebase is sitting on, replacing it with the staged
    /// index. This is what finishes an `edit` step. An empty `message` keeps the
    /// commit's own. Refuses when no rebase is open.
    void amendStoppedCommit(const std::string& message);

    Oid revertCommit(const Oid& commitId);

    /// Rewrites the current branch's history with `git rebase -i`, driven by a generated
    /// todo list: squash, fixup, reword, edit, drop, in any order.
    ///
    /// The two editors git opens are answered by scripts written into the repository's
    /// temporary directory — there is nobody there to type, and a rebase waiting for an
    /// editor waits forever.
    ///
    /// `onto`, when not empty, retargets the replay (`git rebase -i --onto onto upstream`);
    /// `branch`, when not empty, is the branch to replay rather than the checked-out one.
    /// `kRebaseRoot` as `upstream` replays from the first commit. The steps' set must equal
    /// rebaseCandidates()'s — a mismatch is refused rather than silently dropping a commit.
    void rewriteHistory(const Oid& upstream, const std::vector<RewriteStep>& steps,
                        const Oid& onto = {}, const std::string& branch = {});

    /// The commits a rewrite of `tip` onto `upstream` would replay, oldest first, merges
    /// flattened out — exactly git's own todo for that range. `kRebaseRoot` means the tip's
    /// whole history.
    std::vector<RebaseCandidate> rebaseCandidates(const Oid& upstream, const Oid& tip) const;

    /// Removes a commit from the current branch's history, replaying everything above it
    /// onto its parent — or, for the first commit, from a root rebase. The rewritten branch
    /// is left checked out. Stops with a conflict like any other replay, and the rebase
    /// stays open for continue or abort.
    void dropCommit(const Oid& commitId);

    void createTag(const std::string& name, const Oid& target, const std::string& message);

    void deleteTag(const std::string& name);

    /// `git format-patch -1 <commit> --stdout`: the commit as a mailbox patch that
    /// `git am` can apply elsewhere.
    std::string formatPatch(const Oid& commitId) const;

    /// Drops the cached history walk. Call after anything that can move a ref or
    /// HEAD; the next `commits()` rebuilds it.
    void invalidateCommitWalk() const;
    /// How many commits the cached walk had to read one by one because the
    /// commit-graph does not hold them: all of them when there is no graph.
    std::size_t walkReadOutsideGraph() const;
    /// `core.commitGraph`, which is on unless the reader turned it off. Off means
    /// the walk does not read the graph and nothing here writes one.
    bool commitGraphEnabled() const;

private:
    /// `commit.gpgsign`, for the commits that finish an operation rather than being
    /// asked for with an explicit choice.
    bool gpgSignConfigured() const;
    explicit Repository(git_repository* repo);
    FileDiff untrackedFileDiff(const std::string& path) const;
    /// `git cat-file`-backed content read, used when the repository is a partial
    /// (promisor) clone: libgit2 cannot read a blob that was filtered out, while the
    /// binary fetches it on demand.
    std::string fileAtCli(const Oid& commitId, const std::string& path) const;
    /// The unified-diff text between two revisions (empty base = the empty tree) for
    /// an optional path, through the git binary — the promisor-clone read path.
    std::string diffCliText(const Oid& base, const Oid& target, const std::string& path) const;
    /// `git diff --name-status --no-renames` between two revisions (empty base = the
    /// empty tree): the file list, without reading any blob.
    std::string nameStatusCli(const Oid& base, const Oid& target) const;
    std::string workdirDiffCli(const Oid& base, const std::string& path) const;
    std::string workdirNameStatusCli(const Oid& base) const;
    /// One path's staged (`cached`) or unstaged diff through the binary, for a
    /// promisor clone whose index blobs may be missing.
    std::string indexDiffCli(bool cached, const std::string& path) const;
    /// A commit's first parent, or empty for a root commit. libgit2 answers this even in
    /// a promisor clone: commit objects are present, only trees and blobs may not be.
    std::string firstParentOf(const Oid& commitId) const;
    bool buildCommitWalk(const HistoryFilter& filter) const;
    /// The next commit id from the cached walk, including one held back to answer
    /// the previous `hasMore`.
    bool takeFromWalk(Oid& id) const;
    git_repository* m_repo = nullptr;
    bool m_partialClone = false;
    /// The tips the history walk starts from, in the order git lists them for
    /// `--all`: every ref by name, then HEAD.
    std::vector<std::array<unsigned char, 20>> walkStarts(const HistoryFilter& filter) const;
    std::string trunkBranch() const;
    /// The history walk kept between pages. Building one re-pushes every ref and
    /// counts children down to the depth it needs, which on a monorepo costs more
    /// than the page it returns; reusing it while the caller keeps asking from
    /// where the last page ended is what keeps paging linear. Dropped whenever a
    /// ref or HEAD may move.
    mutable std::unique_ptr<TopoWalk> m_walk;
    /// The commit-graph the walk reads, kept with it: the walk holds positions in it.
    mutable std::unique_ptr<CommitGraph> m_walkGraph;
    /// The stash tips the walk started from, drawn with their first parent only.
    mutable std::vector<std::array<unsigned char, 20>> m_walkStashes;
    mutable HistoryFilter m_walkFilter;
    mutable std::size_t m_walkDelivered = 0;
    mutable bool m_walkValid = false;
    mutable bool m_walkPeeked = false;
    mutable Oid m_peekId;
};

} // namespace gk
