#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gk {

using Oid = std::string;

enum class RefKind { LocalBranch, RemoteBranch, Tag, Other };

enum class HistoryScope {
    HeadOnly,
    AllRefs,
};

/// Which references the history walk starts from, and which carry labels.
///
/// An empty `visibleRefs` means every reference (`HistoryScope::AllRefs`); a
/// non-empty list names the full refs to push (`refs/heads/…`). Hiding a branch
/// is therefore *not pushing it*: the commits reachable solely from it leave the
/// walk, while shared history stays, because `git_revwalk_hide` would also drop
/// every common ancestor.
struct HistoryFilter {
    HistoryScope scope = HistoryScope::HeadOnly;
    std::vector<std::string> visibleRefs;
};

struct RefInfo {
    std::string fullName;
    std::string shortName;
    RefKind kind = RefKind::Other;
    Oid target;
};

struct Signature {
    std::string name;
    std::string email;
    std::int64_t whenUnix = 0;
    int offsetMinutes = 0;
};

/// A person to credit in a commit message's trailer block. No date: the commit's own
/// date is what git records.
struct CoAuthor {
    std::string name;
    std::string email;
};

struct CommitInfo {
    Oid id;
    std::string summary;
    std::string body;
    Signature author;
    Signature committer;
    std::vector<Oid> parents;
};

/// What `git log -1 --format=%G?` says about a commit's signature.
///
/// `status` is git's own one-letter code: `G` good, `U` good but untrusted, `X`
/// expired signature, `Y` expired key, `R` revoked key, `B` bad, `E` cannot be
/// checked (missing key), `N` no signature. `signer` and `key` are empty when
/// there is nothing to name.
struct CommitSignature {
    std::string status;
    std::string signer;
    std::string key;
};

/// One line of a file and the commit it came from.
///
/// `finalLine` is the line's number in the file now; `originalLine` its number in the
/// commit that introduced the text. `uncommitted` marks a working-tree line that no
/// commit has yet, which blame reports with an all-zero commit.
struct BlameLine {
    Oid commit;
    std::string author;
    std::string summary;
    std::int64_t whenUnix = 0;
    int finalLine = 0;
    int originalLine = 0;
    bool uncommitted = false;
};

/// One commit that touched a path — `git log --follow`'s answer. `--follow` is the
/// binary's, because it follows the file across renames and libgit2's revwalk does not.
struct FileHistoryEntry {
    Oid id;
    std::string summary;
    std::string author;
    std::int64_t whenUnix = 0;
};

enum class ChangeKind {
    Added,
    Modified,
    Deleted,
    Renamed,
    TypeChange,
    Untracked,
    Conflicted,
};

struct FileChange {
    std::string path;
    ChangeKind kind = ChangeKind::Modified;
    bool staged = false;
    bool submodule = false;
    /// For a submodule: the gitlink in the parent's index, and the commit checked out
    /// inside it (empty when not initialized).
    std::string recordedOid;
    std::string checkedOutOid;
};

enum class RepoOperation { None, Merge, Rebase, CherryPick, Revert };

/// How the graph groups commits under date separators. A timeline row is a synthetic
/// row inserted between two commits whose period differs — "Today", "Last week",
/// "March 2024".
enum class TimelinePeriod { None, Hour, Day, Week, Month, Year };

/// The stored spelling of a timeline period, and its inverse. An unknown word reads as
/// `None` rather than as whichever enumerator shares a number.
inline const char* timelinePeriodKey(TimelinePeriod period) {
    switch (period) {
    case TimelinePeriod::Hour: return "hour";
    case TimelinePeriod::Day: return "day";
    case TimelinePeriod::Week: return "week";
    case TimelinePeriod::Month: return "month";
    case TimelinePeriod::Year: return "year";
    case TimelinePeriod::None: break;
    }
    return "none";
}

inline TimelinePeriod timelinePeriodFromKey(const std::string& key) {
    if (key == "hour") {
        return TimelinePeriod::Hour;
    }
    if (key == "day") {
        return TimelinePeriod::Day;
    }
    if (key == "week") {
        return TimelinePeriod::Week;
    }
    if (key == "month") {
        return TimelinePeriod::Month;
    }
    if (key == "year") {
        return TimelinePeriod::Year;
    }
    return TimelinePeriod::None;
}

enum class ResetMode {
    Soft,
    Mixed,
    Hard,
};

