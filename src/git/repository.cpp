#include "git/repository.h"

#include "core/conflict_regions.h"
#include "core/porcelain_status.h"
#include "core/unified_diff.h"
#include "git/cli.h"
#include "git/commit_graph.h"
#include "git/hooks.h"
#include "git/submodule.h"
#include "git/topo_walk.h"

#include <git2.h>
#include <git2/sys/index.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_set>

namespace gk {
namespace {

namespace fs = std::filesystem;

bool environmentFlag(const char* name) {
    const char* value = std::getenv(name);
    if (!value) {
        return false;
    }
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text == "1" || text == "true" || text == "yes" || text == "on";
}

void ensureInitialized() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        git_libgit2_init();
        // The binary honours GIT_CONFIG_NOSYSTEM and libgit2 does not, so the two read
        // different settings. On Windows it shows: libgit2 finds Git for Windows'
        // system config through PATH, whose `core.autocrlf=true` then rewrote line
        // endings that the binary, told to ignore it, left alone.
        if (environmentFlag("GIT_CONFIG_NOSYSTEM")) {
            git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_SYSTEM, "");
            git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_PROGRAMDATA, "");
        }
    });
}

[[noreturn]] void fail(const std::string& what) {
    const git_error* err = git_error_last();
    // libgit2 answers "no error" when its last call succeeded, and appending that
    // to a message of our own reads as a bug in the message rather than a reason.
    if (err && err->message && std::string(err->message) != "no error") {
        throw GitError(what + ": " + err->message);
    }
    throw GitError(what);
}

void check(int rc, const std::string& what) {
    if (rc < 0) {
        fail(what);
    }
}

/// Resolves a revision — a branch name, a tag, a short or a full id — to the commit
/// it names. Callers pass whichever they happen to have; "can main fast-forward to
/// feature" is the natural question and should not need the tip looked up by hand.
git_oid resolveRevisionToCommit(git_repository* repo, const std::string& revision) {
    git_object* named = nullptr;
    if (git_revparse_single(&named, repo, revision.c_str()) != 0) {
        fail("no such revision: " + revision);
    }
    git_object* commit = nullptr;
    const int peelRc = git_object_peel(&commit, named, GIT_OBJECT_COMMIT);
    git_object_free(named);
    if (peelRc != 0) {
        fail("'" + revision + "' does not name a commit");
    }
    const git_oid oid = *git_object_id(commit);
    git_object_free(commit);
    return oid;
}

Oid oidToString(const git_oid* oid) {
    if (!oid) {
        return {};
    }
    char buf[GIT_OID_HEXSZ + 1] = {0};
    git_oid_tostr(buf, sizeof(buf), oid);
    return std::string(buf);
}

std::string trimTrailingSlash(std::string s) {
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

RefKind classifyRef(const std::string& name) {
    if (name.rfind("refs/heads/", 0) == 0) {
        return RefKind::LocalBranch;
    }
    if (name.rfind("refs/remotes/", 0) == 0) {
        return RefKind::RemoteBranch;
    }
    if (name.rfind("refs/tags/", 0) == 0) {
        return RefKind::Tag;
    }
    return RefKind::Other;
}

std::string shorthand(const std::string& fullName, RefKind kind) {
    switch (kind) {
    case RefKind::LocalBranch: return fullName.substr(std::string("refs/heads/").size());
    case RefKind::RemoteBranch: return fullName.substr(std::string("refs/remotes/").size());
    case RefKind::Tag: return fullName.substr(std::string("refs/tags/").size());
    case RefKind::Other:
        // `refs/stash` is a single, well-known name; its full path is noise in a
        // pill and in the graph, so it reads as just "stash".
        return fullName == "refs/stash" ? std::string("stash") : fullName;
    }
    return fullName;
}

ChangeKind mapDelta(git_delta_t delta) {
    switch (delta) {
    case GIT_DELTA_ADDED: return ChangeKind::Added;
    case GIT_DELTA_DELETED: return ChangeKind::Deleted;
    case GIT_DELTA_RENAMED: return ChangeKind::Renamed;
    case GIT_DELTA_TYPECHANGE: return ChangeKind::TypeChange;
    case GIT_DELTA_UNTRACKED: return ChangeKind::Untracked;
    default: return ChangeKind::Modified;
    }
}

const char* pathOf(const git_diff_delta* d) {
    if (!d) {
        return nullptr;
    }
    return d->new_file.path ? d->new_file.path : d->old_file.path;
}

bool isGitlinkDelta(const git_diff_delta* d) {
    return d && (d->old_file.mode == GIT_FILEMODE_COMMIT ||
                 d->new_file.mode == GIT_FILEMODE_COMMIT);
}

Oid nonZeroOid(const Oid& value) {
    return value == std::string(GIT_OID_HEXSZ, '0') ? Oid{} : value;
}

/// True when `path` is registered as a submodule in `.gitmodules`. The gitlink may
/// be in the index without the nested repository existing (a fresh clone), so the
/// registration, not the directory, is what says "this is a submodule".
bool isRegisteredSubmodule(git_repository* repo, const char* path) {
    git_submodule* submodule = nullptr;
    if (git_submodule_lookup(&submodule, repo, path) != 0) {
        return false;
    }
    git_submodule_free(submodule);
    return true;
}

/// Submodule pointer changes, for the status list.
///
/// libgit2's own status does not report a gitlink whose checkout moved, so these are
/// computed here: a gitlink the index carries but HEAD's tree does not is a *staged*
/// change, and an out-of-sync checkout (the submodule's HEAD against the recorded
/// gitlink) is an *unstaged* one. The second question is `git submodule status`, so a
/// repository without `.gitmodules` never runs that binary at all.
std::vector<FileChange> submoduleChanges(git_repository* repo, const std::string& workDir) {
    std::vector<FileChange> out;
    if (workDir.empty() || !fs::exists(fs::path(workDir) / ".gitmodules")) {
        return out;
    }

    const auto checkedOutOf = [repo](const std::string& path) {
        git_submodule* submodule = nullptr;
        std::string head;
        if (git_submodule_lookup(&submodule, repo, path.c_str()) == 0) {
            head = nonZeroOid(oidToString(git_submodule_head_id(submodule)));
            git_submodule_free(submodule);
        }
        return head;
    };

    // Staged: the index gitlink differs from HEAD's. A tree-to-index diff is how git
    // itself asks, and only gitlink rows are taken from it.
    git_object* head = nullptr;
    if (git_revparse_single(&head, repo, "HEAD") == 0 && head) {
        git_tree* headTree = nullptr;
        if (git_object_peel(reinterpret_cast<git_object**>(&headTree), head, GIT_OBJECT_TREE) == 0) {
            git_diff* diff = nullptr;
            git_diff_options diffOpts = GIT_DIFF_OPTIONS_INIT;
            if (git_diff_tree_to_index(&diff, repo, headTree, nullptr, &diffOpts) == 0) {
                const std::size_t deltas = git_diff_num_deltas(diff);
                for (std::size_t i = 0; i < deltas; ++i) {
                    const git_diff_delta* delta = git_diff_get_delta(diff, i);
                    if (!isGitlinkDelta(delta)) {
                        continue;
                    }
                    const char* raw = pathOf(delta);
                    FileChange c;
                    c.path = raw ? raw : "";
                    c.kind = mapDelta(delta->status);
                    c.staged = true;
                    c.submodule = true;
                    c.recordedOid = nonZeroOid(oidToString(&delta->new_file.id));
                    c.checkedOutOid = checkedOutOf(c.path);
                    out.push_back(std::move(c));
                }
                git_diff_free(diff);
            }
            git_tree_free(headTree);
        }
        git_object_free(head);
    }

    // Unstaged and conflicted, from the submodule's own checkout against the gitlink.
    for (const SubmoduleEntry& entry : SubmoduleOps(workDir).list()) {
        if (entry.state == SubmoduleState::OutOfSync) {
            FileChange c;
            c.path = entry.path;
            c.kind = ChangeKind::Modified;
            c.staged = false;
            c.submodule = true;
            c.recordedOid = entry.recordedOid;
            c.checkedOutOid = entry.checkedOutOid;
            out.push_back(std::move(c));
        } else if (entry.state == SubmoduleState::Conflict) {
            FileChange c;
            c.path = entry.path;
            c.kind = ChangeKind::Conflicted;
            c.submodule = true;
            c.recordedOid = entry.recordedOid;
            c.checkedOutOid = entry.checkedOutOid;
            out.push_back(std::move(c));
        }
    }
    return out;
}

RepoOperation detectOperation(const std::string& gitDir) {
    std::error_code ec;
    auto exists = [&](const char* name) {
        return fs::exists(fs::path(gitDir) / name, ec);
    };
    if (exists("rebase-merge") || exists("rebase-apply")) {
        return RepoOperation::Rebase;
    }
    if (exists("MERGE_HEAD")) {
        return RepoOperation::Merge;
    }
    if (exists("CHERRY_PICK_HEAD")) {
        return RepoOperation::CherryPick;
    }
    if (exists("REVERT_HEAD")) {
        return RepoOperation::Revert;
    }
    return RepoOperation::None;
}

bool detectPartialClone(const std::string& gitDir) {
    std::error_code ec;
    fs::path packs = fs::path(gitDir) / "objects" / "pack";
    if (!fs::is_directory(packs, ec)) {
        return false;
    }
    for (const auto& entry : fs::directory_iterator(packs, ec)) {
        if (entry.path().extension() == ".promisor") {
            return true;
        }
    }
    return false;
}

Signature toSignature(const git_signature* sig) {
    Signature out;
    if (!sig) {
        return out;
    }
    out.name = sig->name ? sig->name : "";
    out.email = sig->email ? sig->email : "";
    out.whenUnix = static_cast<std::int64_t>(sig->when.time);
    out.offsetMinutes = sig->when.offset;
    return out;
}

git_oid parseOid(const Oid& hex) {
    git_oid oid;
    if (git_oid_fromstr(&oid, hex.c_str()) < 0) {
        fail("malformed object id '" + hex + "'");
    }
    return oid;
}

/// The committer/author identity to use for commits we create.
/// Falls back to a local identity when user.name/user.email are not set, which
/// is the common case for a freshly initialised test repository.
git_signature* defaultSignature(git_repository* repo) {
    git_signature* signature = nullptr;
    if (git_signature_default(&signature, repo) == 0 && signature) {
        return signature;
    }
    git_signature_new(&signature, "GitMycelium", "gitmycelium@localhost",
                      static_cast<git_time_t>(std::time(nullptr)), 0);
    return signature;
}

std::filesystem::path gitDirOf(git_repository* repo) {
    const char* path = git_repository_path(repo);
    return path ? std::filesystem::path(path) : std::filesystem::path();
}

std::filesystem::path workDirOf(git_repository* repo) {
    const char* path = git_repository_workdir(repo);
    return path ? std::filesystem::path(path) : std::filesystem::path();
}

/// Writes `message` to a scratch file under the git directory for the git binary to
/// read with `-F`. `runGit` has no stdin, so a message git is to consume has to live
/// in a file; it goes under the git dir so an interrupted run leaves nothing in the
/// working tree.
std::filesystem::path writeMessageFile(git_repository* repo, const std::string& name,
                                       const std::string& message) {
    const std::filesystem::path dir = gitDirOf(repo) / "gitmycelium-sign";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / name;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << message;
    return file;
}

/// Whether git would run a clean/smudge program for filter `name` (any filter when
/// empty). libgit2 runs none of them, so a file such a filter owns — git-lfs is the
/// common one — has to be staged and checked out by the git binary, or the index
/// gets the whole file instead of its pointer and the work tree gets the pointer
/// instead of the file.
bool externalFilterConfigured(git_repository* repo, const std::string& name) {
    git_config* config = nullptr;
    if (git_repository_config_snapshot(&config, repo) != 0) {
        return false;
    }
    std::string escaped;
    for (const char c : name) {
        if (std::string_view(".^$|()[]{}*+?\\").find(c) != std::string_view::npos) {
            escaped += '\\';
        }
        escaped += c;
    }
    const std::string pattern =
        "^filter\\." + (name.empty() ? std::string(".+") : escaped) + "\\.(clean|smudge|process)$";
    git_config_iterator* iterator = nullptr;
    bool found = false;
    if (git_config_iterator_glob_new(&iterator, config, pattern.c_str()) == 0) {
        git_config_entry* entry = nullptr;
        found = git_config_next(&entry, iterator) == 0;
        git_config_iterator_free(iterator);
    }
    git_config_free(config);
    return found;
}

/// `core.filemode`: false where the file system has no executable bit (git sets it so
/// on Windows), and then no file is executable whatever the OS reports.
bool trustsFileMode(git_repository* repo) {
    git_config* config = nullptr;
    if (git_repository_config_snapshot(&config, repo) != 0) {
        return true;
    }
    int value = 1;
    if (git_config_get_bool(&value, config, "core.filemode") != 0) {
        value = 1;
    }
    git_config_free(config);
    return value != 0;
}

bool pathUsesExternalFilter(git_repository* repo, const std::string& path) {
    const char* value = nullptr;
    if (git_attr_get(&value, repo, GIT_ATTR_CHECK_FILE_THEN_INDEX, path.c_str(), "filter") != 0 ||
        git_attr_value(value) != GIT_ATTR_VALUE_STRING) {
        return false;
    }
    return externalFilterConfigured(repo, value);
}

void stageWithGit(git_repository* repo, const std::vector<std::string>& paths) {
    const std::string workDir = workDirOf(repo).string();
    if (workDir.empty()) {
        fail("cannot stage in a bare repository");
    }
    std::vector<std::string> args = {"add"};
    if (paths.empty()) {
        args.push_back("-A");
    } else {
        args.push_back("--");
        args.insert(args.end(), paths.begin(), paths.end());
    }
    const ProcessResult run = runGit(args, workDir);
    if (run.exitCode != 0) {
        fail("cannot stage: " + failureText(run));
    }
}

/// `git switch`, which runs the filters (and the post-checkout hook git-lfs installs)
/// that a libgit2 checkout skips.
void switchWithGit(git_repository* repo, const std::vector<std::string>& switchArgs) {
    const std::string workDir = workDirOf(repo).string();
    if (workDir.empty()) {
        fail("cannot check out in a bare repository");
    }
    std::vector<std::string> args = {"switch"};
    args.insert(args.end(), switchArgs.begin(), switchArgs.end());
    const ProcessResult run = runGit(args, workDir);
    if (run.exitCode != 0) {
        fail("cannot check out " + switchArgs.back() + ": " + failureText(run));
    }
}

/// Moves the index and the working tree to `target`'s tree through the binary, so a
/// clean/smudge filter (git-lfs) runs as it would for git's own checkout. Safe, it is
/// `read-tree -m -u HEAD <target>` — git's two-tree checkout, which refuses when a
/// local change is in the way, as GIT_CHECKOUT_SAFE does; forced, `read-tree --reset
/// -u`, which discards them. HEAD and the refs are left to the caller.
void readTreeWithGit(git_repository* repo, const std::string& target, bool force,
                     const std::string& refusal) {
    const std::string workDir = workDirOf(repo).string();
    if (workDir.empty()) {
        fail("cannot check out in a bare repository");
    }
    const ProcessResult run =
        force ? runGit({"read-tree", "--reset", "-u", target}, workDir)
              : runGit({"read-tree", "-m", "-u", "HEAD", target}, workDir);
    if (run.exitCode != 0) {
        fail(refusal + ": " + failureText(run));
    }
}

std::string shellQuoted(const std::string& text) {
    std::string quoted = "'";
    for (const char c : text) {
        quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return quoted + "'";
}

/// The hooks `git commit` runs. libgit2 runs none of them, so a repository with one
/// enabled commits through the binary: a pre-commit check that never runs is a check
/// the reader believes they have.
bool commitHooksEnabled(const std::string& workDir) {
    if (workDir.empty()) {
        return false;
    }
    for (const HookEntry& hook : HooksOps(workDir).list()) {
        if (hook.state == HookState::Enabled &&
            (hook.name == "pre-commit" || hook.name == "prepare-commit-msg" ||
             hook.name == "commit-msg" || hook.name == "post-commit")) {
            return true;
        }
    }
    return false;
}

enum class GitCommitKind { New, Amend, AmendMessage };

/// `git commit` with the message verbatim, signed or explicitly not. It also finishes
/// whatever operation is open — a MERGE_HEAD makes it a merge commit and a SQUASH_MSG
/// is consumed — exactly as it does on the command line.
void commitWithGit(git_repository* repo, const std::string& message, bool sign,
                   GitCommitKind kind) {
    const std::string workDir = workDirOf(repo).string();
    if (workDir.empty()) {
        fail("cannot commit in a bare repository");
    }
    const std::filesystem::path messageFile = writeMessageFile(
        repo, kind == GitCommitKind::New ? "commit-message" : "amend-message", message);
    std::vector<std::string> args = {"commit"};
    if (kind == GitCommitKind::Amend) {
        args.push_back("--amend");
    }
    if (kind == GitCommitKind::AmendMessage) {
        // `--only` with no paths keeps the amended commit's tree, so a reword does not
        // fold in whatever happens to be staged.
        args.push_back("--amend");
        args.push_back("--only");
    }
    args.push_back(sign ? "-S" : "--no-gpg-sign");
    args.push_back("-F");
    args.push_back(messageFile.string());
    args.push_back("--cleanup=verbatim");
    const ProcessResult run = runGit(args, workDir);
    if (run.exitCode != 0) {
        fail(std::string(sign ? "cannot create the signed commit: " : "cannot create the commit: ") +
             failureText(run));
    }
}

ConflictKind classifyConflict(bool hasAncestor, bool hasOurs, bool hasTheirs) {
    if (hasOurs && hasTheirs) {
        return hasAncestor ? ConflictKind::BothModified : ConflictKind::BothAdded;
    }
    if (hasOurs) {
        return hasAncestor ? ConflictKind::DeletedByThem : ConflictKind::AddedByUs;
    }
    if (hasTheirs) {
        return hasAncestor ? ConflictKind::DeletedByUs : ConflictKind::AddedByThem;
    }
    return ConflictKind::BothDeleted;
}

struct CommitDiff {
    git_commit* commit = nullptr;
    git_tree* tree = nullptr;
    git_tree* parentTree = nullptr;
    git_diff* diff = nullptr;

    ~CommitDiff() {
        if (diff) {
            git_diff_free(diff);
        }
        if (parentTree) {
            git_tree_free(parentTree);
        }
        if (tree) {
            git_tree_free(tree);
        }
        if (commit) {
            git_commit_free(commit);
        }
    }
};

void diffTrees(git_repository* repo, git_tree* base, git_tree* target,
               const std::vector<std::string>& paths, git_diff** out) {
    std::vector<char*> specs;
    specs.reserve(paths.size());
    for (const auto& p : paths) {
        specs.push_back(const_cast<char*>(p.c_str()));
    }

    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    opts.flags = GIT_DIFF_INCLUDE_TYPECHANGE;
    if (!specs.empty()) {
        opts.pathspec.strings = specs.data();
        opts.pathspec.count = specs.size();
    }

    check(git_diff_tree_to_tree(out, repo, base, target, &opts), "cannot diff trees");
}

void buildCommitDiff(git_repository* repo, const Oid& commitId,
                     const std::vector<std::string>& paths, CommitDiff& out) {
    const git_oid oid = parseOid(commitId);
    check(git_commit_lookup(&out.commit, repo, &oid), "cannot look up commit " + commitId);
    check(git_commit_tree(&out.tree, out.commit), "cannot read commit tree");

    if (git_commit_parentcount(out.commit) > 0) {
        git_commit* parent = nullptr;
        if (git_commit_parent(&parent, out.commit, 0) == 0) {
            git_commit_tree(&out.parentTree, parent);
            git_commit_free(parent);
        }
    }

    diffTrees(repo, out.parentTree, out.tree, paths, &out.diff);
}

/// Every stash, newest first (`stash@{0}`, `stash@{1}`, …): `refs/stash` names only
/// the newest, the others live in its reflog.
std::vector<git_oid> stashEntries(git_repository* repo) {
    std::vector<git_oid> out;
    git_reflog* log = nullptr;
    if (git_reflog_read(&log, repo, "refs/stash") != 0) {
        return out;
    }
    const std::size_t count = git_reflog_entrycount(log);
    for (std::size_t i = 0; i < count; ++i) {
        if (const git_reflog_entry* entry = git_reflog_entry_byindex(log, i)) {
            out.push_back(*git_reflog_entry_id_new(entry));
        }
    }
    git_reflog_free(log);
    return out;
}

/// The tree of a stash's untracked files (its third parent), or null when the
/// commit is not a stash or kept none. A stash made with untracked files holds them
/// only there: diffing the stash against HEAD alone shows nothing for them.
git_tree* stashUntrackedTree(git_repository* repo, git_commit* commit) {
    if (git_commit_parentcount(commit) != 3) {
        return nullptr;
    }
    const git_oid* id = git_commit_id(commit);
    const std::vector<git_oid> stashes = stashEntries(repo);
    if (std::none_of(stashes.begin(), stashes.end(),
                     [id](const git_oid& stash) { return git_oid_equal(&stash, id) != 0; })) {
        return nullptr;
    }
    git_commit* untracked = nullptr;
    if (git_commit_parent(&untracked, commit, 2) != 0) {
        return nullptr;
    }
    git_tree* tree = nullptr;
    git_commit_tree(&tree, untracked);
    git_commit_free(untracked);
    return tree;
}

void fillFromPatch(git_patch* patch, FileDiff& out) {
    const std::size_t hunkCount = git_patch_num_hunks(patch);
    for (std::size_t hi = 0; hi < hunkCount; ++hi) {
        const git_diff_hunk* rawHunk = nullptr;
        std::size_t linesInHunk = 0;
        if (git_patch_get_hunk(&rawHunk, &linesInHunk, patch, hi) < 0) {
            git_patch_free(patch);
            fail("cannot read hunk");
        }

        DiffHunk hunk;
        hunk.oldStart = rawHunk->old_start;
        hunk.oldLines = rawHunk->old_lines;
        hunk.newStart = rawHunk->new_start;
        hunk.newLines = rawHunk->new_lines;

        for (std::size_t li = 0; li < linesInHunk; ++li) {
            const git_diff_line* rawLine = nullptr;
            if (git_patch_get_line_in_hunk(&rawLine, patch, hi, li) < 0) {
                git_patch_free(patch);
                fail("cannot read diff line");
            }

            DiffLine line;
            switch (rawLine->origin) {
            case GIT_DIFF_LINE_ADDITION: line.kind = DiffLineKind::Added; ++out.added; break;
            case GIT_DIFF_LINE_DELETION: line.kind = DiffLineKind::Removed; ++out.removed; break;
            case GIT_DIFF_LINE_CONTEXT: line.kind = DiffLineKind::Context; break;
            case GIT_DIFF_LINE_CONTEXT_EOFNL:
            case GIT_DIFF_LINE_ADD_EOFNL:
            case GIT_DIFF_LINE_DEL_EOFNL:
                // "\ No newline at end of file" belongs to the line just before it.
                // Dropping it would give a staged blob a trailing newline the file
                // does not have.
                if (!hunk.lines.empty()) {
                    hunk.lines.back().noNewline = true;
                }
                continue;
            default:
                continue; // file and hunk headers, binary markers
            }
            // libgit2 stores the line without its +/-/space origin character.
            line.text.assign(rawLine->content, static_cast<std::size_t>(rawLine->content_len));
            while (!line.text.empty() &&
                   (line.text.back() == '\n' || line.text.back() == '\r')) {
                line.text.pop_back();
            }
            line.oldLine = rawLine->old_lineno;
            line.newLine = rawLine->new_lineno;
            hunk.lines.push_back(std::move(line));
        }
        out.hunks.push_back(std::move(hunk));
    }

    git_patch_free(patch);
}

void fillFileDiff(git_diff* diff, std::size_t index, FileDiff& out) {
    git_patch* patch = nullptr;
    if (git_patch_from_diff(&patch, diff, index) < 0) {
        fail("cannot create patch");
    }
    fillFromPatch(patch, out);
}

} // namespace

void Repository::initLibrary() { ensureInitialized(); }

std::unique_ptr<Repository> Repository::open(const std::string& path) {
    ensureInitialized();
    git_repository* raw = nullptr;
    if (git_repository_open(&raw, path.c_str()) < 0) {
        fail("cannot open repository at '" + path + "'");
    }
    return std::unique_ptr<Repository>(new Repository(raw));
}

std::unique_ptr<Repository> Repository::init(const std::string& path,
                                             const std::string& initialBranch) {
    ensureInitialized();
    git_repository_init_options options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
    // MKPATH makes the directory chain; NO_REINIT refuses an existing repository
    // rather than layering a new HEAD over somebody's history.
    options.flags = GIT_REPOSITORY_INIT_MKPATH | GIT_REPOSITORY_INIT_NO_REINIT;
    options.initial_head = initialBranch.empty() ? nullptr : initialBranch.c_str();
    git_repository* raw = nullptr;
    if (git_repository_init_ext(&raw, path.c_str(), &options) < 0) {
        fail("cannot create repository at '" + path + "'");
    }
    return std::unique_ptr<Repository>(new Repository(raw));
}

Repository::Repository(git_repository* repo) : m_repo(repo) {
    // Read once, at open: whether the repo is a promisor clone decides the content
    // read path, and it does not change while the handle is open.
    m_partialClone = detectPartialClone(gitDirOf(repo).string());
}

Repository::~Repository() {
    invalidateCommitWalk();
    if (m_repo) {
        git_repository_free(m_repo);
        m_repo = nullptr;
    }
}

RepoInfo Repository::info() const {
    RepoInfo out;
    out.gitDir = trimTrailingSlash(git_repository_path(m_repo) ? git_repository_path(m_repo) : "");
    // A linked worktree's git dir is `.git/worktrees/<name>`; the common dir is the
    // main `.git`. Refs and objects live in the common dir, and so do the view
    // settings this key exists for.
    const char* common = git_repository_commondir(m_repo);
    out.commonDir = trimTrailingSlash(common ? common : out.gitDir);
    const char* wd = git_repository_workdir(m_repo);
    out.workDir = wd ? trimTrailingSlash(wd) : "";
    out.bare = git_repository_is_bare(m_repo) != 0;
    out.detached = git_repository_head_detached(m_repo) == 1;
    out.partialClone = detectPartialClone(out.gitDir);
    out.operation = detectOperation(out.gitDir);

    git_reference* head = nullptr;
    const int rc = git_repository_head(&head, m_repo);
    if (rc == 0 && head) {
        const char* name = git_reference_name(head);
        const RefKind kind = classifyRef(name ? name : "");
        if (!out.detached && kind == RefKind::LocalBranch) {
            out.headBranch = shorthand(name, kind);
        }
        out.headId = oidToString(git_reference_target(head));
        git_reference_free(head);
    }
    out.trunkBranch = trunkBranch();
    return out;
}

std::string Repository::trunkBranch() const {
    git_reference* remoteHead = nullptr;
    if (git_reference_lookup(&remoteHead, m_repo, "refs/remotes/origin/HEAD") == 0) {
        const char* target = git_reference_symbolic_target(remoteHead);
        const std::string name = target ? target : "";
        git_reference_free(remoteHead);
        const std::string prefix = "refs/remotes/origin/";
        if (name.rfind(prefix, 0) == 0 && name.size() > prefix.size()) {
            return name.substr(prefix.size());
        }
    }
    for (const char* candidate : {"main", "master", "trunk"}) {
        for (const std::string& ref : {std::string("refs/heads/") + candidate,
                                       std::string("refs/remotes/origin/") + candidate}) {
            git_reference* found = nullptr;
            if (git_reference_lookup(&found, m_repo, ref.c_str()) == 0) {
                git_reference_free(found);
                return candidate;
            }
        }
    }
    return {};
}

std::vector<RefInfo> Repository::refs() const {
    std::vector<RefInfo> out;
    git_reference_iterator* it = nullptr;
    check(git_reference_iterator_new(&it, m_repo), "cannot iterate refs");

    git_reference* ref = nullptr;
    while (git_reference_next(&ref, it) == 0) {
        const char* rawName = git_reference_name(ref);
        const std::string full = rawName ? rawName : "";

        git_reference* resolved = nullptr;
        const git_oid* target = nullptr;
        if (git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC) {
            if (git_reference_resolve(&resolved, ref) == 0) {
                target = git_reference_target(resolved);
            }
        } else {
            target = git_reference_target(ref);
        }

        RefInfo info;
        info.fullName = full;
        info.kind = classifyRef(full);
        info.shortName = shorthand(full, info.kind);
        info.target = oidToString(target);
        out.push_back(std::move(info));

        if (resolved) {
            git_reference_free(resolved);
        }
        git_reference_free(ref);
        ref = nullptr;
    }
    git_reference_iterator_free(it);
    return out;
}

RefState Repository::snapshotRefs() const {
    RefState state;

    // Every direct reference. Symbolic ones other than HEAD (refs/remotes/*/HEAD)
    // are left out: putting one back as a direct ref would change what it is.
    git_reference_iterator* iterator = nullptr;
    check(git_reference_iterator_new(&iterator, m_repo), "cannot iterate refs");
    git_reference* ref = nullptr;
    while (git_reference_next(&ref, iterator) == 0) {
        if (git_reference_type(ref) == GIT_REFERENCE_DIRECT) {
            const char* rawName = git_reference_name(ref);
            const git_oid* target = git_reference_target(ref);
            if (rawName && target) {
                state.refs[rawName] = oidToString(target);
            }
        }
        git_reference_free(ref);
        ref = nullptr;
    }
    git_reference_iterator_free(iterator);

    // HEAD: its symbolic target when there is one, so "put the branch back" means
    // the branch and not a detached commit.
    git_reference* head = nullptr;
    if (git_reference_lookup(&head, m_repo, "HEAD") == 0) {
        if (git_reference_type(head) == GIT_REFERENCE_SYMBOLIC) {
            const char* symbolic = git_reference_symbolic_target(head);
            state.headRef = symbolic ? symbolic : "";
            git_reference* resolved = nullptr;
            if (git_reference_resolve(&resolved, head) == 0) {
                state.headOid = oidToString(git_reference_target(resolved));
                git_reference_free(resolved);
            }
        } else {
            state.detached = true;
            state.headOid = oidToString(git_reference_target(head));
        }
        git_reference_free(head);
    }

    // The index tree, when the index has one to write. A conflicted index cannot,
    // and nothing the log records leaves one behind.
    git_index* index = nullptr;
    if (git_repository_index(&index, m_repo) == 0) {
        // The binary writes the index too; a snapshot of libgit2's cached copy would
        // record the index as it was before the git command the operation just ran.
        git_index_read(index, 0);
        if (!git_index_has_conflicts(index)) {
            git_oid tree;
            if (git_index_write_tree(&tree, index) == 0) {
                state.indexTree = oidToString(&tree);
                state.hasIndex = true;
            }
        }
        git_index_free(index);
    }

    return state;
}

void Repository::applyRefDelta(const RefState& target, const RefState& from,
                               bool checkoutWorkingTree) {
    // Whether HEAD moved to a different *place* — another branch, or a detached
    // commit — as opposed to the same branch's tip moving. A commit or a reset is
    // the second, and their whole point is to leave the working tree and the index
    // alone so the changes survive.
    const bool headMovedToAnotherPlace =
        target.detached != from.detached ||
        (!target.detached && target.headRef != from.headRef) ||
        (target.detached && target.headOid != from.headOid);

    // The working tree first, while nothing has moved yet: a safe checkout that refuses
    // (local changes in the way) must leave the repository exactly as it was. Moving
    // HEAD and the refs first and checking out last left a HEAD on one branch over the
    // working tree of another — every file reported as changed.
    if (checkoutWorkingTree && headMovedToAnotherPlace && !target.headOid.empty()) {
        readTreeWithGit(m_repo, target.headOid, false,
                        "the working tree would be overwritten; commit or stash those changes "
                        "first");
    }

    // HEAD next: deleting the branch it used to point at is only safe once it
    // points somewhere else.
    if (headMovedToAnotherPlace) {
        if (target.detached) {
            if (!target.headOid.empty()) {
                git_oid oid;
                check(git_oid_fromstr(&oid, target.headOid.c_str()), "bad HEAD object id");
                check(git_repository_set_head_detached(m_repo, &oid), "cannot detach HEAD");
            }
        } else if (!target.headRef.empty()) {
            check(git_repository_set_head(m_repo, target.headRef.c_str()), "cannot set HEAD");
        }
    }

    // The refs the operation touched, through one transaction: created, moved and
    // removed together, or not at all. A ref the operation left alone is not
    // touched here either — undoing a commit must not disturb a fetched branch.
    git_transaction* transaction = nullptr;
    check(git_transaction_new(&transaction, m_repo), "cannot begin a ref transaction");
    try {
        for (const auto& entry : target.refs) {
            const std::string& name = entry.first;
            const auto before = from.refs.find(name);
            if (before != from.refs.end() && before->second == entry.second) {
                continue; // the operation did not touch this one
            }
            check(git_transaction_lock_ref(transaction, name.c_str()),
                  "cannot lock ref '" + name + "'");
            git_oid oid;
            if (git_oid_fromstr(&oid, entry.second.c_str()) != 0) {
                fail("bad object id for ref '" + name + "'");
            }
            check(git_transaction_set_target(transaction, name.c_str(), &oid, nullptr, nullptr),
                  "cannot set ref '" + name + "'");
        }
        for (const auto& entry : from.refs) {
            const std::string& name = entry.first;
            if (target.refs.find(name) != target.refs.end()) {
                continue;
            }
            check(git_transaction_lock_ref(transaction, name.c_str()),
                  "cannot lock ref '" + name + "'");
            check(git_transaction_remove(transaction, name.c_str()),
                  "cannot remove ref '" + name + "'");
        }
        check(git_transaction_commit(transaction), "cannot apply the ref transaction");
    } catch (...) {
        git_transaction_free(transaction);
        throw;
    }
    git_transaction_free(transaction);

    if (target.hasIndex && !target.indexTree.empty() && target.indexTree != from.indexTree) {
        git_oid treeId;
        check(git_oid_fromstr(&treeId, target.indexTree.c_str()), "bad index tree id");
        git_tree* tree = nullptr;
        check(git_tree_lookup(&tree, m_repo, &treeId), "cannot find the recorded index tree");
        git_index* index = nullptr;
        int rc = git_repository_index(&index, m_repo);
        if (rc == 0) {
            rc = git_index_read_tree(index, tree);
            if (rc == 0) {
                rc = git_index_write(index);
            }
            git_index_free(index);
        }
        git_tree_free(tree);
        check(rc, "cannot restore the index");
    }
}

std::vector<CommitInfo> Repository::commits(std::size_t skip, std::size_t limit, bool* hasMore,
                                            const HistoryFilter& filter) const {
    if (hasMore) {
        *hasMore = false;
    }

    std::vector<CommitInfo> out;
    const bool sameFilter = m_walkFilter.scope == filter.scope &&
                            m_walkFilter.visibleRefs == filter.visibleRefs &&
                            m_walkFilter.firstParentHistory == filter.firstParentHistory;
    // The walk is reused only while the caller keeps asking from where the last
    // page ended. A jump or a new filter rebuilds it, and the fast-forward below
    // is paid once, never twice for the same row.
    if (!(m_walkValid && sameFilter && skip == m_walkDelivered)) {
        invalidateCommitWalk();
        if (!buildCommitWalk(filter)) {
            return out;
        }
        Oid ignored;
        std::size_t dropped = 0;
        while (dropped < skip && takeFromWalk(ignored)) {
            ++dropped;
        }
        m_walkDelivered = dropped;
        m_walkFilter = filter;
        m_walkValid = true;
    }

    Oid id;
    while (out.size() < limit && takeFromWalk(id)) {
        const git_oid oid = parseOid(id);
        git_commit* commit = nullptr;
        if (git_commit_lookup(&commit, m_repo, &oid) < 0) {
            fail("cannot look up commit");
        }
        CommitInfo ci;
        ci.id = id;
        const char* msg = git_commit_message(commit);
        const char* sum = git_commit_summary(commit);
        ci.summary = sum ? sum : "";
        if (msg) {
            const std::string full(msg);
            const auto pos = full.find('\n');
            ci.body = pos == std::string::npos ? std::string() : full.substr(pos + 1);
        }
        ci.author = toSignature(git_commit_author(commit));
        ci.committer = toSignature(git_commit_committer(commit));
        unsigned parents = git_commit_parentcount(commit);
        if (parents > 1 && std::any_of(m_walkStashes.begin(), m_walkStashes.end(),
                                       [&oid](const TopoWalk::RawOid& stash) {
                                           return std::memcmp(stash.data(), oid.id,
                                                              stash.size()) == 0;
                                       })) {
            parents = 1;
        }
        ci.parents.reserve(parents);
        for (unsigned i = 0; i < parents; ++i) {
            const git_oid* parent = git_commit_parent_id(commit, i);
            TopoWalk::RawOid raw;
            std::memcpy(raw.data(), parent->id, raw.size());
            if (i > 0 && m_walk->omitted(raw)) {
                continue;
            }
            ci.parents.push_back(oidToString(parent));
        }
        out.push_back(std::move(ci));
        git_commit_free(commit);
        ++m_walkDelivered;
    }

    if (hasMore && out.size() == limit) {
        Oid peek;
        if (takeFromWalk(peek)) {
            *hasMore = true;
            m_peekId = peek;
            m_walkPeeked = true;
        }
    }
    return out;
}

bool Repository::takeFromWalk(Oid& id) const {
    if (m_walkPeeked) {
        id = m_peekId;
        m_walkPeeked = false;
        return true;
    }
    TopoWalk::RawOid raw;
    if (!m_walk || !m_walk->next(raw)) {
        return false;
    }
    git_oid oid;
    std::memcpy(oid.id, raw.data(), raw.size());
    id = oidToString(&oid);
    return true;
}

std::vector<TopoWalk::RawOid> Repository::walkStarts(const HistoryFilter& filter) const {
    const auto peel = [](git_reference* ref, TopoWalk::RawOid& out) {
        git_object* peeled = nullptr;
        if (git_reference_peel(&peeled, ref, GIT_OBJECT_COMMIT) != 0) {
            return false;
        }
        std::memcpy(out.data(), git_object_id(peeled)->id, out.size());
        git_object_free(peeled);
        return true;
    };
    const std::unordered_set<std::string> visible(filter.visibleRefs.begin(),
                                                  filter.visibleRefs.end());
    const auto isWalked = [&visible](const std::string& name) {
        if (!visible.empty()) {
            // A hidden branch is simply not pushed: hiding the ref instead would
            // take the common ancestors with it.
            return visible.count(name) > 0;
        }
        // A Git client is expected to draw the whole repository, not just the line
        // you happen to be standing on. Stashes are commits too, and they are
        // drawn as nodes; without the ref the stash's own commit is
        // unreachable from any branch and never appears.
        for (const char* prefix : {"refs/heads/", "refs/remotes/", "refs/tags/"}) {
            if (name.rfind(prefix, 0) == 0) {
                return true;
            }
        }
        return name == "refs/stash";
    };

    // Peeled while iterating: looking each ref up again by name afterwards costs a
    // refdb round trip per ref, which is most of the time on thousands of branches.
    std::vector<std::pair<std::string, TopoWalk::RawOid>> named;
    if (filter.scope == HistoryScope::AllRefs) {
        git_reference_iterator* it = nullptr;
        if (git_reference_iterator_new(&it, m_repo) == 0) {
            git_reference* ref = nullptr;
            while (git_reference_next(&ref, it) == 0) {
                std::string name = git_reference_name(ref);
                TopoWalk::RawOid id;
                if (isWalked(name) && peel(ref, id)) {
                    if (name == "refs/stash") {
                        m_walkStashes.push_back(id);
                    }
                    named.emplace_back(std::move(name), id);
                }
                git_reference_free(ref);
            }
            git_reference_iterator_free(it);
        }
        std::sort(named.begin(), named.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    std::vector<TopoWalk::RawOid> tips;
    tips.reserve(named.size() + 1);
    for (const auto& entry : named) {
        tips.push_back(entry.second);
    }
    // `refs/stash` is only the newest stash; the older ones are drawn too.
    if (!m_walkStashes.empty()) {
        const std::vector<git_oid> stashes = stashEntries(m_repo);
        for (std::size_t i = 1; i < stashes.size(); ++i) {
            TopoWalk::RawOid id;
            std::memcpy(id.data(), stashes[i].id, id.size());
            m_walkStashes.push_back(id);
            tips.push_back(id);
        }
    }
    git_reference* head = nullptr;
    if (git_repository_head(&head, m_repo) == 0) {
        TopoWalk::RawOid id;
        if (peel(head, id)) {
            tips.push_back(id);
        }
        git_reference_free(head);
    }
    return tips;
}

bool Repository::buildCommitWalk(const HistoryFilter& filter) const {
    m_walkStashes.clear();
    const std::vector<TopoWalk::RawOid> tips = walkStarts(filter);
    if (tips.empty()) {
        return false;
    }

    git_buf objects = GIT_BUF_INIT;
    if (commitGraphEnabled() &&
        git_repository_item_path(&objects, m_repo, GIT_REPOSITORY_ITEM_OBJECTS) == 0) {
        m_walkGraph = CommitGraph::open(objects.ptr);
        git_buf_dispose(&objects);
    }
    // Read raw from the object database rather than through git_commit_lookup: the
    // walk only needs the parents and the date, and every parsed commit would stay
    // in libgit2's object cache — most of a gigabyte on a history with no graph.
    git_odb* rawOdb = nullptr;
    if (git_repository_odb(&rawOdb, m_repo) != 0) {
        return false;
    }
    const std::shared_ptr<git_odb> odb(rawOdb, git_odb_free);
    m_walk = std::make_unique<TopoWalk>(
        m_walkGraph.get(), [odb](const TopoWalk::RawOid& id, WalkCommit& out) {
            git_oid oid;
            std::memcpy(oid.id, id.data(), id.size());
            git_odb_object* object = nullptr;
            if (git_odb_read(&object, odb.get(), &oid) != 0) {
                return false;
            }
            const bool parsed =
                git_odb_object_type(object) == GIT_OBJECT_COMMIT &&
                parseCommitHeader(static_cast<const char*>(git_odb_object_data(object)),
                                  git_odb_object_size(object), out);
            git_odb_object_free(object);
            return parsed;
        });
    m_walk->setFirstParentHistory(filter.firstParentHistory);
    m_walk->start(tips, m_walkStashes);
    return true;
}

std::size_t Repository::walkReadOutsideGraph() const {
    return m_walk ? m_walk->readOutsideGraph() : 0;
}

bool Repository::commitGraphEnabled() const {
    std::string value = configValue("core.commitGraph");
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value != "false" && value != "no" && value != "off" && value != "0";
}

void Repository::invalidateCommitWalk() const {
    m_walk.reset();
    m_walkGraph.reset();
    m_walkValid = false;
    m_walkPeeked = false;
    m_walkDelivered = 0;
}

std::unordered_map<std::string, std::vector<RefInfo>> Repository::refsByCommit(
    const HistoryFilter& filter) const {
    std::unordered_map<std::string, std::vector<RefInfo>> out;

    const auto visible = [&filter](const RefInfo& ref) {
        if (filter.visibleRefs.empty()) {
            return true;
        }
        return std::find(filter.visibleRefs.begin(), filter.visibleRefs.end(), ref.fullName) !=
               filter.visibleRefs.end();
    };

    for (const RefInfo& ref : refs()) {
        // "origin/HEAD" is a symref, not a branch worth labelling.
        if (ref.shortName.size() >= 5 &&
            ref.shortName.compare(ref.shortName.size() - 5, 5, "/HEAD") == 0) {
            continue;
        }
        // A hidden branch keeps no label: removing it from the walk but leaving
        // its pill in place would name a line that is not drawn.
        if (!visible(ref)) {
            continue;
        }

        std::string commitId = ref.target;
        if (ref.kind == RefKind::Tag && ref.target.size() == GIT_OID_HEXSZ) {
            // An annotated tag points at a tag object; the label belongs on the
            // commit it ultimately refers to.
            git_object* object = nullptr;
            const git_oid tagOid = parseOid(ref.target);
            if (git_object_lookup(&object, m_repo, &tagOid, GIT_OBJECT_ANY) == 0 && object) {
                git_object* peeled = nullptr;
                if (git_object_peel(&peeled, object, GIT_OBJECT_COMMIT) == 0 && peeled) {
                    commitId = oidToString(git_object_id(peeled));
                    git_object_free(peeled);
                }
                git_object_free(object);
            }
        }
        out[commitId].push_back(ref);
    }
    // The older stashes are labelled like the newest, `stash@{n}`.
    if (visible(RefInfo{"refs/stash", "stash", RefKind::Other, {}})) {
        const std::vector<git_oid> stashes = stashEntries(m_repo);
        for (std::size_t i = 1; i < stashes.size(); ++i) {
            const std::string id = oidToString(&stashes[i]);
            out[id].push_back(RefInfo{"refs/stash@{" + std::to_string(i) + "}",
                                      "stash@{" + std::to_string(i) + "}", RefKind::Other, id});
        }
    }

    // Local branches first, then remotes, then tags — the order the labels are
    // stacked in a row.
    for (auto& [oid, bucket] : out) {
        std::stable_sort(bucket.begin(), bucket.end(), [](const RefInfo& a, const RefInfo& b) {
            return static_cast<int>(a.kind) < static_cast<int>(b.kind);
        });
    }
    return out;
}

std::vector<FileChange> Repository::status() const {
    const std::string workDir = workDirOf(m_repo).string();
    if (workDir.empty()) {
        return {};
    }
    std::vector<FileChange> out = worktreeStatus(workDir);
    std::vector<FileChange> submodules = submoduleChanges(m_repo, workDir);
    out.insert(out.end(), std::make_move_iterator(submodules.begin()),
               std::make_move_iterator(submodules.end()));
    return out;
}

std::vector<FileChange> Repository::submoduleStatus() const {
    return submoduleChanges(m_repo, workDirOf(m_repo).string());
}

std::vector<FileChange> Repository::worktreeStatus(const std::string& workDir) {
    if (workDir.empty()) {
        return {};
    }
    // The binary, not libgit2: it honours the reader's fsmonitor and untracked
    // cache, which libgit2 has neither of, and on a 100 000-file tree it answered
    // in 18 ms where libgit2 took 170 — a status the window reads on every refresh.
    // Renames are asked for explicitly because libgit2 reported them (HEAD to
    // index), and submodules are left out because the gitlink changes are computed
    // below, exactly as before.
    const ProcessResult run =
        runGit({"status", "--porcelain=v2", "-z", "--untracked-files=all", "--renames",
                "--ignore-submodules=all"},
               workDir);
    if (run.exitCode != 0) {
        fail("cannot read status: " + failureText(run));
    }
    return parsePorcelainStatus(run.out);
}

std::vector<std::string> Repository::ignoredPaths(const std::string& workDir) {
    std::vector<std::string> out;
    if (workDir.empty()) {
        return out;
    }
    const ProcessResult run =
        runGit({"ls-files", "--others", "--ignored", "--directory", "--exclude-standard", "-z"},
               workDir);
    if (run.exitCode != 0) {
        return out;
    }
    std::size_t start = 0;
    while (start < run.out.size()) {
        std::size_t end = run.out.find('\0', start);
        if (end == std::string::npos) {
            end = run.out.size();
        }
        if (end > start) {
            out.emplace_back(run.out, start, end - start);
        }
        start = end + 1;
    }
    return out;
}

void Repository::stageFile(const std::string& path) {
    git_index* index = nullptr;
    check(git_repository_index(&index, m_repo), "cannot read the index");
    // The git binary writes the index too, and libgit2 keeps the copy it read
    // when the repository was opened. Every write path re-reads first, or a file
    // staged by `git add` disappears the moment we touch the index.
    git_index_read(index, 1);

    // A submodule is a gitlink, not a file: `git_index_add_bypath` cannot record a
    // directory, so the binary records the checked-out commit (`git add <path>`).
    // The registration is checked too, because an uninitialized submodule has a
    // gitlink in the index and no nested repository to read.
    const git_index_entry* existing = git_index_get_bypath(index, path.c_str(), 0);
    const bool gitlink = existing && existing->mode == GIT_FILEMODE_COMMIT;
    if (gitlink || isRegisteredSubmodule(m_repo, path.c_str())) {
        git_index_free(index);
        const RepoInfo repositoryInfo = info();
        if (repositoryInfo.workDir.empty()) {
            fail("cannot stage a submodule in a bare repository");
        }
        const ProcessResult result = runGit({"add", "--", path}, repositoryInfo.workDir);
        if (result.exitCode != 0) {
            fail("cannot stage submodule '" + path + "': " +
                 (result.err.empty() ? result.out : result.err));
        }
        return;
    }

    const std::filesystem::path full = workDirOf(m_repo) / path;
    if (std::filesystem::exists(full) && pathUsesExternalFilter(m_repo, path)) {
        git_index_free(index);
        stageWithGit(m_repo, {path});
        return;
    }
    const int rc = std::filesystem::exists(full) ? git_index_add_bypath(index, path.c_str())
                                                 : git_index_remove_bypath(index, path.c_str());
    // A path that is neither on disk nor in the index was never tracked: there is
    // nothing to stage, and reporting a failure for it would be a lie.
    if (rc == GIT_ENOTFOUND) {
        git_index_free(index);
        return;
    }
    const int written = rc == 0 ? git_index_write(index) : rc;
    git_index_free(index);
    check(written, "cannot stage " + path);
}

void Repository::unstageFile(const std::string& path) {
    // `git reset -- <path>`: the index entry goes back to HEAD's version, and for
    // a path HEAD does not have (a newly added file) it leaves the index. The
    // target is HEAD's commit; libgit2 takes a null target to mean "remove from
    // the index", not "HEAD", which is the opposite of what unstage has to do.
    git_object* target = nullptr; // the HEAD commit, owned; freed as a git_object
    git_reference* head = nullptr;
    const int headRc = git_repository_head(&head, m_repo);
    if (headRc == 0) {
        git_commit* headCommit = nullptr;
        const int peelRc = git_reference_peel(reinterpret_cast<git_object**>(&headCommit), head,
                                              GIT_OBJECT_COMMIT);
        git_reference_free(head);
        if (peelRc != 0) {
            check(peelRc, "cannot resolve HEAD's commit");
        }
        target = reinterpret_cast<git_object*>(headCommit);
    } else if (headRc != GIT_EUNBORNBRANCH) {
        check(headRc, "cannot read HEAD");
    }

    char* paths[1] = {const_cast<char*>(path.c_str())};
    git_strarray pathspec = {paths, 1};
    const int rc = git_reset_default(m_repo, target, &pathspec);
    git_object_free(target);
    check(rc, "cannot unstage " + path);
}

void Repository::stageAll() {
    if (externalFilterConfigured(m_repo, {})) {
        stageWithGit(m_repo, {});
        return;
    }
    git_index* index = nullptr;
    check(git_repository_index(&index, m_repo), "cannot read the index");
    git_index_read(index, 1);

    git_strarray pathspec = {nullptr, 0};
    // add_all takes new and modified files (and untracked ones); update_all takes
    // deletions of tracked files, which are not files any more and so are not
    // reached by a scan of the working directory.
    int rc = git_index_add_all(index, &pathspec, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr);
    if (rc == 0) {
        rc = git_index_update_all(index, &pathspec, nullptr, nullptr);
    }
    const int written = rc == 0 ? git_index_write(index) : rc;
    git_index_free(index);
    check(written, "cannot stage all changes");
}

void Repository::discardAllChanges() {
    if (detectOperation(gitDirOf(m_repo).string()) != RepoOperation::None || squashInProgress()) {
        fail("an operation is in progress: finish or abort it before discarding everything");
    }
    const std::string workDir = workDirOf(m_repo).string();
    if (workDir.empty()) {
        fail("a bare repository has no changes to discard");
    }
    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"reset", "--hard", "-q", "HEAD"},
          std::vector<std::string>{"clean", "-fd", "-q"}}) {
        const ProcessResult run = runGit(args, workDir);
        if (run.exitCode != 0) {
            fail("cannot discard the changes: " + failureText(run));
        }
    }
}