struct RepoInfo {
    std::string gitDir;
    /// The shared git directory: `gitDir` normally, the main `.git` for a linked
    /// worktree — what the worktrees of one repository have in common, and therefore
    /// what per-repository view settings are keyed by.
    std::string commonDir;
    std::string workDir; ///< empty for bare repositories
    std::string headBranch;
    /// The branch the others are cut from: what `origin/HEAD` names, else the first
    /// of `main`, `master`, `trunk` that exists. A short local name, empty if none.
    std::string trunkBranch;
    Oid headId;
    bool detached = false;
    bool bare = false;
    bool partialClone = false; ///< promisor / filtered clone
    RepoOperation operation = RepoOperation::None;
};

/// The pointer state of a repository: every direct reference, where HEAD points,
/// and the tree the index holds. What the operation log records before and after
/// each completed operation — and what it puts back to undo one.
///
/// It captures pointers, not file contents: a file a `--hard` reset overwrote is
/// not in here, and cannot be recovered from it. Symbolic references other than
/// HEAD (`refs/remotes/*/HEAD`) are deliberately left out, so a restore never
/// turns one into a direct reference.
struct RefState {
    std::map<std::string, std::string> refs;
    /// HEAD's symbolic target (`refs/heads/main`); empty when HEAD is detached.
    std::string headRef;
    /// The commit HEAD resolves to; empty for an unborn branch.
    std::string headOid;
    bool detached = false;
    /// Tree id of the index. Empty when the index could not be written (a
    /// conflicted one cannot be), in which case `hasIndex` is false and a restore
    /// leaves the index alone.
    std::string indexTree;
    bool hasIndex = false;

    bool operator==(const RefState&) const = default;
};

struct ChangedFile {
    std::string path;
    std::string oldPath; ///< differs from `path` on rename/copy
    ChangeKind kind = ChangeKind::Modified;
};

enum class DiffLineKind { Context, Added, Removed };

struct DiffLine {
    DiffLineKind kind = DiffLineKind::Context;
    std::string text; ///< without the trailing newline
    int oldLine = 0;  ///< 0 when the line does not exist on that side
    int newLine = 0;
    /// True when this is the last line of its file and that file does not end in a
    /// newline. A patch that forgets it gives the staged blob a trailing newline
    /// the working file does not have.
    bool noNewline = false;
};

struct DiffHunk {
    int oldStart = 0;
    int oldLines = 0;
    int newStart = 0;
    int newLines = 0;
    std::vector<DiffLine> lines;
};

struct FileDiff {
    std::string oldPath;
    std::string newPath;
    bool binary = false;
    int added = 0;
    int removed = 0;
    /// How the path changed. `Added`/`Untracked` diff against an empty old side,
    /// `Deleted` against an empty new side — what a patch has to say `/dev/null`.
    ChangeKind kind = ChangeKind::Modified;
    /// File modes, as libgit2 reports them (0 when unknown). A new file needs its
    /// mode line or `git apply` guesses.
    int oldMode = 0;
    int newMode = 0;
    std::vector<DiffHunk> hunks;
};

enum class ConflictKind {
    BothModified,
    BothAdded,
    DeletedByUs,
    DeletedByThem,
    BothDeleted,
    AddedByUs,
    AddedByThem,
    Unknown,
};

enum class ConflictSide { Ours, Base, Theirs };

/// What a commit search looks at. `Message` and `Author` are literal, case-insensitive
/// substrings; `Sha` resolves a hash (full or an unambiguous abbreviation).
enum class CommitSearchField { Message, Author, Sha };

/// One path in the index that carries conflict stages. An empty oid means the stage is
/// absent (`ours` is empty when we deleted the file).
struct ConflictEntry {
    std::string path;
    Oid ancestor;
    Oid ours;
    Oid theirs;
    ConflictKind kind = ConflictKind::Unknown;
};

enum class MergeStatus {
    UpToDate,
    FastForward,
    Clean,
    Conflicted,
};

struct MergeResult {
    MergeStatus status = MergeStatus::UpToDate;
    std::size_t conflictCount = 0;
};

/// What `git merge --squash` did. Kept apart from MergeStatus because a squash
/// stages a change and creates nothing: its ending is a plain commit the reader
/// makes, not the merge commit `MergeStatus::Clean` means.
enum class SquashStatus {
    UpToDate,
    Staged,
    Conflicted,
};

struct SquashResult {
    SquashStatus status = SquashStatus::UpToDate;
    std::size_t conflictCount = 0;
};

/// A run of lines in a file, 1-based. `count` 0 is a point between lines: the conflict
/// there is an insertion on the other side, before line `first`.
struct LineSpan {
    std::size_t first = 0;
    std::size_t count = 0;
};