void Repository::discardFileChanges(const std::string& path) {
    git_index* index = nullptr;
    check(git_repository_index(&index, m_repo), "cannot read the index");
    git_index_read(index, 1);

    const git_index_entry* entry = git_index_get_bypath(index, path.c_str(), 0);
    if (entry && entry->mode == GIT_FILEMODE_COMMIT) {
        // A submodule's checkout is restored by `git submodule update`, not by
        // checking the index out: the pointer lives in the parent's index, but the
        // commit inside the submodule is a checkout of its own.
        git_index_free(index);
        const RepoInfo repositoryInfo = info();
        if (repositoryInfo.workDir.empty()) {
            fail("cannot discard a submodule in a bare repository");
        }
        const ProcessResult result =
            runGit({"submodule", "update", "--", path}, repositoryInfo.workDir);
        if (result.exitCode != 0) {
            fail("cannot restore submodule '" + path + "': " +
                 (result.err.empty() ? result.out : result.err));
        }
        return;
    }

    if (!entry) {
        // Nothing in the index to restore from: this is an untracked file, and
        // discarding it can only mean deleting it.
        git_index_free(index);
        std::error_code ec;
        std::filesystem::remove(workDirOf(m_repo) / path, ec);
        return;
    }

    if (pathUsesExternalFilter(m_repo, path)) {
        git_index_free(index);
        const ProcessResult run = runGit({"checkout", "--", path}, workDirOf(m_repo).string());
        if (run.exitCode != 0) {
            fail("cannot discard changes to " + path + ": " + failureText(run));
        }
        return;
    }
    git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
    opts.checkout_strategy = GIT_CHECKOUT_FORCE;
    char* paths[1] = {const_cast<char*>(path.c_str())};
    opts.paths.strings = paths;
    opts.paths.count = 1;
    const int rc = git_checkout_index(m_repo, index, &opts);
    git_index_free(index);
    check(rc, "cannot discard changes to " + path);
}

Oid Repository::createCommit(const std::string& message) {
    return createCommit(message, false);
}

Oid Repository::createCommit(const std::string& message, bool sign) {
    if (message.empty()) {
        fail("a commit needs a message");
    }

    // Only the git binary knows the reader's gpg/ssh setup and runs the commit hooks.
    if (sign || commitHooksEnabled(workDirOf(m_repo).string())) {
        commitWithGit(m_repo, message, sign, GitCommitKind::New);
        return resolveRevision("HEAD");
    }

    git_index* index = nullptr;
    check(git_repository_index(&index, m_repo), "cannot read the index");
    git_index_read(index, 1);
    git_oid treeId;
    check(git_index_write_tree(&treeId, index), "cannot write the index tree");
    git_tree* tree = nullptr;
    check(git_tree_lookup(&tree, m_repo, &treeId), "cannot look up the committed tree");

    // A fresh repository has an unborn HEAD: the first commit has no parent, and
    // asking git_repository_head for one is not an error there, it is the answer.
    git_reference* head = nullptr;
    git_commit* parent = nullptr;
    const int headRc = git_repository_head(&head, m_repo);
    if (headRc == 0) {
        const int peelRc =
            git_reference_peel(reinterpret_cast<git_object**>(&parent), head, GIT_OBJECT_COMMIT);
        git_reference_free(head);
        if (peelRc != 0) {
            git_tree_free(tree);
            git_index_free(index);
            check(peelRc, "cannot resolve the HEAD commit");
        }
    } else if (headRc != GIT_EUNBORNBRANCH) {
        git_tree_free(tree);
        git_index_free(index);
        check(headRc, "cannot read HEAD");
    }

    git_signature* signature = defaultSignature(m_repo);
    git_oid newId;
    int rc;
    if (parent) {
        const git_commit* parents[1] = {parent};
        rc = git_commit_create(&newId, m_repo, "HEAD", signature, signature, "UTF-8",
                               message.c_str(), tree, 1, parents);
    } else {
        rc = git_commit_create(&newId, m_repo, "HEAD", signature, signature, "UTF-8",
                               message.c_str(), tree, 0, nullptr);
    }
    git_signature_free(signature);
    git_commit_free(parent);
    git_tree_free(tree);
    git_index_free(index);
    check(rc, "cannot create the commit");
    return oidToString(&newId);
}

void Repository::addIgnorePattern(const std::string& pattern, bool globalExclude) {
    const RepoInfo repositoryInfo = info();
    std::filesystem::path file;
    if (globalExclude) {
        if (repositoryInfo.commonDir.empty()) {
            fail("cannot find the git directory for the exclude file");
        }
        file = std::filesystem::path(repositoryInfo.commonDir) / "info" / "exclude";
    } else {
        if (repositoryInfo.workDir.empty()) {
            fail("cannot ignore a path in a bare repository");
        }
        file = std::filesystem::path(repositoryInfo.workDir) / ".gitignore";
    }

    std::string existing;
    if (std::filesystem::exists(file)) {
        std::ifstream in(file, std::ios::binary);
        existing.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // A pattern that is already there needs no second line: the file would grow
    // every time the reader asked for something they already have.
    std::istringstream lines(existing);
    std::string line;
    while (std::getline(lines, line)) {
        if (line == pattern) {
            return;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) {
        fail("cannot write " + file.string());
    }
    if (!existing.empty() && existing.back() != '\n') {
        out << '\n';
    }
    out << pattern << '\n';
    if (!out) {
        fail("cannot write " + file.string());
    }
}

namespace {

std::filesystem::path localExcludeFile(const RepoInfo& repositoryInfo) {
    if (repositoryInfo.commonDir.empty()) {
        fail("cannot find the git directory for the exclude file");
    }
    return std::filesystem::path(repositoryInfo.commonDir) / "info" / "exclude";
}

std::string readWhole(const std::filesystem::path& file) {
    std::string text;
    if (std::filesystem::exists(file)) {
        std::ifstream in(file, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return text;
}

} // namespace

std::vector<std::string> Repository::localExcludePatterns() const {
    std::vector<std::string> out;
    std::istringstream lines(readWhole(localExcludeFile(info())));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && line.front() != '#') {
            out.push_back(line);
        }
    }
    return out;
}

void Repository::removeLocalExcludePattern(const std::string& pattern) {
    const std::filesystem::path file = localExcludeFile(info());
    std::istringstream lines(readWhole(file));
    std::string kept;
    std::string line;
    while (std::getline(lines, line)) {
        std::string bare = line;
        if (!bare.empty() && bare.back() == '\r') {
            bare.pop_back();
        }
        if (bare != pattern) {
            kept += line + '\n';
        }
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << kept;
    if (!out) {
        fail("cannot write " + file.string());
    }
}

std::vector<std::string> Repository::skipWorktreePaths() const {
    std::vector<std::string> out;
    const std::string workDir = info().workDir;
    if (workDir.empty()) {
        return out;
    }
    const ProcessResult run = runGit({"ls-files", "-v", "-z"}, workDir);
    if (run.exitCode != 0) {
        fail("cannot list the index: " + failureText(run));
    }
    std::size_t start = 0;
    while (start < run.out.size()) {
        std::size_t end = run.out.find('\0', start);
        if (end == std::string::npos) {
            end = run.out.size();
        }
        if (end - start > 2 && run.out[start] == 'S' && run.out[start + 1] == ' ') {
            out.emplace_back(run.out, start + 2, end - start - 2);
        }
        start = end + 1;
    }
    return out;
}

void Repository::setSkipWorktree(const std::vector<std::string>& paths, bool skip) {
    const std::string workDir = info().workDir;
    if (workDir.empty() || paths.empty()) {
        return;
    }
    std::vector<std::string> args{"update-index", skip ? "--skip-worktree" : "--no-skip-worktree",
                                  "--"};
    args.insert(args.end(), paths.begin(), paths.end());
    const ProcessResult run = runGit(args, workDir);
    if (run.exitCode != 0) {
        fail(failureText(run));
    }
}

void Repository::untrackPathspec(const std::string& pathspec) {
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        fail("cannot untrack a path in a bare repository");
    }
    // Nothing tracked matches: ignoring an untracked file is the ordinary case, and
    // `git rm` would call it a fatal pathspec error. Ask `ls-files` instead of
    // reading the failure message, which is localised.
    const ProcessResult listed = runGit({"ls-files", "--", pathspec}, repositoryInfo.workDir);
    if (listed.exitCode != 0 || listed.out.empty()) {
        return;
    }
    const ProcessResult process =
        runGit({"rm", "-r", "--cached", "--quiet", "--", pathspec}, repositoryInfo.workDir);
    if (process.exitCode != 0) {
        fail("cannot untrack '" + pathspec + "': " +
             (process.err.empty() ? process.out : process.err));
    }
}

void Repository::removeFile(const std::string& path) {
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        fail("cannot remove a file in a bare repository");
    }
    const ProcessResult process =
        runGit({"rm", "-f", "--quiet", "--", path}, repositoryInfo.workDir);
    if (process.exitCode != 0) {
        fail("cannot remove '" + path + "': " +
             (process.err.empty() ? process.out : process.err));
    }
}

std::string Repository::addCoAuthors(const std::string& message,
                                     const std::vector<CoAuthor>& authors) const {
    if (authors.empty()) {
        return message;
    }
    const RepoInfo repositoryInfo = info();
    if (repositoryInfo.workDir.empty()) {
        fail("cannot build a co-author trailer in a bare repository");
    }

    // `interpret-trailers` reads and writes a file, so the message goes through a
    // temporary one instead of a shell pipe: a name with a quote or a newline in it
    // is then still just an argument.
    const std::filesystem::path file =
        std::filesystem::path(repositoryInfo.gitDir) / "gitmycelium-trailers.txt";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            fail("cannot write a temporary message file");
        }
        out << message;
    }

    std::vector<std::string> args{"interpret-trailers", "--in-place", "--if-exists",
                                  "addIfDifferent"};
    for (const CoAuthor& author : authors) {
        args.push_back("--trailer");
        args.push_back("Co-authored-by: " + author.name + " <" + author.email + ">");
    }
    args.push_back(file.string());
    const ProcessResult process = runGit(args, repositoryInfo.workDir);
    if (process.exitCode != 0) {
        std::filesystem::remove(file);
        fail("cannot add the co-author trailer: " +
             (process.err.empty() ? process.out : process.err));
    }

    std::string result;
    {
        std::ifstream in(file, std::ios::binary);
        result.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::filesystem::remove(file);
    return result;
}

std::string Repository::commitTemplate() const {
    const RepoInfo repositoryInfo = info();
    // `--path` expands `~/`; a value that is still relative is resolved against the
    // work tree, because git reads the template relative to where the commit runs.
    const ProcessResult process =
        runGit({"config", "--path", "--get", "commit.template"}, repositoryInfo.workDir);
    if (process.exitCode != 0) {
        return {};
    }
    std::string path = process.out;
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) {
        path.pop_back();
    }
    if (path.empty()) {
        return {};
    }
    std::filesystem::path full(path);
    if (full.is_relative() && !repositoryInfo.workDir.empty()) {
        full = std::filesystem::path(repositoryInfo.workDir) / full;
    }
    std::ifstream in(full, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<CoAuthor> Repository::recentAuthors(std::size_t limit) const {
    if (limit == 0) {
        return {};
    }
    const RepoInfo repositoryInfo = info();
    const ProcessResult process = runGit(
        {"log", "-n", std::to_string(limit), "--format=%an%x1f%ae"}, repositoryInfo.workDir);
    if (process.exitCode != 0) {
        return {};
    }

    std::vector<CoAuthor> out;
    std::unordered_map<std::string, bool> seen;
    std::istringstream lines(process.out);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t sep = line.find('\x1f');
        if (sep == std::string::npos) {
            continue;
        }
        CoAuthor author;
        author.name = line.substr(0, sep);
        author.email = line.substr(sep + 1);
        if (author.name.empty() && author.email.empty()) {
            continue;
        }
        const std::string key = author.email.empty() ? author.name : author.email;
        if (!seen.emplace(key, true).second) {
            continue;
        }
        out.push_back(author);
    }
    return out;
}

namespace {

std::vector<ChangedFile> changedFilesFromDiff(git_diff* diff) {
    std::vector<ChangedFile> out;
    const std::size_t count = git_diff_num_deltas(diff);
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const git_diff_delta* delta = git_diff_get_delta(diff, i);
        if (!delta) {
            continue;
        }
        ChangedFile file;
        file.path = delta->new_file.path ? delta->new_file.path
                                         : (delta->old_file.path ? delta->old_file.path : "");
        file.oldPath = delta->old_file.path ? delta->old_file.path : file.path;
        file.kind = mapDelta(delta->status);
        out.push_back(std::move(file));
    }
    return out;
}

FileDiff fileDiffFromDiff(git_diff* diff, const std::string& path) {
    FileDiff result;
    const std::size_t count = git_diff_num_deltas(diff);
    if (count == 0) {
        return result;
    }

    std::size_t index = 0;
    const git_diff_delta* delta = git_diff_get_delta(diff, 0);
    for (std::size_t i = 0; i < count; ++i) {
        const git_diff_delta* candidate = git_diff_get_delta(diff, i);
        const char* candidatePath =
            candidate->new_file.path ? candidate->new_file.path : candidate->old_file.path;
        if (candidatePath && path == candidatePath) {
            delta = candidate;
            index = i;
            break;
        }
    }

    result.oldPath = delta->old_file.path ? delta->old_file.path : "";
    result.newPath = delta->new_file.path ? delta->new_file.path : "";
    result.kind = mapDelta(delta->status);
    result.oldMode = delta->old_file.mode;
    result.newMode = delta->new_file.mode;
    // The binary flag is set while the patch is generated, so it must be read after
    // fillFileDiff() has built one — reading it off the raw delta reports every blob
    // as text.
    fillFileDiff(diff, index, result);
    result.binary = (delta->flags & GIT_DIFF_FLAG_BINARY) != 0;
    return result;
}

git_tree* treeAtRevision(git_repository* repo, const Oid& revision) {
    if (revision.empty()) {
        return nullptr;
    }
    const git_oid oid = resolveRevisionToCommit(repo, revision);
    git_commit* commit = nullptr;
    check(git_commit_lookup(&commit, repo, &oid), "cannot look up " + revision);
    git_tree* tree = nullptr;
    check(git_commit_tree(&tree, commit), "cannot read the tree of " + revision);
    git_commit_free(commit);
    return tree;
}

} // namespace

std::vector<ChangedFile> Repository::changedFiles(const Oid& commitId) const {
    if (m_partialClone) {
        return parseNameStatus(nameStatusCli(firstParentOf(commitId), commitId));
    }
    CommitDiff cd;
    buildCommitDiff(m_repo, commitId, {}, cd);
    std::vector<ChangedFile> out = changedFilesFromDiff(cd.diff);
    if (git_tree* untracked = stashUntrackedTree(m_repo, cd.commit)) {
        git_diff* added = nullptr;
        diffTrees(m_repo, nullptr, untracked, {}, &added);
        std::vector<ChangedFile> more = changedFilesFromDiff(added);
        out.insert(out.end(), more.begin(), more.end());
        git_diff_free(added);
        git_tree_free(untracked);
    }
    return out;
}

FileDiff Repository::commitFileDiff(const Oid& commitId, const std::string& path) const {
    if (m_partialClone) {
        // The parent comes from libgit2 (commit objects are present even in a promisor
        // clone); the trees and, above all, the blobs come from the binary.
        const std::vector<FileDiff> parsed =
            parseUnifiedDiff(diffCliText(firstParentOf(commitId), commitId, path));
        return parsed.empty() ? FileDiff{} : parsed.front();
    }
    CommitDiff cd;
    buildCommitDiff(m_repo, commitId, {path}, cd);
    if (git_diff_num_deltas(cd.diff) == 0) {
        if (git_tree* untracked = stashUntrackedTree(m_repo, cd.commit)) {
            git_diff* added = nullptr;
            diffTrees(m_repo, nullptr, untracked, {path}, &added);
            FileDiff result = fileDiffFromDiff(added, path);
            git_diff_free(added);
            git_tree_free(untracked);
            return result;
        }
    }
    return fileDiffFromDiff(cd.diff, path);
}

std::vector<ChangedFile> Repository::changedFilesBetween(const Oid& base,
                                                         const Oid& target) const {
    if (m_partialClone) {
        return parseNameStatus(nameStatusCli(base, target));
    }
    git_tree* baseTree = treeAtRevision(m_repo, base);
    git_tree* targetTree = treeAtRevision(m_repo, target);
    git_diff* diff = nullptr;
    diffTrees(m_repo, baseTree, targetTree, {}, &diff);
    std::vector<ChangedFile> out = changedFilesFromDiff(diff);
    git_diff_free(diff);
    git_tree_free(baseTree);
    git_tree_free(targetTree);
    return out;
}

FileDiff Repository::fileDiffBetween(const Oid& base, const Oid& target,
                                     const std::string& path) const {
    if (m_partialClone) {
        const std::vector<FileDiff> parsed = parseUnifiedDiff(diffCliText(base, target, path));
        return parsed.empty() ? FileDiff{} : parsed.front();
    }
    git_tree* baseTree = treeAtRevision(m_repo, base);
    git_tree* targetTree = treeAtRevision(m_repo, target);
    git_diff* diff = nullptr;
    diffTrees(m_repo, baseTree, targetTree, {path}, &diff);
    FileDiff result = fileDiffFromDiff(diff, path);
    git_diff_free(diff);
    git_tree_free(baseTree);
    git_tree_free(targetTree);
    return result;
}

FileDiff Repository::stagedFileDiff(const std::string& path) const {
    FileDiff result;
    result.newPath = path;
    if (m_partialClone) {
        const std::vector<FileDiff> parsed = parseUnifiedDiff(indexDiffCli(true, path));
        return parsed.empty() ? result : parsed.front();
    }
    // HEAD's tree against the index — what is staged. An unborn HEAD has no tree;
    // every staged path is then an addition, which is what a null old tree says.

    git_tree* headTree = nullptr;
    git_revparse_single(reinterpret_cast<git_object**>(&headTree), m_repo, "HEAD^{tree}");

    git_diff* diff = nullptr;
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    opts.flags = GIT_DIFF_INCLUDE_TYPECHANGE;
    char* spec = const_cast<char*>(path.c_str());
    opts.pathspec.strings = &spec;
    opts.pathspec.count = 1;

    const int rc = git_diff_tree_to_index(&diff, m_repo, headTree, nullptr, &opts);
    git_tree_free(headTree);
    check(rc, "cannot diff the index");
    const std::size_t count = git_diff_num_deltas(diff);
    if (count == 0) {
        git_diff_free(diff);
        return result;
    }

    const git_diff_delta* delta = git_diff_get_delta(diff, 0);
    result.oldPath = delta->old_file.path ? delta->old_file.path : path;
    result.newPath = delta->new_file.path ? delta->new_file.path : path;
    result.binary = (delta->flags & GIT_DIFF_FLAG_BINARY) != 0;
    result.kind = mapDelta(delta->status);
    result.oldMode = delta->old_file.mode;
    result.newMode = delta->new_file.mode;
    fillFileDiff(diff, 0, result);
    git_diff_free(diff);
    return result;
}

FileDiff Repository::workdirFileDiff(const std::string& path) const {
    FileDiff result;
    result.newPath = path;

    git_diff* diff = nullptr;
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    // SHOW_UNTRACKED_CONTENT is what makes an untracked file's lines appear: with
    // INCLUDE_UNTRACKED alone the delta is there but carries no hunks.
    opts.flags = GIT_DIFF_INCLUDE_TYPECHANGE | GIT_DIFF_INCLUDE_UNTRACKED |
                 GIT_DIFF_SHOW_UNTRACKED_CONTENT;

    // A pathspec keeps the walk to the one file. libgit2 does not match an untracked
    // file against a pathspec, and without one it walks the whole work tree (a
    // second on 300k files), so an untracked file is diffed from its own bytes.
    unsigned status = 0;
    const bool untracked =
        git_status_file(&status, m_repo, path.c_str()) == 0 && (status & GIT_STATUS_WT_NEW) != 0;
    if (m_partialClone && !untracked) {
        // A tracked file's index side is a blob a promisor clone may not have; an
        // untracked one has no index side, so libgit2 can still read it from disk.
        const std::vector<FileDiff> parsed = parseUnifiedDiff(indexDiffCli(false, path));
        return parsed.empty() ? result : parsed.front();
    }
    if (untracked) {
        return untrackedFileDiff(path);
    }
    char* spec = const_cast<char*>(path.c_str());
    opts.pathspec.strings = &spec;
    opts.pathspec.count = 1;

    check(git_diff_index_to_workdir(&diff, m_repo, nullptr, &opts), "cannot diff working tree");
    const std::size_t count = git_diff_num_deltas(diff);
    if (count == 0) {
        git_diff_free(diff);
        return result;
    }

    std::size_t index = 0;
    const git_diff_delta* delta = nullptr;
    for (std::size_t i = 0; i < count; ++i) {
        const git_diff_delta* candidate = git_diff_get_delta(diff, i);
        const char* candidatePath =
            candidate->new_file.path ? candidate->new_file.path : candidate->old_file.path;
        if (candidatePath && path == candidatePath) {
            delta = candidate;
            index = i;
            break;
        }
    }
    if (!delta) {
        git_diff_free(diff);
        return result;
    }

    result.oldPath = delta->old_file.path ? delta->old_file.path : path;
    result.newPath = delta->new_file.path ? delta->new_file.path : path;
    result.binary = (delta->flags & GIT_DIFF_FLAG_BINARY) != 0;
    result.kind = mapDelta(delta->status);
    result.oldMode = delta->old_file.mode;
    result.newMode = delta->new_file.mode;
    fillFileDiff(diff, index, result);
    git_diff_free(diff);
    return result;
}