/// What a merge would do, predicted by `git merge-tree` without touching the working
/// tree or the index. `available` is false when there is nothing to predict — one side
/// already reachable from the other, so no merge would run.
struct MergePreview {
    bool available = false;
    bool clean = true;
    std::vector<std::string> conflictedPaths;
    /// Where in our side of each conflicted file the conflicts are, keyed by path.
    /// Absent for a path with no text conflict to point at (binary, delete/modify).
    std::map<std::string, std::vector<LineSpan>> conflictedLines;
};

enum class RebaseStatus {
    UpToDate,
    Completed,
    Conflicted,
};

struct RebaseResult {
    /// Steps whose change was already upstream, and were skipped in silence.
    std::size_t commitsSkipped = 0;
    RebaseStatus status = RebaseStatus::UpToDate;
    std::size_t conflictCount = 0;
    std::size_t commitsApplied = 0;
};

enum class RewriteAction {
    Keep,
    Drop,
    Squash,
    Fixup,
    Reword,
    Edit,
};

struct RewriteStep {
    Oid commit;
    RewriteAction action = RewriteAction::Keep;
    std::string message;
};

/// One commit a rewrite would replay, in the order git's todo lists them: oldest first,
/// merges flattened out. The subject is carried so the editor can show a list without a
/// second lookup per row.
struct RebaseCandidate {
    Oid id;
    std::string subject;
};

/// Rebase base naming git's `--root`; an empty base already means "none given".
inline constexpr const char* kRebaseRoot = "--root";

inline bool isRebaseRoot(const std::string& base) { return base == kRebaseRoot; }

/// One entry of `git reflog`: where a ref pointed, and why it moved.
///
/// The reflog is the record of every move a ref has made — commits, checkouts, rebases,
/// resets, and the moves another client made. It is the one place to answer "where was
/// this branch an hour ago".
struct ReflogEntry {
    std::string selector;
    std::string id;
    std::string shortId;
    std::string action;
    std::string message;
    std::string author;
    std::string date; ///< ISO 8601
};

struct UpstreamInfo {
    bool configured = false;
    std::string name;
    std::size_t ahead = 0;
    std::size_t behind = 0;
};

/// One entry of `git worktree list`.
///
/// A linked worktree shares the repository's refs and object store but has its own
/// working tree, index and HEAD — which is why the same branch cannot be checked out in
/// two of them, and why operation markers (`MERGE_HEAD`, `SQUASH_MSG`) live in the
/// worktree's own git dir, not the common one.
struct WorktreeEntry {
    std::string path;
    std::string name;
    Oid head;
    std::string branch;
    bool bare = false;
    bool detached = false;
    bool locked = false;
    std::string lockReason;
    bool prunable = false;
    bool current = false;
};

/// The synthetic working-tree node for one worktree: what the graph hangs off a
/// worktree's HEAD when that checkout has uncommitted changes.
struct WorktreeWip {
    std::string path;
    std::string name;
    Oid head;
    int staged = 0;
    int unstaged = 0;
    /// Index entries carrying conflict stages. Kept apart from `unstaged` so the graph
    /// can tell "you have edits" from "you have a conflict to settle".
    int conflicted = 0;
    /// A rebase is open in this checkout. Only the current checkout can know it cheaply.
    bool rebaseOpen = false;
    bool current = false;
};

/// State of a submodule, as the leading character of `git submodule status` says.
///
/// `InSync` and `OutOfSync` both mean the working tree is checked out; they differ in
/// whether the checked-out commit matches the gitlink recorded in the parent's index.
/// "Not initialized" is the state a fresh clone leaves: the gitlink is there, the
/// directory is not.
enum class SubmoduleState {
    InSync,
    NotInitialized,
    OutOfSync,
    Conflict,
};

/// One entry of `git submodule status`, joined with its `.gitmodules` section.
///
/// A submodule is a gitlink in the parent's index plus a nested repository in a
/// subdirectory. `name` is the section key (`submodule.<name>`), often but not always the
/// path — git allows a name with dots and slashes.
struct SubmoduleEntry {
    std::string name;
    std::string path;
    std::string url;
    std::string branch;
    Oid recordedOid;
    Oid checkedOutOid;
    SubmoduleState state = SubmoduleState::InSync;
    std::string describe;
    bool initialized = false;
    bool nested = false;
    std::string parentPath;
};

const char* toString(RefKind kind);
const char* toString(ChangeKind kind);
const char* toString(RepoOperation op);
const char* toString(MergeStatus status);
const char* toString(ConflictKind kind);
const char* toString(RebaseStatus status);
const char* toString(SubmoduleState state);

} // namespace gk