FileDiff Repository::untrackedFileDiff(const std::string& path) const {
    FileDiff result;
    result.oldPath = path;
    result.newPath = path;
    result.kind = ChangeKind::Untracked;

    const std::filesystem::path workDir = workDirOf(m_repo);
    if (workDir.empty()) {
        return result;
    }
    const std::filesystem::path full = workDir / path;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(full, error);
    if (error) {
        return result;
    }
    std::string bytes;
    if (std::filesystem::is_symlink(status)) {
        bytes = std::filesystem::read_symlink(full, error).string();
        result.newMode = GIT_FILEMODE_LINK;
    } else {
        std::ifstream in(full, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        const auto perms = status.permissions();
        const bool executable =
            (perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
        result.newMode = executable && trustsFileMode(m_repo)
                             ? GIT_FILEMODE_BLOB_EXECUTABLE
                             : GIT_FILEMODE_BLOB;
    }

    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    git_patch* patch = nullptr;
    check(git_patch_from_buffers(&patch, nullptr, 0, nullptr, bytes.data(), bytes.size(),
                                 path.c_str(), &opts),
          "cannot diff an untracked file");
    result.binary = (git_patch_get_delta(patch)->flags & GIT_DIFF_FLAG_BINARY) != 0;
    fillFromPatch(patch, result);
    return result;
}

std::vector<ChangedFile> Repository::workdirChangedFilesAgainst(const Oid& revision) const {
    if (m_partialClone) {
        return parseNameStatus(workdirNameStatusCli(revision));
    }
    git_tree* tree = treeAtRevision(m_repo, revision);
    git_diff* diff = nullptr;
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    // Tracked changes only: this is `git diff <rev>`, and an untracked file is new to
    // the tree rather than a difference from it.
    opts.flags = GIT_DIFF_INCLUDE_TYPECHANGE;
    check(git_diff_tree_to_workdir(&diff, m_repo, tree, &opts), "cannot diff the working tree");
    std::vector<ChangedFile> out = changedFilesFromDiff(diff);
    git_diff_free(diff);
    git_tree_free(tree);
    return out;
}

FileDiff Repository::workdirFileDiffAgainst(const Oid& revision, const std::string& path) const {
    if (m_partialClone) {
        const std::vector<FileDiff> parsed = parseUnifiedDiff(workdirDiffCli(revision, path));
        return parsed.empty() ? FileDiff{} : parsed.front();
    }
    git_tree* tree = treeAtRevision(m_repo, revision);
    git_diff* diff = nullptr;
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    opts.flags = GIT_DIFF_INCLUDE_TYPECHANGE;
    char* spec = const_cast<char*>(path.c_str());
    opts.pathspec.strings = &spec;
    opts.pathspec.count = 1;
    check(git_diff_tree_to_workdir(&diff, m_repo, tree, &opts), "cannot diff the working tree");
    FileDiff result = fileDiffFromDiff(diff, path);
    git_diff_free(diff);
    git_tree_free(tree);
    return result;
}

std::string Repository::indexFileContent(const std::string& path) const {
    git_index* index = nullptr;
    check(git_repository_index(&index, m_repo), "cannot read the index");
    git_index_read(index, 0);
    const git_index_entry* entry = git_index_get_bypath(index, path.c_str(), 0);
    if (!entry) {
        git_index_free(index);
        return {};
    }
    git_blob* blob = nullptr;
    const int rc = git_blob_lookup(&blob, m_repo, &entry->id);
    git_index_free(index);
    if (rc != 0 || !blob) {
        return {};
    }
    std::string out;
    const void* raw = git_blob_rawcontent(blob);
    const git_object_size_t size = git_blob_rawsize(blob);
    if (raw && size > 0) {
        out.assign(static_cast<const char*>(raw), static_cast<std::size_t>(size));
    }
    git_blob_free(blob);
    return out;
}

void Repository::applyPatchToIndex(const std::string& patch) {
    if (patch.empty()) {
        return;
    }
    // git's own parser reads the patch, so the index application obeys the same
    // rules as `git apply --cached`; writing the bytes ourselves would be a
    // lookalike that diverges on edge cases the tests would have to rediscover.
    git_diff* diff = nullptr;
    check(git_diff_from_buffer(&diff, patch.c_str(), patch.size()),
          "cannot parse the generated patch");
    const int rc = git_apply(m_repo, diff, GIT_APPLY_LOCATION_INDEX, nullptr);
    git_diff_free(diff);
    check(rc, "the change could not be applied to the index");
}

void Repository::applyPatchToWorkdir(const std::string& patch) {
    if (patch.empty()) {
        return;
    }
    git_diff* diff = nullptr;
    check(git_diff_from_buffer(&diff, patch.c_str(), patch.size()),
          "cannot parse the generated patch");
    const int rc = git_apply(m_repo, diff, GIT_APPLY_LOCATION_WORKDIR, nullptr);
    git_diff_free(diff);
    check(rc, "the change could not be applied to the working tree");
}

std::vector<ConflictEntry> Repository::conflicts() const {
    std::vector<ConflictEntry> out;

    git_index* index = nullptr;
    check(git_repository_index(&index, m_repo), "cannot read the index");
    // The git binary writes the index too — a rebase, a merge, a stash — and libgit2
    // keeps the copy it read when the repository was opened. Without this, a conflict
    // created by `git rebase` is invisible: the conflict page finds nothing to resolve
    // and the reader is told an operation stopped while nothing asks for them.
    git_index_read(index, 0);
    if (git_index_has_conflicts(index) == 0) {
        git_index_free(index);
        return out;
    }

    git_index_conflict_iterator* iterator = nullptr;
    check(git_index_conflict_iterator_new(&iterator, index), "cannot iterate over conflicts");

    const git_index_entry* ancestor = nullptr;
    const git_index_entry* ours = nullptr;
    const git_index_entry* theirs = nullptr;
    int rc = 0;
    while ((rc = git_index_conflict_next(&ancestor, &ours, &theirs, iterator)) == 0) {
        const git_index_entry* any = ours ? ours : (theirs ? theirs : ancestor);
        if (!any) {
            continue;
        }
        ConflictEntry entry;
        entry.path = any->path;
        entry.ancestor = oidToString(ancestor ? &ancestor->id : nullptr);
        entry.ours = oidToString(ours ? &ours->id : nullptr);
        entry.theirs = oidToString(theirs ? &theirs->id : nullptr);
        entry.kind = classifyConflict(ancestor != nullptr, ours != nullptr, theirs != nullptr);
        out.push_back(std::move(entry));
    }
    const bool complete = (rc == GIT_ITEROVER);

    git_index_conflict_iterator_free(iterator);
    git_index_free(index);
    if (!complete) {
        fail("cannot read conflict entries");
    }
    return out;
}

std::string Repository::blobText(const Oid& blobId) const {
    if (blobId.empty()) {
        return {};
    }
    const git_oid oid = parseOid(blobId);
    git_blob* blob = nullptr;
    if (git_blob_lookup(&blob, m_repo, &oid) < 0) {
        return {};
    }
    std::string text;
    if (git_blob_is_binary(blob) == 0) {
        const char* raw = static_cast<const char*>(git_blob_rawcontent(blob));
        text.assign(raw, static_cast<std::size_t>(git_blob_rawsize(blob)));
    }
    git_blob_free(blob);
    return text;
}

std::string Repository::fileAt(const Oid& commitId, const std::string& path) const {
    if (m_partialClone) {
        return fileAtCli(commitId, path);
    }
    const git_oid oid = parseOid(commitId);
    git_commit* commit = nullptr;
    if (git_commit_lookup(&commit, m_repo, &oid) < 0) {
        return {};
    }
    git_tree* tree = nullptr;
    if (git_commit_tree(&tree, commit) < 0) {
        git_commit_free(commit);
        return {};
    }

    std::string content;
    git_tree_entry* entry = nullptr;
    if (git_tree_entry_bypath(&entry, tree, path.c_str()) == 0 && entry) {
        if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB) {
            const git_oid* blobId = git_tree_entry_id(entry);
            if (blobId) {
                content = blobText(oidToString(blobId));
            }
        }
        git_tree_entry_free(entry);
    }

    git_tree_free(tree);
    git_commit_free(commit);
    return content;
}

std::string Repository::fileAtCli(const Oid& commitId, const std::string& path) const {
    const std::string workDir = workDirOf(m_repo).string();
    if (workDir.empty() || commitId.empty() || path.empty()) {
        return {};
    }
    // `<rev>:<path>` reads the blob as it is at that revision, with no filters or
    // textconv, and fetches it from the promisor remote if it was filtered out.
    const std::string spec = commitId + ":" + path;

    const ProcessResult blob = runGit({"cat-file", "blob", spec}, workDir);
    if (blob.exitCode != 0) {
        return {}; // no such path at that revision, or it is a tree
    }
    // Binary is returned as no text, the answer libgit2 gives: git's own test, a NUL
    // in the first 8000 bytes.
    const std::size_t probe = std::min<std::size_t>(blob.out.size(), 8000);
    if (blob.out.find('\0') < probe) {
        return {};
    }
    return blob.out;
}

std::string Repository::workdirFileContent(const std::string& path) const {
    const std::filesystem::path workDir = workDirOf(m_repo);
    if (workDir.empty()) {
        return {};
    }
    std::ifstream in(workDir / path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string Repository::conflictMarkedContent(const std::string& path, bool* textAvailable) const {
    if (textAvailable) {
        *textAvailable = false;
    }

    git_index* index = nullptr;
    check(git_repository_index(&index, m_repo), "cannot read the index");
    git_index_read(index, 0);

    bool anySideBinary = false;
    const auto stageText = [&](int stage, std::string* bytes) {
        const git_index_entry* entry = git_index_get_bypath(index, path.c_str(), stage);
        if (entry == nullptr) {
            return false;
        }
        git_blob* blob = nullptr;
        if (git_blob_lookup(&blob, m_repo, &entry->id) != 0) {
            return false;
        }
        const bool binary = git_blob_is_binary(blob) != 0;
        anySideBinary = anySideBinary || binary;
        if (!binary) {
            const void* raw = git_blob_rawcontent(blob);
            bytes->assign(static_cast<const char*>(raw),
                          static_cast<std::size_t>(git_blob_rawsize(blob)));
        }
        git_blob_free(blob);
        return !binary;
    };

    std::string ancestorText;
    std::string oursText;
    std::string theirsText;
    stageText(GIT_INDEX_STAGE_ANCESTOR, &ancestorText);
    const bool oursIsText = stageText(GIT_INDEX_STAGE_OURS, &oursText);
    const bool theirsIsText = stageText(GIT_INDEX_STAGE_THEIRS, &theirsText);
    git_index_free(index);

    // One binary side is enough: shown as text, its side would read as empty, and
    // saving the editor's answer would write text over the binary.
    if ((!oursIsText && !theirsIsText) || anySideBinary) {
        return {};
    }

    git_merge_file_input ancestorInput = GIT_MERGE_FILE_INPUT_INIT;
    ancestorInput.path = path.c_str();
    ancestorInput.mode = 0100644;
    ancestorInput.ptr = ancestorText.data();
    ancestorInput.size = ancestorText.size();

    git_merge_file_input oursInput = GIT_MERGE_FILE_INPUT_INIT;
    oursInput.path = path.c_str();
    oursInput.mode = 0100644;
    oursInput.ptr = oursText.data();
    oursInput.size = oursText.size();

    git_merge_file_input theirsInput = GIT_MERGE_FILE_INPUT_INIT;
    theirsInput.path = path.c_str();
    theirsInput.mode = 0100644;
    theirsInput.ptr = theirsText.data();
    theirsInput.size = theirsText.size();

    git_merge_file_options options = GIT_MERGE_FILE_OPTIONS_INIT;
    options.flags = GIT_MERGE_FILE_STYLE_DIFF3;
    // A file whose own lines start with a marker-length run — a heading underlined
    // with `=======` — gets longer markers, so the parser cannot take its text for one.
    options.marker_size = static_cast<unsigned short>(
        safeConflictMarkerSize({ancestorText, oursText, theirsText}));

    git_merge_file_result merged{};
    const int rc = git_merge_file(&merged, &ancestorInput, &oursInput, &theirsInput, &options);

    std::string out;
    if (rc == 0 && merged.ptr != nullptr) {
        out.assign(merged.ptr, merged.len);
        if (textAvailable) {
            *textAvailable = true;
        }
    }
    git_merge_file_result_free(&merged);
    return out;
}

long long Repository::blobSize(const Oid& blobId) const {
    if (blobId.empty()) {
        return -1;
    }
    const git_oid oid = parseOid(blobId);
    git_blob* blob = nullptr;
    if (git_blob_lookup(&blob, m_repo, &oid) < 0) {
        return -1;
    }
    const long long size = static_cast<long long>(git_blob_rawsize(blob));
    git_blob_free(blob);
    return size;
}

std::string Repository::conflictStageBytes(const std::string& path, ConflictSide side,
                                           bool* present) const {
    if (present) {
        *present = false;
    }
    git_index* index = nullptr;
    check(git_repository_index(&index, m_repo), "cannot read the index");
    git_index_read(index, 0);

    const int stage = side == ConflictSide::Ours     ? GIT_INDEX_STAGE_OURS
                      : side == ConflictSide::Base   ? GIT_INDEX_STAGE_ANCESTOR
                                                     : GIT_INDEX_STAGE_THEIRS;
    const git_index_entry* entry = git_index_get_bypath(index, path.c_str(), stage);
    std::string bytes;
    if (entry != nullptr) {
        git_blob* blob = nullptr;
        if (git_blob_lookup(&blob, m_repo, &entry->id) == 0) {
            const void* raw = git_blob_rawcontent(blob);
            const std::size_t size = static_cast<std::size_t>(git_blob_rawsize(blob));
            if (raw != nullptr) {
                bytes.assign(static_cast<const char*>(raw), size);
            }
            if (present) {
                *present = true;
            }
            git_blob_free(blob);
        }
    }
    git_index_free(index);
    return bytes;
}

void Repository::keepConflictSide(const std::string& path, ConflictSide side) {
    bool present = false;
    const std::string bytes = conflictStageBytes(path, side, &present);
    if (!present) {
        fail("that side does not have the file");
    }
    resolveConflict(path, bytes);
}

void Repository::writeWorkdirFile(const std::string& path, const std::string& content) {
    const std::filesystem::path workDir = workDirOf(m_repo);
    if (workDir.empty()) {
        fail("cannot write a file in a bare repository");
    }

    const std::filesystem::path full = workDir / path;
    std::error_code ec;
    if (full.has_parent_path()) {
        std::filesystem::create_directories(full.parent_path(), ec);
    }
    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    if (!out) {
        fail("cannot write " + full.string());
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    if (!out) {
        fail("cannot write " + full.string());
    }
}

void Repository::resolveConflict(const std::string& path, const std::string& content) {
    writeWorkdirFile(path, content);
    if (pathUsesExternalFilter(m_repo, path)) {
        stageWithGit(m_repo, {path});
        return;
    }

    git_index* index = nullptr;
    check(git_repository_index(&index, m_repo), "cannot read the index");
    git_index_read(index, 1);
    // Adding over a conflicted path is what resolves it, and it moves the three
    // stages into the resolve-undo record on the way, as `git add` does. Removing
    // the stages first threw that record away, and with it the reader's way back
    // to the conflict ("Mark as conflicted").
    const int added = git_index_add_bypath(index, path.c_str());
    const int written = added == 0 ? git_index_write(index) : added;
    git_index_free(index);
    check(written, "cannot stage " + path);
}

std::vector<std::string> Repository::resolvedConflictPaths() const {
    git_index* index = nullptr;
    check(git_repository_index(&index, m_repo), "cannot read the index");
    git_index_read(index, 0);
    std::vector<std::string> out;
    const std::size_t count = git_index_reuc_entrycount(index);
    for (std::size_t i = 0; i < count; ++i) {
        const git_index_reuc_entry* entry = git_index_reuc_get_byindex(index, i);
        if (!entry || !entry->path) {
            continue;
        }
        const git_index_entry* stage = nullptr;
        for (int s = 1; s <= 3 && !stage; ++s) {
            stage = git_index_get_bypath(index, entry->path, s);
        }
        if (!stage) {
            out.emplace_back(entry->path);
        }
    }
    git_index_free(index);
    std::sort(out.begin(), out.end());
    return out;
}

void Repository::markConflicted(const std::string& path) {
    const std::vector<std::string> resolved = resolvedConflictPaths();
    if (std::find(resolved.begin(), resolved.end(), path) == resolved.end()) {
        fail("there is no resolved conflict to reopen at " + path);
    }
    const std::string workDir = workDirOf(m_repo).string();
    const ProcessResult run = runGit({"update-index", "--unresolve", "--", path}, workDir);
    if (run.exitCode != 0) {
        fail("cannot mark " + path + " as conflicted: " + failureText(run));
    }
}

void Repository::createBranch(const std::string& name, const Oid& target) {
    // A tag, a branch name or an id: whatever the caller has to hand. Creating from
    // a tag is ordinary work, and making the reader look up the id first is a
    // usability bug, not a safety measure.
    const git_oid oid = resolveRevisionToCommit(m_repo, target);
    git_commit* commit = nullptr;
    check(git_commit_lookup(&commit, m_repo, &oid), "cannot look up commit " + target);

    // Branches live in a tree of refs, so 'feature' and 'feature/x' cannot both
    // exist. libgit2 reports that as a filesystem failure, which tells the reader
    // nothing; say what actually happened.
    git_reference* existing = nullptr;
    if (git_branch_lookup(&existing, m_repo, name.c_str(), GIT_BRANCH_LOCAL) == 0) {
        git_reference_free(existing);
        git_commit_free(commit);
        fail("a branch called '" + name + "' already exists");
    }
    git_reference_iterator* iterator = nullptr;
    if (git_reference_iterator_new(&iterator, m_repo) == 0) {
        const std::string prefix = name + "/";
        const char* refName = nullptr;
        while (git_reference_next_name(&refName, iterator) == 0) {
            const std::string full = refName;
            if (full.rfind(prefix, 0) == 0) {
                git_reference_iterator_free(iterator);
                git_commit_free(commit);
                fail("branches already exist under '" + name + "', so it cannot itself be "
                     "a branch");
            }
            const std::size_t slash = name.rfind('/');
            if (slash != std::string::npos) {
                const std::string parent = "refs/heads/" + name.substr(0, slash);
                if (full == parent) {
                    git_reference_iterator_free(iterator);
                    git_commit_free(commit);
                    fail("there is already a branch called '" + name.substr(0, slash) +
                         "', so '" + name + "' cannot exist");
                }
            }
        }
        git_reference_iterator_free(iterator);
    }

    git_reference* branch = nullptr;
    const int rc = git_branch_create(&branch, m_repo, name.c_str(), commit, 0);
    git_commit_free(commit);
    check(rc, "cannot create branch '" + name + "'");

    git_reference_free(branch);
}

void Repository::createTrackingBranch(const std::string& name, const std::string& remote) {
    git_reference* remoteRef = nullptr;
    check(git_branch_lookup(&remoteRef, m_repo, remote.c_str(), GIT_BRANCH_REMOTE),
          "no such remote-tracking branch: " + remote);
    const git_oid* tip = git_reference_target(remoteRef);
    check(tip != nullptr ? 0 : -1, "cannot resolve " + remote);
    const std::string tipId = oidToString(tip);

    git_oid oid = parseOid(tipId);
    git_commit* commit = nullptr;
    check(git_commit_lookup(&commit, m_repo, &oid), "cannot look up commit " + tipId);

    git_reference* branch = nullptr;
    const int rc = git_branch_create(&branch, m_repo, name.c_str(), commit, 0);
    git_commit_free(commit);
    check(rc, "cannot create branch '" + name + "'");

    const int upstream = git_branch_set_upstream(branch, remote.c_str());
    if (upstream != 0) {
        // All or nothing. A local branch that looks like it follows the remote and
        // does not is worse than no branch: the first push goes somewhere nobody
        // intended, and nothing said so.
        git_branch_delete(branch);
        git_reference_free(branch);
        git_reference_free(remoteRef);
        fail("cannot make '" + name + "' follow " + remote +
             "': the remote is not configured");
    }
    git_reference_free(branch);
    git_reference_free(remoteRef);
}

void Repository::setUpstream(const std::string& branch, const std::string& upstream) {
    git_reference* local = nullptr;
    check(git_branch_lookup(&local, m_repo, branch.c_str(), GIT_BRANCH_LOCAL),
          "no such local branch: " + branch);

    const int rc = git_branch_set_upstream(local, upstream.c_str());
    git_reference_free(local);
    if (rc == GIT_ENOTFOUND) {
        // git says only "the upstream branch does not exist", which leaves the
        // reader without the one thing they need: what to do about it.
        throw GitError("no branch '" + upstream +
                       "' on the remote; fetch it first, or push the branch and let git set it");
    }
    if (rc != 0) {
        fail("cannot make '" + branch + "' follow " + upstream);
    }
}

void Repository::checkoutBranch(const std::string& name) {
    // git refuses to switch branches with a rebase or merge open, and that refusal has
    // to live here now: the operation runs in the git binary, so libgit2 no longer
    // knows about it and would happily move the branch out from under it.
    if (detectOperation(gitDirOf(m_repo).string()) != RepoOperation::None || squashInProgress()) {
        fail("cannot switch branches while a rebase, merge, cherry-pick, revert or squash is "
             "in progress");
    }

    git_reference* branch = nullptr;
    check(git_branch_lookup(&branch, m_repo, name.c_str(), GIT_BRANCH_LOCAL),
          "no such branch: " + name);
    git_reference_free(branch);
    // The binary, not git_checkout_tree: on 300k files libgit2 took 1.6 s where `git
    // switch` took 0.35 s, and it also runs the filters and post-checkout hook.
    switchWithGit(m_repo, {name});
}

bool Repository::branchMergedIntoHead(const std::string& name) const {
    return branchMergedInto(name, std::string{});
}

bool Repository::branchMergedInto(const std::string& name, const std::string& base) const {
    git_reference* ref = nullptr;
    check(git_branch_lookup(&ref, m_repo, name.c_str(), GIT_BRANCH_LOCAL),
          "no such branch: " + name);
    git_commit* tip = nullptr;
    const int peelRc =
        git_reference_peel(reinterpret_cast<git_object**>(&tip), ref, GIT_OBJECT_COMMIT);
    git_reference_free(ref);
    check(peelRc, "cannot resolve branch tip: " + name);

    // The base resolved as a commit. An empty base is HEAD, and a name may be a
    // branch, a tag or a raw id, so it goes through the parser git itself uses.
    const std::string baseRev = base.empty() ? std::string("HEAD") : base;
    git_object* baseObject = nullptr;
    if (git_revparse_single(&baseObject, m_repo, baseRev.c_str()) != 0) {
        git_commit_free(tip);
        return true; // nothing at the base yet, so nothing can be lost
    }
    const git_oid baseOid = *git_object_id(baseObject);
    git_object_free(baseObject);

    bool merged = git_oid_equal(git_commit_id(tip), &baseOid);
    if (!merged) {
        git_oid mergeBase;
        if (git_merge_base(&mergeBase, m_repo, git_commit_id(tip), &baseOid) == 0) {
            merged = git_oid_equal(&mergeBase, git_commit_id(tip));
        }
    }
    git_commit_free(tip);
    return merged;
}

void Repository::deleteBranch(const std::string& name, bool force, const std::string& base) {
    git_reference* head = nullptr;
    const bool hasHead = git_repository_head(&head, m_repo) == 0;
    const std::string headName = hasHead ? (git_reference_shorthand(head) ? git_reference_shorthand(head) : "") : "";
    if (hasHead) {
        git_reference_free(head);
    }
    if (headName == name) {
        fail("cannot delete the branch you are on: " + name);
    }

    // git refuses to delete a branch that carries commits no other reference
    // reaches, and it is right to: those commits become unreachable, and nothing
    // said so. Deleting anyway is the caller's explicit decision, not a default.
    if (!force && !branchMergedInto(name, base)) {
        fail("'" + name + "' has commits that are not in " +
             (base.empty() ? std::string("the current branch") : base) + " — deleting it "
             "loses them");
    }

    git_reference* branch = nullptr;
    check(git_branch_lookup(&branch, m_repo, name.c_str(), GIT_BRANCH_LOCAL),
          "no such branch: " + name);
    const int rc = git_branch_delete(branch);
    git_reference_free(branch);
    check(rc, "cannot delete branch '" + name + "'");
}

bool Repository::mergeInProgress() const {
    const std::filesystem::path gitDir = gitDirOf(m_repo);
    if (gitDir.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(gitDir / "MERGE_HEAD", ec);
}

MergeResult Repository::mergeBranch(const std::string& branch) {
    MergeResult result;

    // Whatever git can merge: a local branch, a remote-tracking one ("origin/main" —
    // the merge a pull makes by hand), a tag or an id. Looking the name up as a local
    // branch refused every one of those but the first, while `git merge` below takes
    // them all.
    const git_oid theirId = resolveRevisionToCommit(m_repo, branch);
    git_annotated_commit* theirHead = nullptr;
    check(git_annotated_commit_lookup(&theirHead, m_repo, &theirId),
          "cannot resolve head of '" + branch + "'");

    // The analysis has no side effects, so libgit2 answers it; the merge itself is the
    // binary's. libgit2's merge runs no rerere, no `.gitattributes` merge driver, no
    // clean/smudge filter, no hook and no signing, and its abort was a hard reset that
    // took the reader's unrelated changes with it.
    const git_annotated_commit* heads[1] = {theirHead};
    git_merge_analysis_t analysis = GIT_MERGE_ANALYSIS_NONE;
    git_merge_preference_t preference = GIT_MERGE_PREFERENCE_NONE;
    const int analysed = git_merge_analysis(&analysis, &preference, m_repo, heads, 1);
    git_annotated_commit_free(theirHead);
    check(analysed, "merge analysis failed");

    if (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) {
        result.status = MergeStatus::UpToDate;
        return result;
    }

    const std::string workDir = workDirOf(m_repo).string();
    if (workDir.empty()) {
        fail("cannot merge in a bare repository");
    }
    // A fast-forward advances the branch we are standing on — never a checkout of
    // theirs — and a real merge always gets its merge commit, whatever `merge.ff` says:
    // the analysis above already decided which of the two this is.
    const bool fastForward = (analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) != 0;
    const ProcessResult run =
        fastForward ? runGit({"merge", "--ff-only", "--no-stat", branch}, workDir)
                    : runGit({"merge", "--no-ff", "--no-edit", "--no-stat", branch}, workDir);

    if (run.exitCode == 0) {
        result.status = fastForward ? MergeStatus::FastForward : MergeStatus::Clean;
        return result;
    }
    // Stopped with the merge open: conflicts for the reader, or a hook that refused the
    // commit. Either way it is a state to finish on the conflict page, not a failure.
    if (mergeInProgress()) {
        result.status = MergeStatus::Conflicted;
        result.conflictCount = conflicts().size();
        return result;
    }
    fail("cannot merge '" + branch + "': " + failureText(run));
}

MergePreview Repository::previewMerge(const std::string& ours, const std::string& theirs) const {
    MergePreview preview;
    const std::filesystem::path workDir = workDirOf(m_repo);
    if (workDir.empty()) {
        return preview;
    }

    // --no-messages on purpose: the informational lines are localised, and a
    // parser that reads them would break on a machine with a different locale.
    // The tree id is not wanted either, only the names — so --name-only, and the
    // first line of stdout is skipped as the written tree.
    const ProcessResult result =
        runGit({"merge-tree", "--write-tree", "--name-only", "--no-messages", ours, theirs},
               workDir.string());

    // git refuses a pair with no merge to make (one side already reachable from the
    // other): exit 1 with the reason on stderr and nothing on stdout. There is no
    // prediction to offer, and the ordinary merge path handles that case itself.
    if (result.out.empty()) {
        return preview;
    }
    preview.available = true;
    preview.clean = result.exitCode == 0;

    std::istringstream stream(result.out);
    std::string treeLine;
    std::getline(stream, treeLine);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            preview.conflictedPaths.push_back(line);
        }
    }
    preview.conflictedLines = predictedConflictLines(treeLine, preview.conflictedPaths);
    return preview;
}

std::map<std::string, std::vector<LineSpan>> Repository::predictedConflictLines(
    const std::string& treeId, const std::vector<std::string>& paths) const {
    // merge-tree wrote the merged tree into the object database, every conflicted
    // file in it carrying its markers. Reading those blobs here is one lookup each;
    // asking the binary would be a process per file.
    std::map<std::string, std::vector<LineSpan>> out;
    git_oid id;
    git_tree* tree = nullptr;
    if (git_oid_fromstr(&id, treeId.c_str()) != 0 || git_tree_lookup(&tree, m_repo, &id) != 0) {
        return out;
    }
    for (const std::string& path : paths) {
        git_tree_entry* entry = nullptr;
        if (git_tree_entry_bypath(&entry, tree, path.c_str()) != 0) {
            continue; // a delete/modify leaves nothing at the path to point into
        }
        git_blob* blob = nullptr;
        if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB &&
            git_blob_lookup(&blob, m_repo, git_tree_entry_id(entry)) == 0) {
            if (!git_blob_is_binary(blob)) {
                const std::string content(static_cast<const char*>(git_blob_rawcontent(blob)),
                                          static_cast<std::size_t>(git_blob_rawsize(blob)));
                std::vector<LineSpan> spans = oursLineSpans(parseConflictDocument(content));
                if (!spans.empty()) {
                    out.emplace(path, std::move(spans));
                }
            }
            git_blob_free(blob);
        }
        git_tree_entry_free(entry);
    }
    git_tree_free(tree);
    return out;
}

bool Repository::gpgSignConfigured() const {
    std::string value = configValue("commit.gpgsign");
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value == "true" || value == "1" || value == "yes" || value == "on";
}

Oid Repository::commitMerge(const std::string& message) {
    const std::filesystem::path gitDir = gitDirOf(m_repo);
    Oid theirsHex;
    {
        std::ifstream mergeHead(gitDir / "MERGE_HEAD");
        if (mergeHead) {
            mergeHead >> theirsHex;
        }
    }
    if (theirsHex.empty()) {
        fail("no merge in progress: MERGE_HEAD is missing");
    }
    if (const bool sign = gpgSignConfigured(); sign || commitHooksEnabled(workDirOf(m_repo).string())) {
        commitWithGit(m_repo, message, sign, GitCommitKind::New);
        return resolveRevision("HEAD");
    }

    git_index* index = nullptr;
    check(git_repository_index(&index, m_repo), "cannot read the index");
    git_index_read(index, 1);
    git_oid treeId;
    check(git_index_write_tree(&treeId, index), "cannot write the index tree");
    git_tree* tree = nullptr;
    check(git_tree_lookup(&tree, m_repo, &treeId), "cannot look up the merged tree");

    git_reference* head = nullptr;
    check(git_repository_head(&head, m_repo), "cannot read HEAD");
    git_commit* ours = nullptr;
    check(git_reference_peel(reinterpret_cast<git_object**>(&ours), head, GIT_OBJECT_COMMIT),
          "cannot resolve HEAD commit");

    const git_oid theirsOid = parseOid(theirsHex);
    git_commit* theirs = nullptr;
    check(git_commit_lookup(&theirs, m_repo, &theirsOid), "cannot read MERGE_HEAD commit");

    git_signature* signature = defaultSignature(m_repo);
    const git_commit* parents[2] = {ours, theirs};
    git_oid newId;
    const int rc = git_commit_create(&newId, m_repo, "HEAD", signature, signature, nullptr,
                                     message.c_str(), tree, 2, parents);
    git_signature_free(signature);
    git_commit_free(theirs);
    git_commit_free(ours);
    git_reference_free(head);
    git_tree_free(tree);
    git_index_free(index);
    check(rc, "cannot create the merge commit");

    std::error_code ec;
    std::filesystem::remove(gitDir / "MERGE_HEAD", ec);
    std::filesystem::remove(gitDir / "MERGE_MSG", ec);
    std::filesystem::remove(gitDir / "MERGE_MODE", ec);

    return oidToString(&newId);
}

void Repository::abortMerge() {
    const std::string workDir = workDirOf(m_repo).string();
    if (workDir.empty()) {
        fail("cannot abort in a bare repository");
    }
    const std::filesystem::path gitDir = gitDirOf(m_repo);
    std::error_code ec;
    const auto present = [&](const char* marker) {
        return std::filesystem::exists(gitDir / marker, ec);
    };

    // Each operation is undone by its own command, which puts back exactly what the
    // operation changed and leaves the reader's unrelated edits where they were.
    std::vector<std::string> args;
    if (present("MERGE_HEAD")) {
        args = {"merge", "--abort"};
    } else if (present("CHERRY_PICK_HEAD")) {
        args = {"cherry-pick", "--abort"};
    } else if (present("REVERT_HEAD")) {
        args = {"revert", "--abort"};
    } else if (present("SQUASH_MSG") || !conflicts().empty()) {
        // A squash merge or a conflicting stash pop: neither leaves a head to abort to,
        // and `reset --merge` is what keeps local changes while undoing the merge.
        args = {"reset", "--merge"};
    } else {
        return; // nothing is open
    }

    const ProcessResult run = runGit(args, workDir);
    if (run.exitCode != 0) {
        fail("cannot abort: " + failureText(run));
    }
    std::filesystem::remove(gitDir / "SQUASH_MSG", ec);
    std::filesystem::remove(gitDir / "MERGE_MSG", ec);
}

bool Repository::rebaseInProgress() const {
    const std::filesystem::path gitDir = gitDirOf(m_repo);
    if (gitDir.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(gitDir / "rebase-merge", ec) ||
           std::filesystem::exists(gitDir / "rebase-apply", ec);
}

bool Repository::indexMatchesHead() const {
    git_tree* head = nullptr;
    if (git_revparse_single(reinterpret_cast<git_object**>(&head), m_repo, "HEAD^{tree}") != 0) {
        return false;
    }

    git_diff* diff = nullptr;
    const int rc = git_diff_tree_to_index(&diff, m_repo, head, nullptr, nullptr);
    git_tree_free(head);
    if (rc != 0) {
        return false;
    }
    const std::size_t changes = git_diff_num_deltas(diff);
    git_diff_free(diff);
    return changes == 0;
}

namespace {

/// Whether the git binary left a sequencer marker: an operation is still open, waiting
/// for the reader rather than finished.
bool sequencerInProgress(const std::string& marker, const std::filesystem::path& gitDir) {
    std::error_code ec;
    return std::filesystem::exists(gitDir / marker, ec) ||
           std::filesystem::exists(gitDir / "sequencer", ec);
}

/// git's generated squash message, with the `#` comment lines it carries for an
/// editor dropped: the reader of a commit should not read git's instructions.
std::string squashMessage(const std::filesystem::path& gitDir) {
    std::ifstream file(gitDir / "SQUASH_MSG");
    std::string out;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line[0] == '#') {
            continue;
        }
        out += line;
        out += '\n';
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

} // namespace

void Repository::restoreFile(const Oid& revision, const std::string& path, bool staged) {
    const std::filesystem::path workDir = workDirOf(m_repo);
    if (workDir.empty()) {
        fail("cannot restore a file in a bare repository");
    }
    // `git restore` rather than a checkout through libgit2: it is the command whose
    // whole job is "put this path back the way that revision has it", it follows the
    // filters the way a checkout does, and it can target the index or the working
    // tree without the caller spelling out the difference.
    std::vector<std::string> args = {"restore", "--source=" + revision};
    if (staged) {
        args.push_back("--staged");
    }
    args.push_back("--");
    args.push_back(path);
    const ProcessResult run = runGit(args, workDir.string());
    if (run.exitCode != 0) {
        fail("cannot restore " + path + " from " + revision + ": " +
             failureText(run));
    }
}

namespace {

bool isHexToken(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    for (const char c : token) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<BlameLine> Repository::blame(const std::string& path, const Oid& revision) const {
    const std::string workDir = workDirOf(m_repo).string();
    if (workDir.empty()) {
        fail("cannot blame in a bare repository");
    }
    // `--line-porcelain` repeats the full header for every line, so each line is
    // self-contained and the parser need not remember the last commit it saw.
    std::vector<std::string> args = {"blame", "--line-porcelain"};
    if (!revision.empty()) {
        args.push_back(revision);
    }
    args.push_back("--");
    args.push_back(path);
    const ProcessResult run = runGit(args, workDir);
    if (run.exitCode != 0) {
        fail("cannot blame " + path + ": " +
             failureText(run));
    }

    std::vector<BlameLine> out;
    std::istringstream stream(run.out);
    std::string line;
    BlameLine current;
    bool pending = false;
    const std::string zero(GIT_OID_HEXSZ, '0');

    while (std::getline(stream, line)) {
        // A content line ends the group; the text itself is not needed, the caller
        // renders the file and matches it to `finalLine`.
        if (!line.empty() && line.front() == '\t') {
            if (pending) {
                out.push_back(current);
                pending = false;
            }
            continue;
        }

        std::istringstream header(line);
        std::string sha;
        int original = 0;
        int finalLine = 0;
        if ((header >> sha >> original >> finalLine) && sha.size() == GIT_OID_HEXSZ &&
            isHexToken(sha)) {
            current = BlameLine{};
            current.originalLine = original;
            current.finalLine = finalLine;
            current.uncommitted = sha == zero;
            current.commit = current.uncommitted ? Oid{} : sha;
            pending = true;
            continue;
        }
        if (!pending) {
            continue;
        }
        if (line.rfind("author ", 0) == 0) {
            current.author = line.substr(std::string("author ").size());
        } else if (line.rfind("author-time ", 0) == 0) {
            current.whenUnix = std::stoll(line.substr(std::string("author-time ").size()));
        } else if (line.rfind("summary ", 0) == 0) {
            current.summary = line.substr(std::string("summary ").size());
        }
    }
    if (pending) {
        out.push_back(current);
    }
    return out;
}

std::vector<FileHistoryEntry> Repository::fileHistory(const std::string& path) const {
    const std::string workDir = workDirOf(m_repo).string();
    if (workDir.empty()) {
        fail("cannot read file history in a bare repository");
    }
    // A tab between fields, not a space: a subject holds spaces, and a path can too.
    const ProcessResult run =
        runGit({"log", "--follow", "--format=%H%x09%at%x09%an%x09%s", "--", path}, workDir);
    if (run.exitCode != 0) {
        fail("cannot read the history of " + path + ": " +
             failureText(run));
    }

    std::vector<FileHistoryEntry> out;
    std::istringstream stream(run.out);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        const std::size_t t1 = line.find('\t');
        const std::size_t t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
        const std::size_t t3 = t2 == std::string::npos ? std::string::npos : line.find('\t', t2 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos || t3 == std::string::npos) {
            continue;
        }
        FileHistoryEntry entry;
        entry.id = line.substr(0, t1);
        entry.whenUnix = std::stoll(line.substr(t1 + 1, t2 - t1 - 1));
        entry.author = line.substr(t2 + 1, t3 - t2 - 1);
        entry.summary = line.substr(t3 + 1);
        out.push_back(std::move(entry));
    }
    return out;
}

std::vector<ReflogEntry> Repository::reflog(std::size_t limit) const {
    const std::string workDir = workDirOf(m_repo).string();
    if (workDir.empty()) {
        fail("cannot read the reflog in a bare repository");
    }
    // \x1f between fields, never a NUL or a tab: a reflog subject can contain either.
    const ProcessResult run =
        runGit({"reflog", "show", "--format=%gd\x1f%H\x1f%h\x1f%an\x1f%cI\x1f%gs",
                "-n", std::to_string(limit)},
               workDir);
    if (run.exitCode != 0) {
        if (run.out.empty()) {
            return {};
        }
        fail("cannot read the reflog: " + failureText(run));
    }

    std::vector<ReflogEntry> out;
    std::istringstream stream(run.out);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true) {
            const std::size_t sep = line.find('\x1f', start);
            fields.push_back(line.substr(start, sep == std::string::npos ? std::string::npos
                                                                         : sep - start));
            if (sep == std::string::npos) {
                break;
            }
            start = sep + 1;
        }
        if (fields.size() < 6) {
            continue;
        }
        ReflogEntry entry;
        entry.selector = fields[0];
        entry.id = fields[1];
        entry.shortId = fields[2];
        entry.author = fields[3];
        entry.date = fields[4];
        entry.message = fields[5];
        const std::size_t colon = entry.message.find(": ");
        if (colon != std::string::npos) {
            entry.action = entry.message.substr(0, colon);
        }
        out.push_back(std::move(entry));
    }
    return out;
}

namespace {

bool isWildcard(const std::string& pattern) {
    return pattern.find_first_of("*?") != std::string::npos;
}

/// `feat*login` as an extended regex: the wildcards become `.*` and `.`, everything
/// else is literal.
std::string wildcardRegex(const std::string& pattern) {
    std::string out;
    for (const char c : pattern) {
        if (c == '*') {
            out += ".*";
        } else if (c == '?') {
            out += '.';
        } else {
            if (std::strchr("\\^$.|+()[]{}", c)) {
                out += '\\';
            }
            out += c;
        }
    }
    return out;
}

/// `2026-10-16 12:00:00 +0000`: git's own approxidate reads a bare number as a date
/// only past eight digits, and "@<seconds>" not at all.
std::string gitDateUtc(std::int64_t seconds) {
    const std::time_t time = static_cast<std::time_t>(seconds);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &time);
#else
    gmtime_r(&time, &parts);
#endif
    char text[40];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S +0000", &parts);
    return text;
}

bool looksLikeHash(const std::string& text) {
    return text.size() >= 4 && text.size() <= 64 &&
           std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

/// One `-z` record: hash, author time, name, email, source ref, raw message.
bool parseHit(std::string_view record, CommitHit& hit) {
    std::array<std::string_view, 6> fields;
    for (std::size_t i = 0; i + 1 < fields.size(); ++i) {
        const std::size_t cut = record.find('\x1f');
        if (cut == std::string_view::npos) {
            return false;
        }
        fields[i] = record.substr(0, cut);
        record.remove_prefix(cut + 1);
    }
    fields.back() = record;
    if (fields[0].empty()) {
        return false;
    }
    CommitInfo& commit = hit.commit;
    commit.id = std::string(fields[0]);
    commit.author.whenUnix = std::strtoll(std::string(fields[1]).c_str(), nullptr, 10);
    commit.author.name = std::string(fields[2]);
    commit.author.email = std::string(fields[3]);
    hit.source = std::string(fields[4]);
    const std::string_view message = fields[5];
    const std::size_t eol = message.find('\n');
    commit.summary = std::string(message.substr(0, eol));
    if (eol != std::string_view::npos) {
        std::string_view body = message.substr(eol + 1);
        while (!body.empty() && body.front() == '\n') {
            body.remove_prefix(1);
        }
        while (!body.empty() && (body.back() == '\n' || body.back() == ' ')) {
            body.remove_suffix(1);
        }
        commit.body = std::string(body);
    }
    return true;
}

} // namespace

std::string Repository::streamCommitSearch(
    const std::string& workDir, const CommitQuery& query, std::size_t limit,
    const std::function<bool(std::vector<CommitHit>&&)>& deliver) {
    if (workDir.empty() || limit == 0) {
        return {};
    }
    const bool anything = !query.text.empty() || !query.author.empty() || query.since != 0 ||
                          query.until != 0;
    if (!anything) {
        return {};
    }
    const std::string format = "--format=%H%x1f%at%x1f%an%x1f%ae%x1f%S%x1f%B";

    std::string pinned;
    if (looksLikeHash(query.text) && query.author.empty() && query.since == 0 &&
        query.until == 0) {
        const ProcessResult resolved =
            runGit({"rev-parse", "--verify", "--quiet", query.text + "^{commit}"}, workDir);
        std::string oid = resolved.out;
        while (!oid.empty() && (oid.back() == '\n' || oid.back() == '\r')) {
            oid.pop_back();
        }
        if (resolved.exitCode == 0) {
            const ProcessResult shown = runGit({"log", "-1", "-z", format, oid}, workDir);
            CommitHit hit;
            if (shown.exitCode == 0 &&
                parseHit(std::string_view(shown.out).substr(0, shown.out.find('\0')), hit)) {
                pinned = hit.commit.id;
                if (!deliver({std::move(hit)})) {
                    return {};
                }
            }
        }
    }

    std::vector<std::string> args = {"log", "--all", "-z", format, "-n", std::to_string(limit),
                                     "-i"};
    if (query.firstParent) {
        args.push_back("--first-parent");
    }
    const bool regex = isWildcard(query.text) || isWildcard(query.author);
    args.push_back(regex ? "-E" : "-F");
    const auto pattern = [regex](const std::string& text) {
        return regex ? wildcardRegex(text) : text;
    };
    if (!query.text.empty()) {
        args.push_back("--grep=" + pattern(query.text));
    }
    if (!query.author.empty()) {
        args.push_back("--author=" + pattern(query.author));
    }
    if (query.since != 0) {
        args.push_back("--since=" + gitDateUtc(query.since));
    }
    if (query.until != 0) {
        args.push_back("--until=" + gitDateUtc(query.until));
    }

    std::string pending;
    bool stopped = false;
    const ProcessResult run = runGit(args, workDir, [&](std::string_view chunk) {
        pending.append(chunk);
        std::vector<CommitHit> batch;
        std::size_t start = 0;
        for (std::size_t end = pending.find('\0'); end != std::string::npos;
             end = pending.find('\0', start)) {
            CommitHit hit;
            if (parseHit(std::string_view(pending).substr(start, end - start), hit) &&
                hit.commit.id != pinned) {
                batch.push_back(std::move(hit));
            }
            start = end + 1;
        }
        pending.erase(0, start);
        if (!batch.empty() && !deliver(std::move(batch))) {
            stopped = true;
            return false;
        }
        return true;
    });
    if (stopped) {
        return {};
    }
    if (!pending.empty()) {
        CommitHit hit;
        if (parseHit(pending, hit) && hit.commit.id != pinned) {
            deliver({std::move(hit)});
        }
    }
    return run.exitCode == 0 ? std::string() : failureText(run);
}

std::string Repository::diffCliText(const Oid& base, const Oid& target,
                                    const std::string& path) const {
    const std::string workDir = workDirOf(m_repo).string();
    if (workDir.empty() || target.empty()) {
        return {};
    }
    // The empty tree's well-known id, for a base side that is "nothing". `git diff`
    // resolves it even when the object is not in the database.
    const std::string emptyTree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";
    // The prefixes and --no-renames are forced so the text matches what libgit2 would
    // have produced: this code detects no renames, and a config that does must not
    // change a diff the rest of the app compares against libgit2's shape.
    std::vector<std::string> args = {"-c",           "core.quotepath=false",
                                     "diff",         "--no-color",
                                     "--no-ext-diff", "--no-renames",
                                     "--src-prefix=a/", "--dst-prefix=b/",
                                     base.empty() ? emptyTree : base, target};
    if (!path.empty()) {
        args.push_back("--");
        args.push_back(path);
    }
    const ProcessResult run = runGit(args, workDir);
    if (run.exitCode != 0) {
        fail("cannot diff " + target + ": " + failureText(run));
    }
    return run.out;
}

std::string Repository::nameStatusCli(const Oid& base, const Oid& target) const {
    const std::string workDir = workDirOf(m_repo).string();
    if (workDir.empty() || target.empty()) {
        return {};
    }
    const std::string emptyTree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";
    const ProcessResult run =
        runGit({"-c", "core.quotepath=false", "diff", "--name-status", "--no-renames",
                base.empty() ? emptyTree : base, target},
               workDir);
    if (run.exitCode != 0) {
        fail("cannot list the changes of " + target + ": " +
             failureText(run));
    }
    return run.out;
}

std::string Repository::workdirDiffCli(const Oid& base, const std::string& path) const {
    const std::string workDir = workDirOf(m_repo).string();
    if (workDir.empty() || base.empty()) {
        return {};
    }
    // The same shape as diffCliText: `--no-renames` and fixed prefixes keep the text
    // the rest of the app was written against. The omission of a target is what makes
    // this a working-tree diff.
    std::vector<std::string> args = {"-c",            "core.quotepath=false",
                                     "diff",          "--no-color",
                                     "--no-ext-diff", "--no-renames",
                                     "--src-prefix=a/", "--dst-prefix=b/",
                                     base};
    if (!path.empty()) {
        args.push_back("--");
        args.push_back(path);
    }
    const ProcessResult run = runGit(args, workDir);
    if (run.exitCode != 0) {
        fail("cannot diff the working tree against " + base + ": " +
             failureText(run));
    }
    return run.out;
}

std::string Repository::indexDiffCli(bool cached, const std::string& path) const {
    const std::string workDir = workDirOf(m_repo).string();
    if (workDir.empty()) {
        return {};
    }
    std::vector<std::string> args = {"-c",            "core.quotepath=false",
                                     "diff",          "--no-color",
                                     "--no-ext-diff", "--no-renames",
                                     "--src-prefix=a/", "--dst-prefix=b/"};
    if (cached) {
        args.push_back("--cached");
    }
    args.push_back("--");
    args.push_back(path);
    const ProcessResult run = runGit(args, workDir);
    if (run.exitCode != 0) {
        fail("cannot diff " + path + ": " + failureText(run));
    }
    return run.out;
}

std::string Repository::workdirNameStatusCli(const Oid& base) const {
    const std::string workDir = workDirOf(m_repo).string();
    if (workDir.empty() || base.empty()) {
        return {};
    }
    const ProcessResult run = runGit(
        {"-c", "core.quotepath=false", "diff", "--name-status", "--no-renames", base}, workDir);
    if (run.exitCode != 0) {
        fail("cannot list the working-tree changes against " + base + ": " +
             failureText(run));
    }
    return run.out;
}

std::string Repository::firstParentOf(const Oid& commitId) const {
    const git_oid oid = parseOid(commitId);
    git_commit* commit = nullptr;
    if (git_commit_lookup(&commit, m_repo, &oid) != 0) {
        return {};
    }
    std::string base;
    if (git_commit_parentcount(commit) > 0) {
        const git_oid* parent = git_commit_parent_id(commit, 0);
        if (parent) {
            base = oidToString(parent);
        }
    }
    git_commit_free(commit);
    return base;
}

SquashResult Repository::squashMerge(const std::string& branch) {
    // Starting a second operation on top of an open one is what git refuses anyway,
    // and the refusal belongs here, in words that name the state.
    if (rebaseInProgress() || mergeInProgress() || squashInProgress()) {
        fail("a rebase, merge or squash is already in progress");
    }
    const std::filesystem::path workDir = workDirOf(m_repo);
    if (workDir.empty()) {
        fail("this repository has no working tree to squash into");
    }

    // The binary, not libgit2: `git merge --squash` produces the index state without
    // any merge metadata, and reproducing that by hand would only approximate it.
    const ProcessResult run = runGit({"merge", "--squash", branch}, workDir.string());
    SquashResult result;
    if (run.exitCode != 0) {
        // A conflict is a state, not a failure: the index carries the stages and the
        // reader resolves them. Anything else is a real error worth reporting.
        if (!conflicts().empty()) {
            result.status = SquashStatus::Conflicted;
            result.conflictCount = conflicts().size();
            return result;
        }
        fail("cannot squash-merge: " + failureText(run));
    }
    result.status = indexMatchesHead() ? SquashStatus::UpToDate : SquashStatus::Staged;
    return result;
}

bool Repository::squashInProgress() const {
    const std::filesystem::path gitDir = gitDirOf(m_repo);
    if (gitDir.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(gitDir / "SQUASH_MSG", ec);
}

Oid Repository::commitSquash(const std::string& message) {
    const std::filesystem::path gitDir = gitDirOf(m_repo);
    if (!std::filesystem::exists(gitDir / "SQUASH_MSG")) {
        fail("no squash in progress: SQUASH_MSG is missing");
    }
    std::string text = message.empty() ? squashMessage(gitDir) : message;
    if (text.empty()) {
        text = "Squash merge";
    }
    const Oid id = createCommit(text, gpgSignConfigured());
    // git removes these when `git commit` finishes a squash; libgit2's commit does
    // not, and a leftover SQUASH_MSG keeps the repository looking mid-operation.
    std::error_code ec;
    std::filesystem::remove(gitDir / "SQUASH_MSG", ec);
    std::filesystem::remove(gitDir / "MERGE_MSG", ec);
    std::filesystem::remove(gitDir / "MERGE_MODE", ec);
    return id;
}

RebaseResult Repository::rebaseOnto(const std::string& branch, const std::string& onto) {
    if (branch == onto) {
        fail("cannot rebase a branch onto itself");
    }
    // A second rebase on top of an open one is what git refuses, and the refusal belongs
    // here: the state is on disk, and the binary would only say so in its own words.
    if (rebaseInProgress() || mergeInProgress()) {
        fail("a rebase or merge is already in progress");
    }

    checkoutBranch(branch);

    const char* workDirRaw = git_repository_workdir(m_repo);
    const std::string workDir = workDirRaw ? workDirRaw : std::string();

    // The rebase runs through the git binary, not libgit2: libgit2 has no equivalent of
    // git's skipped cherry-picks. Given a commit whose change is already in the target it
    // replays it anyway, and the replay can conflict with its own original — a conflict
    // whose cause nobody can see, stopping a rebase git would have finished in silence.
    // Already based on the target: git says "up to date" and touches nothing. Asking
    // first also saves the subprocess and keeps the status off a translated message.
    {
        const ProcessResult base =
            runGit({"merge-base", "--is-ancestor", onto, branch}, workDir);
        if (base.exitCode == 0) {
            RebaseResult result;
            result.status = RebaseStatus::UpToDate;
            return result;
        }
    }

    const ProcessResult countBefore = runGit({"rev-list", "--count", onto + ".." + branch}, workDir);
    const long before = std::strtol(countBefore.out.c_str(), nullptr, 10);
    const ProcessResult tipBefore = runGit({"rev-parse", branch}, workDir);

    const ProcessResult run = runGit({"rebase", onto}, workDir);

    RebaseResult result;
    if (run.exitCode == 0) {
        const ProcessResult countAfter = runGit({"rev-list", "--count", onto + ".." + branch}, workDir);
        const long after = std::strtol(countAfter.out.c_str(), nullptr, 10);
        const ProcessResult tipAfter = runGit({"rev-parse", branch}, workDir);
        result.commitsApplied = static_cast<std::size_t>(std::max(0L, after));
        result.commitsSkipped = static_cast<std::size_t>(std::max(0L, before - after));
        // Nothing to replay and the branch did not move: this is "already up to date",
        // decided by comparing the tip rather than by reading git's translated message.
        // A branch that was behind but had nothing to replay has been fast-forwarded,
        // which is a move, not an up-to-date.
        result.status = (before == 0 && tipBefore.out == tipAfter.out) ? RebaseStatus::UpToDate
                                                                      : RebaseStatus::Completed;
        return result;
    }

    // A non-zero exit with the rebase still open is a conflict: git stopped where a
    // person is needed, which is a state and not a failure.
    if (rebaseInProgress()) {
        result.status = RebaseStatus::Conflicted;
        result.conflictCount = conflicts().size();
        return result;
    }

    fail("cannot rebase '" + branch + "' onto '" + onto + "': " + failureText(run));
}

RebaseResult Repository::rebaseContinue() {
    if (!rebaseInProgress()) {
        fail("no rebase to continue");
    }

    const char* workDirRaw = git_repository_workdir(m_repo);
    const std::string workDir = workDirRaw ? workDirRaw : std::string();

    // Started by the binary, continued by the binary: the two engines keep their state
    // in the same directory but understand different parts of it.
    const ProcessResult run =
        runGit({"-c", "core.editor=true", "rebase", "--continue"}, workDir);
    if (run.exitCode == 0) {
        RebaseResult result;
        result.status = RebaseStatus::Completed;
        return result;
    }
    if (rebaseInProgress()) {
        RebaseResult result;
        result.status = RebaseStatus::Conflicted;
        return result;
    }
    fail("cannot continue the rebase: " + failureText(run));
}

bool Repository::rangeIsLinear(const Oid& onto, const Oid& tip) const {
    const char* workDirRaw = git_repository_workdir(m_repo);
    const std::string workDir = workDirRaw ? workDirRaw : std::string();
    const std::string range = isRebaseRoot(onto) ? tip : onto + ".." + tip;
    // Counted twice: every commit in the range, and the range walked along first parents
    // only. Equal means the range is one uninterrupted line — no side branch merged in, and
    // no commit reachable by a second path. A todo list for a squash folds a commit into the
    // line above it, which is only the commit the reader picked when the range is one line.
    const ProcessResult all = runGit({"rev-list", "--count", range}, workDir);
    const ProcessResult line = runGit({"rev-list", "--count", "--first-parent", range}, workDir);
    if (all.exitCode != 0 || line.exitCode != 0) {
        return false;
    }
    return !all.out.empty() && all.out == line.out;
}

void Repository::rewriteHistory(const Oid& upstream, const std::vector<RewriteStep>& steps,
                                const Oid& onto, const std::string& branch) {
    if (rebaseInProgress() || mergeInProgress()) {
        fail("a rebase or merge is already in progress");
    }
    const char* workDirRaw = git_repository_workdir(m_repo);
    const std::string workDir = workDirRaw ? workDirRaw : std::string();
    if (workDir.empty()) {
        fail("cannot rewrite history in a bare repository");
    }
    const Oid tip = resolveRevision(branch.empty() ? std::string("HEAD") : branch);
    if (tip.empty()) {
        fail("cannot resolve the branch to rewrite");
    }

    // The todo is rebuilt from the plan, which is what lets the reader reorder commits —
    // but it also means a commit the plan forgets is a commit git never replays. The two
    // sets are compared before anything is written or started, so a malformed plan is a
    // refusal rather than a silent loss: an action git's todo does not recognise is not an
    // error it stops on, and neither is a missing line.
    const std::vector<RebaseCandidate> candidates = rebaseCandidates(upstream, tip);
    if (candidates.size() != steps.size()) {
        fail("the rewrite plan names " + std::to_string(steps.size()) +
             " commit(s) but the range holds " + std::to_string(candidates.size()));
    }
    std::vector<std::string> remaining;
    remaining.reserve(candidates.size());
    for (const RebaseCandidate& candidate : candidates) {
        remaining.push_back(candidate.id);
    }
    for (const RewriteStep& step : steps) {
        const auto at = std::find(remaining.begin(), remaining.end(), step.commit);
        if (at == remaining.end()) {
            fail("the rewrite plan names a commit outside the range: " + step.commit);
        }
        remaining.erase(at);
    }
    if (steps.empty()) {
        fail("a rewrite needs at least one commit");
    }

    const std::filesystem::path temp = gitDirOf(m_repo) / "gitmycelium-rewrite";
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    std::filesystem::create_directories(temp, ec);

    // The plan, one line per commit, in the order the reader arranged them. The action
    // words are this file's own vocabulary — the sequence editor below is the only reader
    // — and a reword carries its replacement message in a file of its own so the message
    // cannot be mangled by shell quoting.
    std::string plan;
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const RewriteStep& step = steps[index];
        const char* action = "pick";
        if (step.action == RewriteAction::Drop) {
            action = "drop";
        } else if (step.action == RewriteAction::Squash) {
            action = "squash";
        } else if (step.action == RewriteAction::Fixup) {
            action = "fixup";
        } else if (step.action == RewriteAction::Reword) {
            action = "reword";
        } else if (step.action == RewriteAction::Edit) {
            action = "edit";
        }
        plan += std::string(action) + " " + step.commit;
        if (step.action == RewriteAction::Reword) {
            const std::string messageName = "msg-" + std::to_string(index);
            std::ofstream(temp / messageName, std::ios::binary) << step.message;
            plan += " " + messageName;
        }
        plan += "\n";
    }
    std::ofstream(temp / "plan", std::ios::binary) << plan;

    // The sequence editor rebuilds the todo from the plan, in the plan's order. Full
    // hashes go into the todo; git resolves them. An `exec` line names its scripts through
    // `git rev-parse --git-path`, never by absolute path: the todo is run by the shell,
    // and a repository path holding a quote or a `$` would be read as shell syntax.
    std::ofstream(temp / "sequence-editor", std::ios::binary) << R"SH(#!/bin/sh
plan="${0%/*}/plan"
dir=gitmycelium-rewrite
out="$1.new"
: > "$out"
while IFS=' ' read -r action sha msg; do
    [ -z "$action" ] && continue
    case "$action" in
        reword)
            printf 'pick %s\n' "$sha" >> "$out"
            printf 'exec sh "$(git rev-parse --git-path %s/set-message)" "$(git rev-parse --git-path %s/%s)"\n' "$dir" "$dir" "$msg" >> "$out"
            ;;
        edit)
            printf 'edit %s\n' "$sha" >> "$out"
            ;;
        *)
            printf '%s %s\n' "$action" "$sha" >> "$out"
            ;;
    esac
done < "$plan"
mv "$out" "$1"
)SH";

    // Applies a message to the commit just replayed. A reword runs it from the todo; the
    // message is an argument, so nothing about it has to survive quoting.
    std::ofstream(temp / "set-message", std::ios::binary) << R"(#!/bin/sh
git commit --amend --only -F "$1" >/dev/null
)";

    // The editor git opens on a message. A squash has the combined message git prepared,
    // which is what a reader expects, and a fixup does not open this at all.
    std::ofstream(temp / "message-editor", std::ios::binary) << R"(#!/bin/sh
true
)";

    for (const char* script : {"sequence-editor", "message-editor", "set-message"}) {
        std::filesystem::permissions(temp / script,
                                     std::filesystem::perms::owner_exec |
                                         std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add, ec);
    }

    // `onto` empty means replay onto `upstream` itself; a branch means replay that branch
    // rather than the checked-out one. Both are what the badge-drop's "interactive rebase"
    // needs, and neither changes the ordinary current-branch path.
    // git runs an editor through the shell, so the path is quoted for it: a repository
    // under a directory with a space in its name split it in two. Forward slashes even
    // on Windows: the sequence editor finds its plan beside itself with `${0%/*}`.
    std::vector<std::string> args = {
        "-c", "sequence.editor=" + shellQuoted((temp / "sequence-editor").generic_string()),
        "-c", "core.editor=" + shellQuoted((temp / "message-editor").generic_string()),
        "rebase", "-i"};
    if (!onto.empty()) {
        args.push_back("--onto");
        args.push_back(onto);
    }
    if (isRebaseRoot(upstream)) {
        args.push_back("--root");
    } else {
        args.push_back(upstream);
    }
    if (!branch.empty()) {
        args.push_back(branch);
    }
    const ProcessResult run = runGit(args, workDir);
    // A rebase stopped for `edit` exits 0 and leaves state on disk; a conflicted one exits
    // non-zero. Only a failure that left nothing behind is a failure here.
    if (run.exitCode != 0 && !rebaseInProgress()) {
        fail("cannot rewrite the history: " +
             failureText(run));
    }
}

std::vector<RebaseCandidate> Repository::rebaseCandidates(const Oid& upstream,
                                                          const Oid& tip) const {
    const char* workDirRaw = git_repository_workdir(m_repo);
    const std::string workDir = workDirRaw ? workDirRaw : std::string();
    if (workDir.empty()) {
        fail("cannot list a rebase range in a bare repository");
    }
    // `--reverse` gives the order git's todo uses (oldest first); `--no-merges` matches the
    // flattening `git rebase` does by default. One process, tab-separated: a subject holds
    // spaces.
    const std::string range = isRebaseRoot(upstream) ? tip : upstream + ".." + tip;
    const ProcessResult run = runGit(
        {"log", "--reverse", "--no-merges", "--format=%H%x09%s", range}, workDir);
    if (run.exitCode != 0) {
        fail("cannot list the rebase range " + range + ": " + failureText(run));
    }
    std::vector<RebaseCandidate> out;
    std::istringstream lines(run.out);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        const std::size_t tab = line.find('\t');
        RebaseCandidate candidate;
        candidate.id = tab == std::string::npos ? line : line.substr(0, tab);
        candidate.subject = tab == std::string::npos ? std::string() : line.substr(tab + 1);
        out.push_back(std::move(candidate));
    }
    return out;
}

void Repository::dropCommit(const Oid& commitId) {
    const char* workDirRaw = git_repository_workdir(m_repo);
    const std::string workDir = workDirRaw ? workDirRaw : std::string();

    if (rebaseInProgress() || mergeInProgress()) {
        fail("a rebase or merge is already in progress");
    }
    // The first commit has no parent to replay onto: it is dropped from a root rebase,
    // which makes the commit above it the new root.
    const std::string parent = resolveRevision(commitId + "^");
    if (parent.empty()) {
        const Oid tip = resolveRevision("HEAD");
        std::vector<RewriteStep> steps;
        bool found = false;
        for (const RebaseCandidate& candidate : rebaseCandidates(kRebaseRoot, tip)) {
            RewriteStep step;
            step.commit = candidate.id;
            if (candidate.id == commitId) {
                step.action = RewriteAction::Drop;
                found = true;
            } else {
                step.action = RewriteAction::Keep;
            }
            steps.push_back(std::move(step));
        }
        if (!found) {
            fail("cannot drop " + commitId + ": it is not in this branch's history");
        }
        rewriteHistory(kRebaseRoot, steps);
        return;
    }

    // The one-liner git itself recommends: replay everything after `commitId` onto its
    // parent, which leaves `commitId` out. Through the binary, like the rebase, so the
    // same skipped cherry-picks and the same conflict behaviour apply.
    const ProcessResult run =
        runGit({"rebase", "--onto", parent, commitId}, workDir);
    if (run.exitCode != 0) {
        if (rebaseInProgress()) {
            RebaseResult conflict;
            conflict.status = RebaseStatus::Conflicted;
            conflict.conflictCount = conflicts().size();
            (void)conflict;
            return;
        }
        fail("cannot drop " + commitId + ": " + failureText(run));
    }
}

std::string Repository::resolveRevision(const std::string& revision) const {
    git_object* object = nullptr;
    if (git_revparse_single(&object, m_repo, revision.c_str()) != 0) {
        return std::string();
    }
    // A tag or a tree is not a commit: peel until it is one, or report nothing.
    git_object* commitObject = nullptr;
    const int rc = git_object_peel(&commitObject, object, GIT_OBJECT_COMMIT);
    git_object_free(object);
    if (rc != 0) {
        return std::string();
    }
    const std::string id = oidToString(git_object_id(commitObject));
    git_object_free(commitObject);
    return id;
}

void Repository::rebaseAbort() {
    // The binary started it, the binary ends it. libgit2 keeps its own idea of the
    // state and aborting through the wrong one leaves the repository half in a rebase.
    const char* workDirRaw = git_repository_workdir(m_repo);
    const std::string workDir = workDirRaw ? workDirRaw : std::string();
    const ProcessResult run = runGit({"rebase", "--abort"}, workDir);
    check(run.exitCode, "cannot abort the rebase: " + failureText(run));
}

void Repository::checkoutCommit(const Oid& commitId) {
    const git_oid oid = parseOid(commitId);
    git_object* target = nullptr;
    check(git_object_lookup(&target, m_repo, &oid, GIT_OBJECT_COMMIT),
          "cannot resolve commit " + commitId);

    git_object_free(target);
    switchWithGit(m_repo, {"--detach", commitId});
}

void Repository::moveBranch(const std::string& name, const Oid& target, bool force) {
    git_reference* branch = nullptr;
    check(git_branch_lookup(&branch, m_repo, name.c_str(), GIT_BRANCH_LOCAL),
          "no such branch: " + name);

    const git_oid oid = resolveRevisionToCommit(m_repo, target);
    git_object* targetObject = nullptr;
    check(git_object_lookup(&targetObject, m_repo, &oid, GIT_OBJECT_COMMIT),
          "cannot resolve commit " + target);

    git_reference* head = nullptr;
    const bool onHead = git_repository_head(&head, m_repo) == 0 && head != nullptr &&
                        git_reference_shorthand(head) != nullptr &&
                        name == git_reference_shorthand(head);
    if (head) {
        git_reference_free(head);
    }

    if (onHead) {
        // Moving the branch you are standing on moves the working tree with it, and
        // that can discard uncommitted work. The safe form refuses to do that;
        // discarding it is the caller's explicit decision, never a side effect of
        // dragging a label. The binary, as for every checkout.
        try {
            readTreeWithGit(m_repo, oidToString(&oid), force,
                            "moving '" + name + "' would discard uncommitted changes");
        } catch (...) {
            git_object_free(targetObject);
            git_reference_free(branch);
            throw;
        }
    }

    // The tree is in place, so the ref can follow. It is set last: a refusal above
    // must leave the repository exactly as it was.
    git_reference* moved = nullptr;
    const int rc = git_reference_set_target(&moved, branch, &oid, "GitMycelium: move branch");
    if (moved) {
        git_reference_free(moved);
    }
    git_reference_free(branch);
    git_object_free(targetObject);
    check(rc, "cannot move branch '" + name + "'");
}

UpstreamInfo Repository::upstreamInfo(const std::string& branch) const {
    UpstreamInfo info;

    git_reference* head = nullptr;
    if (branch.empty()) {
        if (git_repository_head(&head, m_repo) != 0 || !head) {
            return info;
        }
    } else {
        if (git_branch_lookup(&head, m_repo, branch.c_str(), GIT_BRANCH_LOCAL) != 0 || !head) {
            return info;
        }
    }

    git_reference* upstream = nullptr;
    if (git_branch_upstream(&upstream, head) == 0 && upstream) {
        info.configured = true;
        if (const char* name = git_reference_shorthand(upstream)) {
            info.name = name;
        }
        const git_oid* localId = git_reference_target(head);
        const git_oid* upstreamId = git_reference_target(upstream);
        if (localId && upstreamId) {
            std::size_t ahead = 0;
            std::size_t behind = 0;
            if (git_graph_ahead_behind(&ahead, &behind, m_repo, localId, upstreamId) == 0) {
                info.ahead = ahead;
                info.behind = behind;
            }
        }
        git_reference_free(upstream);
    }

    git_reference_free(head);
    return info;
}

std::string Repository::targetBranch() const {
    // What a remote says its default branch is, preferring `origin`. The local
    // counterpart wins when it exists, because that is the branch the reader
    // actually types into the box.
    git_strarray remotes = {nullptr, 0};
    if (git_remote_list(&remotes, m_repo) == 0) {
        std::string chosen;
        for (std::size_t i = 0; i < remotes.count; ++i) {
            const std::string name = remotes.strings[i] ? remotes.strings[i] : "";
            if (name == "origin") {
                chosen = name;
                break;
            }
            if (chosen.empty()) {
                chosen = name;
            }
        }
        git_strarray_dispose(&remotes);

        if (!chosen.empty()) {
            const std::string prefix = "refs/remotes/" + chosen + "/";
            git_reference* symbolic = nullptr;
            if (git_reference_lookup(&symbolic, m_repo, (prefix + "HEAD").c_str()) == 0 &&
                symbolic) {
                if (git_reference_type(symbolic) == GIT_REFERENCE_SYMBOLIC) {
                    const char* targetRef = git_reference_symbolic_target(symbolic);
                    const std::string full = targetRef ? targetRef : "";
                    if (full.rfind(prefix, 0) == 0) {
                        const std::string branch = full.substr(prefix.size());
                        const std::string localRef = "refs/heads/" + branch;
                        git_reference* local = nullptr;
                        if (git_reference_lookup(&local, m_repo, localRef.c_str()) == 0 && local) {
                            git_reference_free(local);
                            git_reference_free(symbolic);
                            return localRef;
                        }
                        git_reference_free(symbolic);
                        return full;
                    }
                }
                git_reference_free(symbolic);
            }
        }
    }

    for (const char* candidate : {"refs/heads/main", "refs/heads/master"}) {
        git_reference* ref = nullptr;
        if (git_reference_lookup(&ref, m_repo, candidate) == 0 && ref) {
            git_reference_free(ref);
            return candidate;
        }
    }
    return {};
}

std::string Repository::configValue(const std::string& key) const {
    git_config* config = nullptr;
    if (git_repository_config(&config, m_repo) != 0 || !config) {
        return {};
    }
    // `git_config_get_string` answers ENOTFOUND for a key that is plainly there on
    // libgit2 1.9.7, while `get_entry` reads it. Use the one that works.
    git_config_entry* entry = nullptr;
    std::string out;
    if (git_config_get_entry(&entry, config, key.c_str()) == 0 && entry) {
        out = entry->value ? entry->value : "";
    }
    if (entry) {
        git_config_entry_free(entry);
    }
    git_config_free(config);
    return out;
}

void Repository::setConfigValue(const std::string& key, const std::string& value) {
    git_config* config = nullptr;
    check(git_repository_config(&config, m_repo), "cannot open the configuration");
    int rc = 0;
    if (value.empty()) {
        // Removing a key that is not there is the state the caller asked for.
        rc = git_config_delete_entry(config, key.c_str());
        if (rc == GIT_ENOTFOUND) {
            rc = 0;
        }
    } else {
        rc = git_config_set_string(config, key.c_str(), value.c_str());
    }
    git_config_free(config);
    check(rc, "cannot write the configuration value " + key);
}

CommitSignature Repository::commitSignature(const std::string& revision) const {
    CommitSignature signature;
    const std::string workDir = workDirOf(m_repo).string();
    if (workDir.empty()) {
        fail("a bare repository has no working tree to check a signature in");
    }
    // `%G?` is a one-letter code and does not localise; `%GS`/`%GK` name the signer
    // and the key. Reading this runs the signature check, so it is asked for one
    // commit at a time and never while walking history.
    const ProcessResult run =
        runGit({"log", "-1", "--format=%G?%x1f%GS%x1f%GK", revision}, workDir);
    if (run.exitCode != 0) {
        fail("cannot read the commit signature: " +
             failureText(run));
    }
    std::string line = run.out;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    const std::size_t first = line.find('\x1f');
    if (first == std::string::npos) {
        signature.status = line;
        return signature;
    }
    signature.status = line.substr(0, first);
    const std::size_t second = line.find('\x1f', first + 1);
    if (second == std::string::npos) {
        signature.signer = line.substr(first + 1);
        return signature;
    }
    signature.signer = line.substr(first + 1, second - first - 1);
    signature.key = line.substr(second + 1);
    return signature;
}

Oid Repository::mergeBase(const Oid& a, const Oid& b) const {
    const git_oid left = resolveRevisionToCommit(m_repo, a);
    const git_oid right = resolveRevisionToCommit(m_repo, b);
    git_oid base;
    if (git_merge_base(&base, m_repo, &left, &right) != 0) {
        // Unrelated histories have no merge base; that is an answer, not an error.
        return {};
    }
    return oidToString(&base);
}

bool Repository::canFastForward(const std::string& branch, const Oid& target) const {
    git_reference* ref = nullptr;
    check(git_branch_lookup(&ref, m_repo, branch.c_str(), GIT_BRANCH_LOCAL),
          "no such branch: " + branch);

    git_commit* branchTip = nullptr;
    const int peelRc =
        git_reference_peel(reinterpret_cast<git_object**>(&branchTip), ref, GIT_OBJECT_COMMIT);
    git_reference_free(ref);
    check(peelRc, "cannot resolve branch tip: " + branch);

    const git_oid targetOid = resolveRevisionToCommit(m_repo, target);

    // Fast-forward means: the branch tip is an ancestor of the target. Asking
    // for the merge base is the cheap way to check that (git_merge_base returns
    // the best common ancestor, which must be the tip itself).
    bool result = false;
    if (git_oid_equal(git_commit_id(branchTip), &targetOid)) {
        result = true;
    } else {
        git_oid base;
        if (git_merge_base(&base, m_repo, git_commit_id(branchTip), &targetOid) == 0) {
            result = git_oid_equal(&base, git_commit_id(branchTip));
        }
    }
    git_commit_free(branchTip);
    return result;
}

void Repository::fastForwardBranch(const std::string& branch, const Oid& target) {
    // The guard is the whole point: without it a typo of "merge" silently drops
    // commits off the branch.
    if (!canFastForward(branch, target)) {
        fail("not a fast-forward: '" + branch + "' has commits that would be lost");
    }
    const git_oid targetOid = resolveRevisionToCommit(m_repo, target);
    moveBranch(branch, oidToString(&targetOid));
}

void Repository::renameBranch(const std::string& from, const std::string& to) {
    git_reference* branch = nullptr;
    check(git_branch_lookup(&branch, m_repo, from.c_str(), GIT_BRANCH_LOCAL),
          "no such branch: " + from);

    git_reference* renamed = nullptr;
    const int rc = git_branch_move(&renamed, branch, to.c_str(), 0);
    if (renamed) {
        git_reference_free(renamed);
    }
    git_reference_free(branch);
    check(rc, "cannot rename '" + from + "' to '" + to + "'");
}

Oid Repository::cherryPick(const std::vector<Oid>& commits) {
    if (commits.empty()) {
        fail("nothing to cherry-pick");
    }
    const char* workDirRaw = git_repository_workdir(m_repo);
    const std::string workDir = workDirRaw ? workDirRaw : std::string();

    // Through the git binary: libgit2 applies the change and then leaves
    // `CHERRY_PICK_HEAD` behind after it succeeds, so the repository looks like an
    // operation is still open. The guard that refuses a second operation — the one that
    // exists to stop exactly that — then refuses everything.
    // git cleans up after a successful pick, keeps the author, and words the message its
    // own way; none of that has to be reproduced by hand here.
    // Several at once: git replays them in the order given, and stops on the first one that
    // conflicts with the whole run still open — the same state a single pick leaves, so the
    // conflict path needs no second version of itself.
    std::vector<std::string> args{"cherry-pick"};
    args.insert(args.end(), commits.begin(), commits.end());
    const ProcessResult run = runGit(args, workDir);
    if (run.exitCode != 0) {
        if (sequencerInProgress("CHERRY_PICK_HEAD", gitDirOf(m_repo))) {
            fail("the cherry-pick stopped with conflicts — they are in the index");
        }
        fail("cannot cherry-pick: " + failureText(run));
    }
    return resolveRevision("HEAD");
}

void Repository::amendHeadMessage(const std::string& message) {
    git_reference* head = nullptr;
    check(git_repository_head(&head, m_repo), "no commit to amend: HEAD is unborn");
    const git_oid* headId = git_reference_target(head);

    git_commit* commit = nullptr;
    const int lookup = git_commit_lookup(&commit, m_repo, headId);
    git_reference_free(head);
    check(lookup, "cannot read the HEAD commit");

    git_tree* tree = nullptr;
    const int treeRc = git_commit_tree(&tree, commit);

    git_oid amended{};
    int rc = treeRc;
    if (treeRc == 0) {
        rc = git_commit_amend(&amended, commit, "HEAD", git_commit_author(commit),
                              git_commit_committer(commit), "UTF-8", message.c_str(), tree);
    }
    git_tree_free(tree);
    git_commit_free(commit);
    check(rc, "cannot amend the commit message");
}

void Repository::amendHeadMessage(const std::string& message, bool sign) {
    if (!sign && !commitHooksEnabled(workDirOf(m_repo).string())) {
        amendHeadMessage(message);
        return;
    }
    commitWithGit(m_repo, message, sign, GitCommitKind::AmendMessage);
}

Oid Repository::amendHead(const std::string& message, bool sign) {
    git_reference* head = nullptr;
    check(git_repository_head(&head, m_repo), "no commit to amend: HEAD is unborn");
    git_reference_free(head);
    commitWithGit(m_repo, message, sign, GitCommitKind::Amend);
    return resolveRevision("HEAD");
}

void Repository::amendStoppedCommit(const std::string& message) {
    if (!rebaseInProgress()) {
        fail("no rebase is stopped to amend");
    }
    const char* workDirRaw = git_repository_workdir(m_repo);
    const std::string workDir = workDirRaw ? workDirRaw : std::string();
    if (workDir.empty()) {
        fail("cannot amend a stopped commit in a bare repository");
    }
    // `git commit --amend` rather than git_commit_amend: it takes the index, so the
    // reader's staged changes are what the amended commit holds, and it is the command
    // that knows it is running inside an open rebase. The editor is silenced because
    // there is nobody to answer it.
    std::vector<std::string> args = {"-c", "core.editor=true", "commit", "--amend"};
    if (message.empty()) {
        args.push_back("--no-edit");
    } else {
        const std::filesystem::path temp = gitDirOf(m_repo) / "gitmycelium-rewrite";
        std::error_code ec;
        std::filesystem::create_directories(temp, ec);
        const std::filesystem::path messageFile = temp / "amend-message";
        std::ofstream(messageFile, std::ios::binary) << message;
        args.push_back("-F");
        args.push_back(messageFile.string());
    }
    const ProcessResult run = runGit(args, workDir);
    if (run.exitCode != 0) {
        fail("cannot amend the stopped commit: " +
             failureText(run));
    }
}

void Repository::resetTo(const Oid& commitId, ResetMode mode) {
    if (mode == ResetMode::Hard && !workDirOf(m_repo).empty() &&
        externalFilterConfigured(m_repo, {})) {
        // A hard reset rewrites the working tree, and only the binary runs the
        // filters that put a file's real content there instead of its pointer.
        const ProcessResult run =
            runGit({"reset", "--hard", "-q", commitId}, workDirOf(m_repo).string());
        if (run.exitCode != 0) {
            fail("cannot reset to " + commitId + ": " + failureText(run));
        }
        return;
    }
    const git_oid targetOid = parseOid(commitId);
    git_object* target = nullptr;
    check(git_object_lookup(&target, m_repo, &targetOid, GIT_OBJECT_COMMIT),
          "no such commit: " + commitId);

    git_reset_t kind = GIT_RESET_MIXED;
    switch (mode) {
    case ResetMode::Soft: kind = GIT_RESET_SOFT; break;
    case ResetMode::Mixed: kind = GIT_RESET_MIXED; break;
    case ResetMode::Hard: kind = GIT_RESET_HARD; break;
    }

    const int rc = git_reset(m_repo, target, kind, nullptr);
    git_object_free(target);
    check(rc, "cannot reset to " + commitId);
}

Oid Repository::revertCommit(const Oid& commitId) {
    const char* workDirRaw = git_repository_workdir(m_repo);
    const std::string workDir = workDirRaw ? workDirRaw : std::string();

    // Same reasoning as the cherry-pick above, including the sequencer state that git
    // cleans up and libgit2 does not. `--no-edit` keeps git's own Revert message without
    // opening an editor there is nobody to drive.
    const ProcessResult run = runGit({"revert", "--no-edit", commitId}, workDir);
    if (run.exitCode != 0) {
        if (sequencerInProgress("REVERT_HEAD", gitDirOf(m_repo))) {
            fail("the revert stopped with conflicts — they are in the index");
        }
        fail("cannot revert " + commitId + ": " + failureText(run));
    }
    return resolveRevision("HEAD");
}

void Repository::createTag(const std::string& name, const Oid& target,
                           const std::string& message) {
    const git_oid tagTarget = parseOid(target);
    git_object* object = nullptr;
    check(git_object_lookup(&object, m_repo, &tagTarget, GIT_OBJECT_COMMIT),
          "no such commit: " + target);

    git_oid tagId;
    int rc = 0;
    if (message.empty()) {
        rc = git_tag_create_lightweight(&tagId, m_repo, name.c_str(), object, 0);
    } else {
        git_signature* signature = defaultSignature(m_repo);
        rc = git_tag_create(&tagId, m_repo, name.c_str(), object, signature, message.c_str(), 0);
        git_signature_free(signature);
    }
    git_object_free(object);
    check(rc, "cannot create tag '" + name + "'");
}

void Repository::deleteTag(const std::string& name) {
    git_reference* ref = nullptr;
    check(git_reference_lookup(&ref, m_repo, ("refs/tags/" + name).c_str()),
          "no such tag: " + name);
    git_reference_free(ref);
    check(git_tag_delete(m_repo, name.c_str()), "cannot delete tag '" + name + "'");
}

std::string Repository::formatPatch(const Oid& commitId) const {
    const RepoInfo repoInfo = info();
    if (repoInfo.workDir.empty()) {
        fail("cannot create a patch from a bare repository");
    }
    const ProcessResult result =
        runGit({"format-patch", "-1", commitId, "--stdout"}, repoInfo.workDir);
    if (result.exitCode != 0) {
        fail("cannot create patch for " + commitId + ": " + result.err);
    }
    return result.out;
}

} // namespace gk
