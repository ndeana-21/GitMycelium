#include "git/repository.h"
#include "graph/graph_layout.h"
#include "repo/repo_session.h"
#include "ui/main_window.h"
#include "update/updater.h"
#include "ui/theme_apply.h"
#include "theme/theme.h"

#include <QApplication>
#include <QEventLoop>
#include <QIcon>
#include <QFont>
#include <QSettings>
#include <QStyleFactory>
#include <QElapsedTimer>
#include <QTimer>

#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

int probe(const std::string& path) {
    try {
        auto repo = gk::Repository::open(path);
        const gk::RepoInfo info = repo->info();

        std::printf("repo:        %s\n", path.c_str());
        std::printf("git dir:     %s\n", info.gitDir.c_str());
        std::printf("common dir:  %s\n",
                    info.commonDir.empty() ? "(same as git dir)" : info.commonDir.c_str());
        std::printf("work dir:    %s\n", info.workDir.empty() ? "(bare)" : info.workDir.c_str());
        std::printf("bare:        %s\n", info.bare ? "yes" : "no");
        std::printf("detached:    %s\n", info.detached ? "yes" : "no");
        std::printf("branch:      %s\n",
                    info.headBranch.empty() ? "(none)" : info.headBranch.c_str());
        std::printf("head:        %s\n", info.headId.empty() ? "(none)" : info.headId.c_str());
        std::printf("operation:   %s\n", gk::toString(info.operation));
        std::printf("partial:     %s\n", info.partialClone ? "yes" : "no");

        const auto refs = repo->refs();
        int local = 0;
        int remote = 0;
        int tags = 0;
        for (const auto& r : refs) {
            switch (r.kind) {
            case gk::RefKind::LocalBranch: ++local; break;
            case gk::RefKind::RemoteBranch: ++remote; break;
            case gk::RefKind::Tag: ++tags; break;
            default: break;
            }
        }
        std::printf("refs:        %zu total (%d local, %d remote, %d tags)\n", refs.size(), local,
                    remote, tags);

        bool hasMore = false;
        const auto page = repo->commits(0, 5, &hasMore);
        std::printf("commits:     showing %zu, hasMore=%s\n", page.size(),
                    hasMore ? "yes" : "no");
        for (const auto& c : page) {
            std::printf("  %s  %-28.28s  %s\n", c.id.substr(0, 10).c_str(), c.summary.c_str(),
                        c.author.name.c_str());
        }

        const auto changes = repo->status();
        std::printf("changes:     %zu\n", changes.size());
        int shown = 0;
        for (const auto& change : changes) {
            if (shown++ >= 10) {
                break;
            }
            std::printf("  %-9s %s %s\n", gk::toString(change.kind),
                        change.staged ? "staged  " : "unstaged", change.path.c_str());
        }

        const auto conflicts = repo->conflicts();
        std::printf("conflicts:   %zu\n", conflicts.size());
        for (const auto& conflict : conflicts) {
            std::printf("  %s  [%s]  base=%s ours=%s theirs=%s\n", conflict.path.c_str(),
                        gk::toString(conflict.kind),
                        conflict.ancestor.empty() ? "-" : conflict.ancestor.substr(0, 8).c_str(),
                        conflict.ours.empty() ? "-" : conflict.ours.substr(0, 8).c_str(),
                        conflict.theirs.empty() ? "-" : conflict.theirs.substr(0, 8).c_str());
        }

        // Printing the lanes makes a topology bug arguable instead of debatable.
        // Rows are newest-first, exactly as the view renders them.
        const auto all = repo->commits(0, 200, &hasMore, gk::HistoryFilter{gk::HistoryScope::AllRefs, {}});
        gk::GraphLayout graph;
        graph.setCommits(all);
        std::printf("graph:       %d lanes, %zu rows\n", graph.laneCount(), graph.size());
        const std::size_t rows = std::min<std::size_t>(graph.size(), 16);
        for (std::size_t i = 0; i < rows; ++i) {
            const gk::GraphNode& node = graph.node(i);
            std::printf("  %2zu %-10.10s lane=%-2d col=%-2d ecol=%-2d emax=%-2d %-4s type=%-9s %s\n",
                        i, all[i].id.c_str(), node.lane, node.colorIndex, node.colorColumn,
                        node.edgeColumnMax, node.isMerge ? "MERG" : "    ",
                        gk::graphNodeTypeName(node.type), all[i].summary.substr(0, 34).c_str());
            for (const gk::GraphEdge& edge : node.edges) {
                std::printf("       edge %d->%d col=%d toRow=%d%s\n", edge.fromLane, edge.toLane,
                            edge.colorIndex, edge.toRow,
                            edge.toRow < 0 ? " (parent not loaded)" : "");
            }
            std::printf("       above:");
            for (const gk::GraphLane& lane : node.activeLanesAbove) {
                std::printf(" %d/col%d", lane.lane, lane.colorIndex);
            }
            std::printf("   below:");
            for (const gk::GraphLane& lane : node.activeLanesBelow) {
                std::printf(" %d/col%d", lane.lane, lane.colorIndex);
            }
            if (!node.converging.empty()) {
                std::printf("   MEET:");
                for (const gk::GraphLane& lane : node.converging) {
                    std::printf(" %d/col%d", lane.lane, lane.colorIndex);
                }
            }
            std::printf("\n");
            // Derived from above/below/edges/converging; printing it keeps the
            // two from drifting apart.
            std::printf("       roles:");
            for (const gk::GraphRowEdge& edge : gk::rowEdgesOf(node)) {
                const char* role = edge.role == gk::GraphEdgeRole::Starting   ? "start"
                                   : edge.role == gk::GraphEdgeRole::Ending   ? "end"
                                                                             : "pass";
                std::printf(" %d/%s/col%d", edge.lane, role, edge.colorIndex);
                if (edge.role == gk::GraphEdgeRole::Starting && edge.toRow >= 0) {
                    std::printf("->%d", edge.toRow);
                }
            }
            std::printf("\n");
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "probe failed: %s\n", e.what());
        return 1;
    }
}

const char* rebaseStatusName(gk::RebaseStatus status) {
    switch (status) {
    case gk::RebaseStatus::UpToDate:
        return "up-to-date";
    case gk::RebaseStatus::Completed:
        return "completed";
    case gk::RebaseStatus::Conflicted:
        return "conflicted";
    }
    return "?";
}

int updateCheckHeadless(bool install) {
    using namespace gk::update;
    Updater updater;
    QEventLoop loop;
    bool ok = false;
    QString error;
    QObject::connect(&updater, &Updater::checked, &loop, [&](bool checkedOk, const QString& why) {
        ok = checkedOk;
        error = why;
        loop.quit();
    });
    updater.check();
    loop.exec();

    std::printf("installed: %s\n", describeVersion(currentVersion(), currentBuild()).toUtf8().constData());
    std::printf("install kind: %s\n",
                describeInstallKind(updater.installTarget().kind).toUtf8().constData());
    std::printf("manifest: %s\n", manifestUrl().toString().toUtf8().constData());
    if (!ok) {
        std::printf("error: %s\n", error.toUtf8().constData());
        return 1;
    }
    const UpdateManifest& manifest = *updater.manifest();
    std::printf("published: %s\n", describeVersion(manifest.version, manifest.build).toUtf8().constData());
    std::printf("update: %s\n", updater.updateAvailable() ? "yes" : "no");
    const QString refusal = updater.installRefusal();
    std::printf("installable: %s\n", refusal.isEmpty() ? "yes" : refusal.toUtf8().constData());
    if (!install) {
        return 0;
    }
    if (!refusal.isEmpty()) {
        return 1;
    }
    bool installed = false;
    QObject::connect(&updater, &Updater::installFinished, &loop,
                     [&](bool installedOk, const QString& why) {
                         installed = installedOk;
                         error = why;
                         loop.quit();
                     });
    updater.install();
    loop.exec();
    if (!installed) {
        std::printf("install: failed: %s\n", error.toUtf8().constData());
        return 1;
    }
    std::printf("install: ok\n");
    if (!updater.stagedBundle().isEmpty()) {
        std::printf("staged: %s\n", updater.stagedBundle().toUtf8().constData());
    }
    return 0;
}

int rebaseHeadless(const std::string& path, const QString& target, bool sweep) {
    gk::RepoSession session;
    QString error;
    if (!session.open(QString::fromStdString(path), &error)) {
        std::fprintf(stderr, "cannot open %s: %s\n", path.c_str(), qPrintable(error));
        return 1;
    }

    const QString branch = QString::fromStdString(session.info().headBranch);
    gk::RebaseResult result;
    if (!session.rebaseOnto(branch, target, &result, &error)) {
        std::fprintf(stderr, "rebase refused: %s\n", qPrintable(error));
        return 1;
    }

    std::printf("branch %s onto %s: %s\n", qPrintable(branch), qPrintable(target),
                rebaseStatusName(result.status));
    std::printf("  replayed %zu, skipped %zu, conflicts %zu\n", result.commitsApplied,
                result.commitsSkipped, result.conflictCount);

    if (sweep) {
        const gk::RepoSession::SweepOutcome outcome = session.sweepOperationConflicts();
        std::printf("  sweep: finished=%d needsAPerson=%d resolved=%zu\n",
                    outcome.finished ? 1 : 0, outcome.needsAPerson ? 1 : 0,
                    outcome.hunksResolved);
        for (const QString& file : outcome.autoStaged) {
            std::printf("    settled automatically: %s\n", qPrintable(file));
        }
        for (const QString& file : outcome.needsAPersonPaths) {
            std::printf("    waiting for a person: %s\n", qPrintable(file));
        }
        if (!outcome.error.isEmpty()) {
            std::printf("    error: %s\n", qPrintable(outcome.error));
        }
    }

    std::printf("  rebase still open: %d\n", session.rebaseInProgress() ? 1 : 0);
    std::printf("  conflicts now: %zu\n", session.conflicts().size());
    return 0;
}

void printUsage() {
    std::fprintf(stderr,
                 "usage: gitmycelium [--probe] [--shot <file.png>] [--help] [<repository-path>]\n"
                 "  --probe          print repository state to stdout and exit (no GUI)\n"
                 "  --menu           print the commit context menu and exit (no GUI)\n"
                 "  --shot <file>    render the window to a PNG and exit (headless check)\n"
                 "  --graph-width <px>  pin the graph column width before the snapshot\n"
                 "  --select-row <n>    select the nth commit before the snapshot\n"
                 "  --theme <dark|light>  start in a theme, without saving the choice\n"
                 "  --zoom <percent>    start at a zoom, without saving the choice\n"
                 "  --file-view         show the file instead of the diff in the snapshot\n"
                 "  --open-diff         open the diff in the middle column before the snapshot\n"
                 "  --rebase-onto <ref> rebase the current branch headlessly and report what\n"
                 "                      came of it (no GUI)\n"
                 "  --sweep             with --rebase-onto: also run the automatic conflict\n"
                 "                      pass, as the window does after a rebase stops\n"
                 "  --ui-rebase-onto <ref>  run the window's own rebase path (the one a drop\n"
                 "                      reaches) and print what the window then shows\n"
                 "  --hover-row <n>     report the label or hover badge of a row\n"
                 "  --ui-hover-expand <n>\n"
                 "                      reveal the branches a collapsed pill on row <n>\n"
                 "                      hides, as a hover would, and report their names\n"
                 "  --ui-hover-act <n>  double-click the ghost badge of a row and report\n"
                 "                      the branch it checks out\n"
                 "  --ui-selection <rows> [op]\n"
                 "  --ui-context-menu <rows> <row>\n"
                 "                      right-click a row with rows selected\n"
                 "                      select rows (0,1,2); with an op, run it and report\n"
                 "  --ui-navigate <row> <parent|child|tip>\n"
                 "                      navigate from a row and report where it landed\n"
                 "  --ui-commit-op <key> <ref>\n"
                 "                      run one commit-menu entry through the window path\n"
                 "  --ui-branch-drop <src> <target> [pick]\n"
                 "                      run the badge drop flow and, with a pick, click that\n"
                 "                      entry (rebase|merge|squash|interactive|check); reports\n"
                 "                      each step of the chain\n"
                 "  --ui-commit-panel <message>\n"
                 "                      stage every change and commit the message through\n"
                 "                      the window's Commit Panel path; reports the result\n"
                 "  --ui-command <query> [run]\n"
                 "                      list the command palette's matches for a query; with\n"
                 "                      run, run the best match through the window's path\n"
                 "  --ui-undo | --ui-redo | --ui-oplog\n"
                 "                      put back / re-apply the last operation, or print the\n"
                 "                      operation log, through the window's own path\n"
                 "  --ui-stage-hunk <path> <index> [unstage]\n"
                 "  --ui-stage-line <path> <hunk> <line> [unstage]\n"
                 "                      stage (or, with \"unstage\", take back) one hunk or\n"
                 "                      one line, through the window's own path\n"
                 "  --ui-discard-hunk <path> <index> [line]\n"
                 "                      throw away a working-tree hunk (or one line of it)\n"
                 "  --ui-stash <push|list|pop|apply|drop|branch> [arg]\n"
                 "                      run one stash command, arg being a message for\n"
                 "                      push, a branch name for branch, or an oid\n"
                 "  --ui-sync <fetch|fetch-all|pull|pull-merge|pull-rebase|push|\n"
                 "             push-force|push-tags|push-tag|delete-tag-all-remotes> [remote|tag]\n"
                 "                      run one network operation against a remote\n"
                 "  --ui-set-upstream <upstream>\n"
                 "                      make the current branch follow a remote branch\n"
                 "  --ui-autofetch <enable|disable|select|now> [remote,remote]\n"
                 "                      turn auto-fetch on/off, choose remotes, or fetch\n"
                 "                      now, through the window's own path\n"
                 "  --ui-preferences <get|set> <key> [value]\n"
                 "                      read or write one preference (theme, zoom,\n"
                 "                      preflight, autofetch.enabled, autofetch.remotes,\n"
                 "                      submodule.autoupdate[-remote], graph.animate,\n"
                 "                      rerere.enabled|autoupdate) and report it\n"
                 "  --ui-keymap <list|set <id> <seq>|reset <id>|reset-all>\n"
                 "                      list, rebind or clear a command shortcut through\n"
                 "                      the window's own binding path\n"
                 "  --ui-search <query> [text|dates:<from>..<to>] [reveal-hash]\n"
                 "                      fill the search window (Ctrl+F) and report its\n"
                 "                      blocks once git is done; optionally page the graph\n"
                 "                      to the named commit and report its row\n"
                 "  --ui-conflict <regions|take|line|takelines|clear-choices|delete|deleted|unresolve|save-draft|auto|next|prev|select|text|save|resolve|keep|smart|stage-worktree|mark-all|close|refresh> [a] [b]\n"
                 "                      drive the conflict editor through the window's\n"
                 "                      own path: list regions, take a side for one\n"
                 "                      (take <region> <ours|base|theirs|both|both-rev>),\n"
                 "                      navigate, select a file, print the result, save,\n"
                 "                      or settle every region with one side and save\n"
                 "                      (close leaves the page, refresh runs the window's\n"
                 "                      own pass — the page has to stay closed across it)\n"
                 "  --ui-merge-preview <ours> <theirs>\n"
                 "                      predict the conflicts a merge would hit, through\n"
                 "                      git merge-tree, without touching the working tree\n"
                 "  --ui-squash-merge <src> <target>\n"
                 "                      squash-merge a branch through the window's own path\n"
                 "                      and report the state it left behind\n"
                 "  --ui-visibility <hide|unhide|solo|unsolo|pin|unpin|show|smart|smart-off|\n"
                 "                   preset:<mine|recent|prefix|no-upstream|ahead|behind>|\n"
                 "                   preset-prefix:<text>|preset-off|merged-commits|\n"
                 "                   merged-commits-off|report> [branch]\n"
                 "                      filter the graph and report what it walks and labels\n"
                 "  --ui-worktree <list|add|remove|lock|unlock|prune|open> [args…]\n"
                 "                      drive worktrees through the window's own path and\n"
                 "                      report the resulting list (pass the repo path first)\n"
                 "  --ui-submodule <list|add|init|update|update-all|sync|sync-all|deinit|open>\n"
                 "                      [args…] drive submodules through the window's own\n"
                 "                      path and report the resulting list\n"
                 "                      (autoupdate <enable|disable|remote-on|remote-off|status|run>)\n"
                 "  --ui-graph-animation <commit|checkout|branch> [arg]\n"
                 "                      perform an operation and report the graph transitions it\n"
                 "                      produced (motion on, unlike other --ui-* checks)\n"
                 "  --ui-branch-group <delete|delete-merged|set-upstream|unmerged|hide|solo>\n"
                 "                      [args…] drive the group branch operations on explicit\n"
                 "                      names and report what went through\n"
                 "  --ui-apply-patch <file>\n"
                 "                      apply a patch file (mailbox → commits, diff → working\n"
                 "                      tree) and report which path was taken\n"
                 "  --ui-clone <url> <dir> [partial] [branch=<name>] [depth=<n>] [since=<date>]\n"
                 "                      clone through the git binary, open the result, and\n"
                 "                      report the branch and whether it is a partial clone\n"
                 "  --ui-ignore <path> [gitignore|exclude] [file|ext|dir]\n"
                 "                      append an ignore pattern and untrack the path,\n"
                 "                      reporting the pattern and whether status still lists it\n"
                 "  --ui-coauthor <name> <email>\n"
                 "                      build a Co-authored-by trailer onto a sample subject\n"
                 "  --ui-commit-template\n"
                 "                      print the resolved commit.template, if any\n"
                 "  --ui-tool <get|set> <diff|merge> [name]\n"
                 "                      read or write the external diff/merge tool in the\n"
                 "                      repository's git config (empty name unsets)\n"
                 "  --ui-difftool <base> <target> <path>\n"
                 "                      run the configured external diff tool (`-` for an\n"
                 "                      empty revision) and report git's output\n"
                 "  --ui-mergetool <path>\n"
                 "                      run the configured external merge tool on a path\n"
                 "  --ui-terminal\n"
                 "                      print the terminal command that would be launched for\n"
                 "                      the open repository, without launching it\n"
                 "  --ui-tabs <op> [args...]\n"
                 "                      list | new | open <path> | activate <n> | close <n> |\n"
                 "                      close-others <n> | close-all | rename <n> <alias>;\n"
                 "                      prints the tab list afterwards\n"
                 "  --ui-recent <list|add|named|clear> [args...]\n"
                 "                      read or change the recently-used repositories and\n"
                 "                      print the list\n"
                 "  --ui-start-page\n"
                 "                      print the rows the start page is showing\n"
                 "  --ui-create-repo <path> [branch]\n"
                 "                      create a repository and open it in a tab\n"
                 "  --ui-maintenance <gc|fsck|commit-graph|multi-pack-index|maintenance|optimize>\n"
                 "                      run repository maintenance and print git's output\n"
                 "  --ui-hooks <list|path|enable|disable|show> [name]\n"
                 "                      list git hooks and their state, resolve the hooks\n"
                 "                      directory, switch a hook on or off, or print its text\n"
                 "  --ui-sign <status|enable|disable|key|format> [value]\n"
                 "                      read or write the commit-signing configuration\n"
                 "  --ui-verify-signature <rev>\n"
                 "                      report git's verdict on a revision's signature\n"
                 "  --ui-rebase-editor <base> [plan]\n"
                 "                      print the interactive-rebase plan for <base>..HEAD\n"
                 "                      (oldest first), or run a plan given as comma-separated\n"
                 "                      action:sha entries in the final order. `root` as the base\n"
                 "                      replays from the first commit (`--root`). Pass the repo\n"
                 "                      path first — this eats words up to the next flag.\n"
                 "  --ui-rebase-amend [message]\n"
                 "                      amend the commit a stopped rebase sits on, with the\n"
                 "                      staged index; no message (or \"-\") keeps its own\n"
                 "  --ui-diff-mode <unified|split|file|blame>\n"
                 "                      set the diff viewer's mode; with --shot it applies to\n"
                 "                      the snapshot, otherwise it prints and exits\n"
                 "  --ui-compare <base> <target> [path]\n"
                 "                      list the files that differ between two revisions, or\n"
                 "                      one path's diff between them; target `//wip` compares\n"
                 "                      with the working tree\n"
                 "  --ui-compare-range <base> <target> [path]\n"
                 "                      the three-dot form: merge-base(base, target)..target\n"
                 "  --ui-restore-file <rev> <path> [staged]\n"
                 "                      restore a path from a revision into the working tree\n"
                 "                      (or the index, with \"staged\")\n"
                 "  --ui-blame <path> [rev]\n"
                 "                      print per-line blame of a path at a revision (or the\n"
                 "                      working tree)\n"
                 "  --ui-file-history <path>\n"
                 "                      print the commits that touched a path, newest first\n"
                 "  --ui-preflight <enable|disable>\n"
                 "  --ui-prevention <check|enable|disable>\n"
                 "                      turn the check-for-conflicts-before-merging step\n"
                 "                      on or off for this repository\n"
                 "  --ui-load-trace <seconds>  open the window as an interactive launch\n"
                 "                             does (background indexing included), leave\n"
                 "                             it idle and print the rows loaded and the\n"
                 "                             longest GUI-thread stall every half second\n"
                 "  --ui-reflog [limit]        print the reflog, newest first\n"
                 "  --ui-columns [rich|text|compact]  report or switch the graph columns\n"
                 "  --ui-timeline [none|hour|day|week|month|year]  report or group by date\n"
                 "  --ui-sparse [show|set|disable] [dir,dir]  report or change sparse checkout\n"
                 "  --ui-profile [list|use|add|remove] [arg]  identity profiles\n"
                 "  --ui-gitflow [show|init|start|finish] [kind|name]  the gitflow workflow\n"
                 "  --ui-notifications [list|clear]  recent notifications\n"
                 "  --ui-lfs [show|init|track|untrack|status|prune|fetch|pull|push] [arg]  Git LFS\n"
                 "  --ui-date-format [iso|european|us|relative]  the Date column format\n"
                 "  --ui-rerere <enable|disable|autoupdate-on|autoupdate-off|status|\n"
                 "               apply|forget> [path]\n"
                 "                      turn rerere on/off, apply recorded resolutions,\n"
                 "                      forget one, or report what it remembers\n"
                 "  --version, -V  print the version and exit\n"
                 "  --update-check  ask the release page for a newer build, print what an\n"
                 "                  update would install, and exit (downloads nothing)\n"
                 "  --update-install  the same, then download, verify and put the update in\n"
                 "                  place without restarting (a bundle is left staged)\n");
}

} // namespace

#ifdef _WIN32
/// A window application has no console, so the headless flags would print into
/// nothing when run from cmd or PowerShell. Output already redirected (a pipe, a
/// file) is left where it goes.
static void attachParentConsole() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
    SetConsoleOutputCP(CP_UTF8);
}
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    attachParentConsole();
#endif
    gk::Repository::initLibrary();

    bool probeMode = false;
    bool updateCheck = false;
    bool updateInstall = false;
    bool menuMode = false;
    std::string path;
    std::string shotPath;
    int graphWidth = 0;
    int selectRow = -1;
    std::string rebaseTarget;
    bool sweepAfterRebase = false;
    std::string uiRebaseTarget;
    int hoverRow = -1;
    int hoverExpandRow = -1;
    int uiHoverActRow = -1;
    std::string uiSelection;
    std::string uiSelectionOp;
    std::string uiContextRows;
    int uiContextRow = -1;
    int uiNavigateRow = -1;
    std::string uiNavigateDirection;
    std::string uiCommitOp;
    std::string uiCommitRef;
    std::string uiDropSource;
    std::string uiDropTarget;
    std::string uiDropPick;
    std::string uiCommitPanelMessage;
    std::string uiCommand;
    bool uiCommandRun = false;
    bool uiUndo = false;
    bool uiRedo = false;
    bool uiOplog = false;
    bool uiRebaseAmend = false;
    std::string uiRebaseAmendMessage;
    std::string uiDiffMode;
    std::string uiCompareBase;
    std::string uiCompareTarget;
    std::string uiComparePath;
    bool uiCompareMergeBase = false;
    std::string uiRestoreRev;
    std::string uiRestorePath;
    bool uiRestoreStaged = false;
    std::string uiBlamePath;
    std::string uiBlameRev;
    std::string uiFileHistoryPath;
    std::string uiStagePath;
    int uiStageHunk = -1;
    int uiStageLine = -1;
    bool uiStageIsLine = false;
    bool uiStageUnstage = false;
    bool uiStageDiscard = false;
    std::string uiStashOp;
    std::string uiStashArg;
    std::string uiSyncOp;
    std::string uiSyncArg;
    std::string uiSetUpstream;
    std::string uiAutoFetchOp;
    std::string uiAutoFetchArg;
    std::string uiPreferencesOp;
    QStringList uiPreferencesArgs;
    std::string uiKeymapOp;
    QStringList uiKeymapArgs;
    std::string uiSearchQuery;
    QStringList uiSearchArgs;
    std::string uiConflictOp;
    std::string uiConflictArg1;
    std::string uiConflictArg2;
    std::string uiMergePreviewA;
    std::string uiMergePreviewB;
    std::string uiSquashSource;
    std::string uiSquashTarget;
    std::string uiVisibilityOp;
    std::string uiVisibilityBranch;
    std::string uiWorktreeOp;
    QStringList uiWorktreeArgs;
    std::string uiSubmoduleOp;
    QStringList uiSubmoduleArgs;
    std::string uiGraphAnimOp;
    std::string uiGraphAnimArg;
    std::string uiBranchGroupOp;
    QStringList uiBranchGroupArgs;
    std::string uiApplyPatchPath;
    std::string uiCloneUrl;
    std::string uiCloneDest;
    QStringList uiCloneArgs;
    std::string uiIgnorePath;
    std::string uiIgnoreScope = "gitignore";
    std::string uiIgnoreKind = "file";
    std::string uiCoauthorName;
    std::string uiCoauthorEmail;
    bool uiCommitTemplate = false;
    std::string uiToolOp;
    std::string uiToolKind;
    std::string uiToolName;
    std::string uiDiffToolBase;
    std::string uiDiffToolTarget;
    std::string uiDiffToolPath;
    std::string uiMergeToolPath;
    bool uiTerminal = false;
    std::string uiTabsOp;
    QStringList uiTabsArgs;
    std::string uiRecentOp;
    QStringList uiRecentArgs;
    bool uiStartPage = false;
    std::string uiCreateRepoPath;
    std::string uiCreateRepoBranch;
    std::string uiMaintenanceOp;
    std::string uiHooksOp;
    QStringList uiHooksArgs;
    std::string uiSignOp;
    QStringList uiSignArgs;
    std::string uiVerifySignature;
    std::string uiRebaseEditorBase;
    QStringList uiRebaseEditorArgs;
    std::string uiPreflightOp;
    std::string uiPreventionOp;
    int uiLoadTraceSeconds = 0;
    int uiReflogLimit = 0;
    std::string uiColumnsOp;
    std::string uiTimelineOp;
    std::string uiSparseOp;
    std::string uiSparseArg;
    std::string uiProfileOp;
    std::string uiProfileArg;
    std::string uiGitflowOp;
    std::string uiGitflowArg;
    std::string uiNotificationsOp;
    std::string uiLfsOp;
    std::string uiLfsArg;
    std::string uiDateFormatOp;
    bool uiDateFormatRequested = false;
    std::string uiRerereOp;
    std::string uiRerereArg;
    bool fileView = false;
    bool openDiff = false;
    std::string themeArg;
    int zoomArg = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--probe") {
            probeMode = true;
        } else if (arg == "--menu") {
            menuMode = true;
        } else if (arg == "--shot" && i + 1 < argc) {
            shotPath = argv[++i];
        } else if (arg == "--graph-width" && i + 1 < argc) {
            graphWidth = std::atoi(argv[++i]);
        } else if (arg == "--select-row" && i + 1 < argc) {
            selectRow = std::atoi(argv[++i]);
        } else if (arg == "--file-view") {
            fileView = true;
        } else if (arg == "--rebase-onto" && i + 1 < argc) {
            rebaseTarget = argv[++i];
        } else if (arg == "--ui-selection" && i + 1 < argc) {
            uiSelection = argv[++i];
            // Only a known operation name is taken as one. Anything else is the repository
            // path that follows, and swallowing it would leave the window with no repo.
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (next == "drop" || next == "squash" || next == "cherry-pick") {
                    uiSelectionOp = argv[++i];
                }
            }
        } else if (arg == "--ui-context-menu" && i + 2 < argc) {
            uiContextRows = argv[++i];
            uiContextRow = std::atoi(argv[++i]);
        } else if (arg == "--hover-row" && i + 1 < argc) {
            hoverRow = std::atoi(argv[++i]);
        } else if (arg == "--ui-hover-expand" && i + 1 < argc) {
            hoverExpandRow = std::atoi(argv[++i]);
        } else if (arg == "--ui-hover-act" && i + 1 < argc) {
            uiHoverActRow = std::atoi(argv[++i]);
        } else if (arg == "--ui-navigate" && i + 2 < argc) {
            uiNavigateRow = std::atoi(argv[++i]);
            uiNavigateDirection = argv[++i];
        } else if (arg == "--ui-commit-op" && i + 2 < argc) {
            uiCommitOp = argv[++i];
            uiCommitRef = argv[++i];
        } else if (arg == "--ui-branch-drop" && i + 2 < argc) {
            uiDropSource = argv[++i];
            uiDropTarget = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                uiDropPick = argv[++i];
            }
        } else if (arg == "--ui-commit-panel" && i + 1 < argc) {
            uiCommitPanelMessage = argv[++i];
        } else if (arg == "--ui-command" && i + 1 < argc) {
            uiCommand = argv[++i];
            if (i + 1 < argc && std::string(argv[i + 1]) == "run") {
                uiCommandRun = true;
                ++i;
            }
        } else if (arg == "--ui-rebase-onto" && i + 1 < argc) {
            uiRebaseTarget = argv[++i];
        } else if (arg == "--ui-undo") {
            uiUndo = true;
        } else if (arg == "--ui-redo") {
            uiRedo = true;
        } else if (arg == "--ui-oplog") {
            uiOplog = true;
        } else if (arg == "--ui-stage-hunk" && i + 2 < argc) {
            uiStagePath = argv[++i];
            uiStageHunk = std::atoi(argv[++i]);
            if (i + 1 < argc && std::string(argv[i + 1]) == "unstage") {
                uiStageUnstage = true;
                ++i;
            }
        } else if (arg == "--ui-stage-line" && i + 3 < argc) {
            uiStagePath = argv[++i];
            uiStageHunk = std::atoi(argv[++i]);
            uiStageLine = std::atoi(argv[++i]);
            uiStageIsLine = true;
            if (i + 1 < argc && std::string(argv[i + 1]) == "unstage") {
                uiStageUnstage = true;
                ++i;
            }
        } else if (arg == "--ui-discard-hunk" && i + 2 < argc) {
            uiStagePath = argv[++i];
            uiStageHunk = std::atoi(argv[++i]);
            uiStageDiscard = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                uiStageLine = std::atoi(argv[++i]);
            }
        } else if (arg == "--ui-stash" && i + 1 < argc) {
            uiStashOp = argv[++i];
            // The optional argument is taken only when a repository path still
            // follows it, so `--ui-stash list /repo` does not eat the path.
            if (i + 2 < argc) {
                uiStashArg = argv[++i];
            }
        } else if (arg == "--ui-sync" && i + 1 < argc) {
            uiSyncOp = argv[++i];
            if (i + 2 < argc) {
                uiSyncArg = argv[++i];
            }
        } else if (arg == "--ui-set-upstream" && i + 1 < argc) {
            uiSetUpstream = argv[++i];
        } else if (arg == "--ui-autofetch" && i + 1 < argc) {
            uiAutoFetchOp = argv[++i];
            if (i + 2 < argc) {
                uiAutoFetchArg = argv[++i];
            }
        } else if (arg == "--ui-preferences" && i + 1 < argc) {
            uiPreferencesOp = argv[++i];
            // The key and its value, up to the next flag. Pass the repository path
            // first, or it would be read as one of them.
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                uiPreferencesArgs << QString::fromStdString(argv[++i]);
            }
        } else if (arg == "--ui-keymap" && i + 1 < argc) {
            uiKeymapOp = argv[++i];
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                uiKeymapArgs << QString::fromStdString(argv[++i]);
            }
        } else if (arg == "--ui-search" && i + 1 < argc) {
            uiSearchQuery = argv[++i];
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                uiSearchArgs << QString::fromStdString(argv[++i]);
            }
        } else if (arg == "--ui-conflict" && i + 1 < argc) {
            uiConflictOp = argv[++i];
            // Up to two arguments. When the repository path came first there is
            // nothing to reserve for it; when it comes last, leave it a token.
            // This is the guard --ui-stash uses, made aware of the path order.
            const bool repoKnown = !path.empty();
            if (i + 1 < argc && argv[i + 1][0] != '-' && (repoKnown || i + 2 < argc)) {
                uiConflictArg1 = argv[++i];
                if (i + 1 < argc && argv[i + 1][0] != '-' && (repoKnown || i + 2 < argc)) {
                    uiConflictArg2 = argv[++i];
                }
            }
        } else if (arg == "--ui-merge-preview" && i + 2 < argc) {
            uiMergePreviewA = argv[++i];
            uiMergePreviewB = argv[++i];
        } else if (arg == "--ui-squash-merge" && i + 2 < argc) {
            uiSquashSource = argv[++i];
            uiSquashTarget = argv[++i];
        } else if (arg == "--ui-visibility" && i + 1 < argc) {
            uiVisibilityOp = argv[++i];
            // The branch is optional: `show` needs none. A following path starts
            // with a letter, so it must be given first, not after the operation.
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                uiVisibilityBranch = argv[++i];
            }
        } else if (arg == "--ui-worktree" && i + 1 < argc) {
            uiWorktreeOp = argv[++i];
            // The operation's own arguments, up to the next flag. The repository
            // path must come first, or it would be read as one of them.
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                uiWorktreeArgs << QString::fromStdString(argv[++i]);
            }
        } else if (arg == "--ui-submodule" && i + 1 < argc) {
            uiSubmoduleOp = argv[++i];
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                uiSubmoduleArgs << QString::fromStdString(argv[++i]);
            }
        } else if (arg == "--ui-graph-animation" && i + 1 < argc) {
            uiGraphAnimOp = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                uiGraphAnimArg = argv[++i];
            }
        } else if (arg == "--ui-branch-group" && i + 1 < argc) {
            uiBranchGroupOp = argv[++i];
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                uiBranchGroupArgs << QString::fromStdString(argv[++i]);
            }
        } else if (arg == "--ui-apply-patch" && i + 1 < argc) {
            uiApplyPatchPath = argv[++i];
        } else if (arg == "--ui-clone" && i + 1 < argc) {
            // The destination must come before the flags, or it would be read as one.
            uiCloneUrl = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                uiCloneDest = argv[++i];
            }
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                uiCloneArgs << QString::fromStdString(argv[++i]);
            }
        } else if (arg == "--ui-ignore" && i + 1 < argc) {
            // <path> [gitignore|exclude] [file|ext|dir]; a following repository path
            // is consumed before the flag, so the two optionals are unambiguous.
            uiIgnorePath = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                uiIgnoreScope = argv[++i];
            }
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                uiIgnoreKind = argv[++i];
            }
        } else if (arg == "--ui-coauthor" && i + 2 < argc) {
            uiCoauthorName = argv[++i];
            uiCoauthorEmail = argv[++i];
        } else if (arg == "--ui-commit-template") {
            uiCommitTemplate = true;
        } else if (arg == "--ui-tool" && i + 2 < argc) {
            uiToolOp = argv[++i];
            uiToolKind = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                uiToolName = argv[++i];
            }
        } else if (arg == "--ui-difftool" && i + 3 < argc) {
            // A `-` stands for an empty revision: `--ui-difftool - - path` diffs the
            // index against the working tree.
            uiDiffToolBase = argv[++i];
            uiDiffToolTarget = argv[++i];
            uiDiffToolPath = argv[++i];
            if (uiDiffToolBase == "-") {
                uiDiffToolBase.clear();
            }
            if (uiDiffToolTarget == "-") {
                uiDiffToolTarget.clear();
            }
        } else if (arg == "--ui-mergetool" && i + 1 < argc) {
            uiMergeToolPath = argv[++i];
        } else if (arg == "--ui-terminal") {
            uiTerminal = true;
        } else if (arg == "--ui-tabs" && i + 1 < argc) {
            // <list|open|activate|close|rename> [args…]. Like the other tab hooks,
            // it eats every following word that is not a flag, so pass the path last.
            uiTabsOp = argv[++i];
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                uiTabsArgs << QString::fromStdString(argv[++i]);
            }
        } else if (arg == "--ui-recent" && i + 1 < argc) {
            uiRecentOp = argv[++i];
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                uiRecentArgs << QString::fromStdString(argv[++i]);
            }
        } else if (arg == "--ui-start-page") {
            uiStartPage = true;
        } else if (arg == "--ui-create-repo" && i + 1 < argc) {
            uiCreateRepoPath = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                uiCreateRepoBranch = argv[++i];
            }
        } else if (arg == "--ui-maintenance" && i + 1 < argc) {
            uiMaintenanceOp = argv[++i];
        } else if (arg == "--ui-hooks" && i + 1 < argc) {
            uiHooksOp = argv[++i];
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                uiHooksArgs << QString::fromStdString(argv[++i]);
            }
        } else if (arg == "--ui-sign" && i + 1 < argc) {
            uiSignOp = argv[++i];
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                uiSignArgs << QString::fromStdString(argv[++i]);
            }
        } else if (arg == "--ui-verify-signature" && i + 1 < argc) {
            uiVerifySignature = argv[++i];
        } else if (arg == "--ui-rebase-editor" && i + 1 < argc) {
            uiRebaseEditorBase = argv[++i];
            // The optional plan spec is comma-separated and holds no spaces, but a
            // following repository path must still be passed first — this eats every
            // word up to the next flag, like --ui-branch-drop.
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                uiRebaseEditorArgs << QString::fromStdString(argv[++i]);
            }
        } else if (arg == "--ui-rebase-amend") {
            uiRebaseAmend = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                uiRebaseAmendMessage = argv[++i];
            }
        } else if (arg == "--ui-diff-mode" && i + 1 < argc) {
            uiDiffMode = argv[++i];
        } else if (arg == "--ui-compare" && i + 2 < argc) {
            uiCompareBase = argv[++i];
            uiCompareTarget = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                uiComparePath = argv[++i];
            }
        } else if (arg == "--ui-compare-range" && i + 2 < argc) {
            uiCompareMergeBase = true;
            uiCompareBase = argv[++i];
            uiCompareTarget = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                uiComparePath = argv[++i];
            }
        } else if (arg == "--ui-restore-file" && i + 2 < argc) {
            uiRestoreRev = argv[++i];
            uiRestorePath = argv[++i];
            if (i + 1 < argc && std::string(argv[i + 1]) == "staged") {
                uiRestoreStaged = true;
                ++i;
            }
        } else if (arg == "--ui-blame" && i + 1 < argc) {
            uiBlamePath = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                uiBlameRev = argv[++i];
            }
        } else if (arg == "--ui-file-history" && i + 1 < argc) {
            uiFileHistoryPath = argv[++i];
        } else if (arg == "--ui-preflight" && i + 1 < argc) {
            uiPreflightOp = argv[++i];
        } else if (arg == "--ui-prevention" && i + 1 < argc) {
            uiPreventionOp = argv[++i];
        } else if (arg == "--ui-load-trace" && i + 1 < argc) {
            uiLoadTraceSeconds = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--ui-reflog") {
            // The optional limit is taken only when it is a number: a bare
            // "--ui-reflog /repo" must not swallow the path as the limit.
            uiReflogLimit = 500;
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (!next.empty() && next.find_first_not_of("0123456789") == std::string::npos) {
                    uiReflogLimit = std::atoi(argv[++i]);
                }
            }
        } else if (arg == "--ui-columns") {
            // `show` is the default; the mode words are taken only when they are
            // one of the three, so a following repository path is never eaten.
            uiColumnsOp = "show";
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (next == "rich" || next == "text" || next == "compact" || next == "show") {
                    uiColumnsOp = argv[++i];
                }
            }
        } else if (arg == "--ui-timeline") {
            uiTimelineOp = "show";
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (next == "none" || next == "hour" || next == "day" || next == "week" ||
                    next == "month" || next == "year" || next == "show") {
                    uiTimelineOp = argv[++i];
                }
            }
        } else if (arg == "--ui-sparse") {
            uiSparseOp = "show";
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (next == "show" || next == "set" || next == "disable") {
                    uiSparseOp = argv[++i];
                    // The directory list is taken when it is a comma list or a bare
                    // name with no slash; a path ("/repo", "a/b") is left alone so a
                    // bare "--ui-sparse set /repo" does not eat it.
                    if (uiSparseOp == "set" && i + 1 < argc) {
                        const std::string dirs = argv[i + 1];
                        const bool bareName = dirs.find('/') == std::string::npos &&
                                              dirs != "." && dirs != "..";
                        if (dirs.find(',') != std::string::npos || bareName) {
                            uiSparseArg = argv[++i];
                        }
                    }
                }
            }
        } else if (arg == "--ui-date-format") {
            uiDateFormatRequested = true;
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (next == "iso" || next == "european" || next == "us" || next == "relative") {
                    uiDateFormatOp = argv[++i];
                }
            }
        } else if (arg == "--ui-lfs") {
            uiLfsOp = "show";
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                const std::vector<std::string> known{
                    "show",     "status", "version", "init",  "track", "untrack",
                    "checkout", "prune",  "fetch",   "pull",  "push",  "push-all"};
                if (std::find(known.begin(), known.end(), next) != known.end()) {
                    uiLfsOp = argv[++i];
                    // A pattern for track/untrack, taken when it is not a path (no
                    // leading slash), so the repository argument is never eaten.
                    if ((uiLfsOp == "track" || uiLfsOp == "untrack") && i + 1 < argc) {
                        const std::string pattern = argv[i + 1];
                        if (!pattern.empty() && pattern.front() != '-' && pattern.front() != '/') {
                            uiLfsArg = argv[++i];
                        }
                    }
                }
            }
        } else if (arg == "--ui-notifications") {
            uiNotificationsOp = "list";
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (next == "list" || next == "clear") {
                    uiNotificationsOp = argv[++i];
                }
            }
        } else if (arg == "--ui-gitflow") {
            uiGitflowOp = "show";
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (next == "show" || next == "init" || next == "start" || next == "finish") {
                    uiGitflowOp = argv[++i];
                    // A pipe-separated record; a bare path is left as the repository.
                    if (i + 1 < argc && std::string(argv[i + 1]).find('|') != std::string::npos) {
                        uiGitflowArg = argv[++i];
                    }
                }
            }
        } else if (arg == "--ui-profile") {
            uiProfileOp = "list";
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (next == "list" || next == "use" || next == "add" || next == "remove" ||
                    next == "config") {
                    uiProfileOp = argv[++i];
                    // A name (no slash) or an `add` record (pipe-separated); a path is
                    // left alone so the repository argument is never eaten.
                    if ((uiProfileOp == "use" || uiProfileOp == "remove") && i + 1 < argc) {
                        const std::string name = argv[i + 1];
                        if (name.find('/') == std::string::npos && name != "." && name != "..") {
                            uiProfileArg = argv[++i];
                        }
                    } else if (uiProfileOp == "add" && i + 1 < argc) {
                        const std::string record = argv[i + 1];
                        if (record.find('|') != std::string::npos) {
                            uiProfileArg = argv[++i];
                        }
                    }
                }
            }
        } else if (arg == "--ui-rerere" && i + 1 < argc) {
            uiRerereOp = argv[++i];
            // The optional path is taken only when a repository path still follows.
            if (i + 2 < argc) {
                uiRerereArg = argv[++i];
            }
        } else if (arg == "--sweep") {
            sweepAfterRebase = true;
        } else if (arg == "--open-diff") {
            openDiff = true;
        } else if (arg == "--theme" && i + 1 < argc) {
            themeArg = argv[++i];
        } else if (arg == "--zoom" && i + 1 < argc) {
            zoomArg = std::atoi(argv[++i]);
        } else if (arg == "--version" || arg == "-V") {
            std::printf("GitMycelium %s\n",
                        gk::update::describeVersion(gk::update::currentVersion(),
                                                    gk::update::currentBuild())
                            .toUtf8()
                            .constData());
            return 0;
        } else if (arg == "--update-check") {
            updateCheck = true;
        } else if (arg == "--update-install") {
            updateCheck = true;
            updateInstall = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            printUsage();
            return 2;
        } else {
            path = arg;
        }
    }

    if (!rebaseTarget.empty()) {
        if (path.empty()) {
            path = ".";
        }
        return rebaseHeadless(path, QString::fromStdString(rebaseTarget), sweepAfterRebase);
    }

    if (probeMode) {
        if (path.empty()) {
            path = ".";
        }
        return probe(path);
    }

    QApplication app(argc, argv);
    // The organization and application names are the *settings* identity: they are
    // the directory and file under `~/.config`, and changing either one reads as a
    // reset of every preference the reader has. They stay as they were shipped. The
    // name a person reads is the display name, which is what dialogs, the window
    // manager and the About box use.
    QCoreApplication::setOrganizationName(QStringLiteral("gitmycelium"));
    QCoreApplication::setApplicationName(QStringLiteral("gitmycelium"));
#ifdef Q_OS_WIN
    // A file under %APPDATA%, not the registry: keys carry repository paths, which the
    // registry would compare without case and nest at every slash.
    QSettings::setDefaultFormat(QSettings::IniFormat);
#endif
    app.setApplicationDisplayName(QStringLiteral("GitMycelium"));
    QCoreApplication::setApplicationVersion(QStringLiteral(GMC_VERSION));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/gitmycelium.svg")));
    if (updateCheck) {
        return updateCheckHeadless(updateInstall);
    }
    // The theme and the zoom are saved globally. The command line can override
    // them for one run — a light snapshot in CI must not rewrite the reader's
    // profile, so a `--theme`/`--zoom` argument applies here and is not stored.
    gk::loadSavedAppearance();
    if (!themeArg.empty()) {
        gk::theme::setMode(QString::fromStdString(themeArg)
                                   .compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0
                               ? gk::theme::ThemeMode::Light
                               : gk::theme::ThemeMode::Dark);
    }
    if (zoomArg >= 0) {
        gk::theme::setZoomPercent(zoomArg);
    }

    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    gk::applyApplicationTheme(&app);

    // Every `--ui-*` check and every snapshot runs with no one at the machine: a
    // confirmation dialog would wait forever, and waiting forever looks exactly like
    // an operation that did nothing. Decided once, before the repository is opened,
    // so nothing writes to the reader's profile on the way in.
    const bool headlessRun =
        !uiCommitOp.empty() || !uiSelection.empty() || uiNavigateRow >= 0 || hoverRow >= 0 ||
        hoverExpandRow >= 0 ||
        uiHoverActRow >= 0 ||
        !uiContextRows.empty() || !uiCommitPanelMessage.empty() || !uiCommand.empty() ||
        uiUndo || uiRedo || uiOplog || !uiStagePath.empty() || !uiStashOp.empty() ||
        !uiDropSource.empty() ||
        !uiSyncOp.empty() || !uiSetUpstream.empty() || !uiAutoFetchOp.empty() ||
        !uiConflictOp.empty() || !uiMergePreviewA.empty() || !uiSquashSource.empty() ||
        !uiVisibilityOp.empty() || !uiWorktreeOp.empty() || !uiSubmoduleOp.empty() ||
        !uiGraphAnimOp.empty() || !uiBranchGroupOp.empty() || !uiApplyPatchPath.empty() ||
        !uiMaintenanceOp.empty() || !uiHooksOp.empty() || !uiSignOp.empty() ||
        !uiVerifySignature.empty() || !uiRebaseEditorBase.empty() || uiRebaseAmend ||
        !uiCloneUrl.empty() || !uiIgnorePath.empty() || !uiCoauthorName.empty() ||
        uiCommitTemplate || !uiToolOp.empty() || !uiDiffToolPath.empty() ||
        !uiMergeToolPath.empty() || uiTerminal || !uiTabsOp.empty() ||
        !uiRecentOp.empty() || uiStartPage || !uiCreateRepoPath.empty() ||
        !uiDiffMode.empty() || !uiCompareBase.empty() || !uiRestoreRev.empty() ||
        !uiBlamePath.empty() || !uiFileHistoryPath.empty() ||
        !uiPreflightOp.empty() || !uiPreventionOp.empty() ||
        uiReflogLimit > 0 || !uiColumnsOp.empty() || !uiTimelineOp.empty() ||
        !uiSparseOp.empty() ||
        !uiProfileOp.empty() ||
        !uiGitflowOp.empty() ||
        !uiNotificationsOp.empty() ||
        !uiLfsOp.empty() ||
        uiDateFormatRequested ||
        !uiRerereOp.empty() ||
        !uiPreferencesOp.empty() ||
        !uiKeymapOp.empty() || !uiSearchQuery.empty();

    gk::MainWindow window;
    window.setTabsPersistenceEnabled(!headlessRun && shotPath.empty() && uiLoadTraceSeconds == 0);
    window.setBackgroundIndexingEnabled(!headlessRun && shotPath.empty());
    window.setUpdateChecksEnabled(!headlessRun && shotPath.empty() && uiLoadTraceSeconds == 0);
    window.setAsyncReadsEnabled(!headlessRun && shotPath.empty());
    if (!path.empty()) {
        window.openRepository(QString::fromStdString(path));
    } else if (!headlessRun && shotPath.empty()) {
        window.restoreTabs();
    }
    window.resize(1400, 880);
    window.show();

    if (!uiRebaseTarget.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiRebaseTarget] {
            const QString branch = QString::fromStdString(window.currentBranchName());
            const QStringList report =
                window.rebaseForDiagnostics(branch, QString::fromStdString(uiRebaseTarget));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiContextRows.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiContextRows, uiContextRow] {
            QVector<int> rows;
            for (const QString& part :
                 QString::fromStdString(uiContextRows).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                rows.push_back(part.toInt());
            }
            const auto probe = [&window, rows, uiContextRow] {
                for (const QString& line : window.contextMenuForDiagnostics(rows, uiContextRow)) {
                    std::printf("%s\n", line.toUtf8().constData());
                }
                QCoreApplication::quit();
            };
            if (window.commitsAreLoaded()) {
                probe();
            } else {
                window.whenCommitsAreLoaded(probe);
            }
        });
    }

    if (!uiSelection.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiSelection, uiSelectionOp] {
            QVector<int> rows;
            for (const QString& part :
                 QString::fromStdString(uiSelection).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                rows.push_back(part.toInt());
            }
            const auto probe = [&window, rows, uiSelectionOp] {
                const QStringList report = uiSelectionOp.empty()
                                               ? window.describeSelection(rows)
                                               : window.runSelectionForDiagnostics(
                                                     rows, QString::fromStdString(uiSelectionOp));
                for (const QString& line : report) {
                    std::printf("%s\n", line.toUtf8().constData());
                }
                QCoreApplication::quit();
            };
            if (window.commitsAreLoaded()) {
                probe();
            } else {
                window.whenCommitsAreLoaded(probe);
            }
        });
    }

    // With a snapshot asked for as well, the run continues — quitting here would end
    // the process before the grab.
    if (hoverRow >= 0 && shotPath.empty()) {
        QTimer::singleShot(1200, &window, [&window, hoverRow] {
            for (const QString& line : window.describeHoverRow(hoverRow)) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (hoverExpandRow >= 0 && shotPath.empty()) {
        QTimer::singleShot(1200, &window, [&window, hoverExpandRow] {
            for (const QString& line : window.hoverExpandForDiagnostics(hoverExpandRow)) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (uiHoverActRow >= 0) {
        QTimer::singleShot(1200, &window, [&window, uiHoverActRow] {
            for (const QString& line : window.activateHoverGhostForDiagnostics(uiHoverActRow)) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (uiNavigateRow >= 0 && !uiNavigateDirection.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiNavigateRow, uiNavigateDirection] {
            const QStringList report = window.navigateForDiagnostics(
                uiNavigateRow, QString::fromStdString(uiNavigateDirection));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (headlessRun) {
        window.setHeadless(true);
    }

    // A headless run turns the graph's motion off so nothing depends on the clock;
    // the animation check is the one that has to see it, so it turns it back on.
    if (!uiGraphAnimOp.empty()) {
        window.setGraphAnimationsEnabled(true);
    }

    if (!uiCommitOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiCommitOp, uiCommitRef] {
            const QStringList report = window.runCommitMenuEntry(
                QString::fromStdString(uiCommitOp), QString::fromStdString(uiCommitRef));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiDropSource.empty() && !uiDropTarget.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiDropSource, uiDropTarget, uiDropPick] {
            const QStringList report = window.simulateBranchDrop(
                QString::fromStdString(uiDropSource), QString::fromStdString(uiDropTarget),
                QString::fromStdString(uiDropPick));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiCommitPanelMessage.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiCommitPanelMessage] {
            const QStringList report = window.commitPanelForDiagnostics(
                QStringList(), QString::fromStdString(uiCommitPanelMessage));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiCommand.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiCommand, uiCommandRun] {
            const QStringList report = window.commandPaletteForDiagnostics(
                QString::fromStdString(uiCommand), uiCommandRun);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (uiUndo) {
        QTimer::singleShot(1200, &window, [&window] {
            for (const QString& line : window.undoForDiagnostics()) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }
    if (uiRedo) {
        QTimer::singleShot(1200, &window, [&window] {
            for (const QString& line : window.redoForDiagnostics()) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }
    if (uiOplog) {
        QTimer::singleShot(1200, &window, [&window] {
            for (const QString& line : window.oplogForDiagnostics()) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiStagePath.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiStagePath, uiStageHunk, uiStageLine,
                                          uiStageIsLine, uiStageUnstage, uiStageDiscard] {
            const QString path = QString::fromStdString(uiStagePath);
            const QStringList report =
                uiStageDiscard
                    ? window.discardHunkForDiagnostics(path, uiStageHunk, uiStageLine)
                    : (uiStageIsLine
                           ? window.stageLineForDiagnostics(path, uiStageHunk, uiStageLine,
                                                            uiStageUnstage)
                           : window.stageHunkForDiagnostics(path, uiStageHunk, uiStageUnstage));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    // Stashing is durable, so a one-shot process is enough to check it — unlike
    // undo, which lives in the session.
    if (!uiStashOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiStashOp, uiStashArg] {
            const QStringList report = window.stashForDiagnostics(
                QString::fromStdString(uiStashOp), QString::fromStdString(uiStashArg));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiSyncOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiSyncOp, uiSyncArg] {
            const QStringList report = window.syncForDiagnostics(
                QString::fromStdString(uiSyncOp), QString::fromStdString(uiSyncArg));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiSetUpstream.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiSetUpstream] {
            const QStringList report =
                window.setUpstreamForDiagnostics(QString::fromStdString(uiSetUpstream));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiAutoFetchOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiAutoFetchOp, uiAutoFetchArg] {
            QStringList remotes;
            const QString arg = QString::fromStdString(uiAutoFetchArg);
            if (!arg.isEmpty()) {
                remotes = arg.split(QLatin1Char(','), Qt::SkipEmptyParts);
            }
            const QStringList report = window.autoFetchForDiagnostics(
                QString::fromStdString(uiAutoFetchOp), remotes);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiConflictOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiConflictOp, uiConflictArg1, uiConflictArg2] {
            QStringList args;
            if (!uiConflictArg1.empty()) {
                args << QString::fromStdString(uiConflictArg1);
            }
            if (!uiConflictArg2.empty()) {
                args << QString::fromStdString(uiConflictArg2);
            }
            const QStringList report = window.conflictForDiagnostics(
                QString::fromStdString(uiConflictOp), args);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiMergePreviewA.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiMergePreviewA, uiMergePreviewB] {
            const QStringList report = window.mergePreviewForDiagnostics(
                QString::fromStdString(uiMergePreviewA),
                QString::fromStdString(uiMergePreviewB));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiSquashSource.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiSquashSource, uiSquashTarget] {
            const QStringList report = window.squashMergeForDiagnostics(
                QString::fromStdString(uiSquashSource),
                QString::fromStdString(uiSquashTarget));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiVisibilityOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiVisibilityOp, uiVisibilityBranch] {
            const QStringList report = window.visibilityForDiagnostics(
                QString::fromStdString(uiVisibilityOp),
                QString::fromStdString(uiVisibilityBranch));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiWorktreeOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiWorktreeOp, uiWorktreeArgs] {
            const QStringList report =
                window.worktreeForDiagnostics(QString::fromStdString(uiWorktreeOp), uiWorktreeArgs);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiSubmoduleOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiSubmoduleOp, uiSubmoduleArgs] {
            const QStringList report = window.submoduleForDiagnostics(
                QString::fromStdString(uiSubmoduleOp), uiSubmoduleArgs);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiGraphAnimOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiGraphAnimOp, uiGraphAnimArg] {
            const QStringList report = window.runGraphAnimationOp(
                QString::fromStdString(uiGraphAnimOp), QString::fromStdString(uiGraphAnimArg));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
        });
        QTimer::singleShot(2000, &window, [&window] {
            const QStringList report = window.graphAnimationReport();
            std::printf("transitions: %d\n", int(report.size()));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiBranchGroupOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiBranchGroupOp, uiBranchGroupArgs] {
            const QStringList report = window.branchGroupForDiagnostics(
                QString::fromStdString(uiBranchGroupOp), uiBranchGroupArgs);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiApplyPatchPath.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiApplyPatchPath] {
            const QStringList report =
                window.patchForDiagnostics(QString::fromStdString(uiApplyPatchPath));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiCloneUrl.empty() && !uiCloneDest.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiCloneUrl, uiCloneDest, uiCloneArgs] {
            const QStringList report = window.cloneForDiagnostics(
                QString::fromStdString(uiCloneUrl), QString::fromStdString(uiCloneDest),
                uiCloneArgs);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiIgnorePath.empty()) {
        QTimer::singleShot(1200, &window,
                           [&window, uiIgnorePath, uiIgnoreScope, uiIgnoreKind] {
                               const QStringList report = window.ignoreForDiagnostics(
                                   QString::fromStdString(uiIgnorePath),
                                   QString::fromStdString(uiIgnoreScope),
                                   QString::fromStdString(uiIgnoreKind));
                               for (const QString& line : report) {
                                   std::printf("%s\n", line.toUtf8().constData());
                               }
                               QCoreApplication::quit();
                           });
    }

    if (!uiCoauthorName.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiCoauthorName, uiCoauthorEmail] {
            const QStringList report = window.coauthorForDiagnostics(
                QString::fromStdString(uiCoauthorName), QString::fromStdString(uiCoauthorEmail));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (uiCommitTemplate) {
        QTimer::singleShot(1200, &window, [&window] {
            const QStringList report = window.commitTemplateForDiagnostics();
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiToolOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiToolOp, uiToolKind, uiToolName] {
            const QStringList report = window.externalToolForDiagnostics(
                QString::fromStdString(uiToolOp), QString::fromStdString(uiToolKind),
                QString::fromStdString(uiToolName));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiDiffToolPath.empty()) {
        QTimer::singleShot(1200, &window,
                           [&window, uiDiffToolBase, uiDiffToolTarget, uiDiffToolPath] {
                               const QStringList report = window.runExternalToolForDiagnostics(
                                   QStringLiteral("diff"), QString::fromStdString(uiDiffToolBase),
                                   QString::fromStdString(uiDiffToolTarget),
                                   QString::fromStdString(uiDiffToolPath));
                               for (const QString& line : report) {
                                   std::printf("%s\n", line.toUtf8().constData());
                               }
                               QCoreApplication::quit();
                           });
    }

    if (!uiMergeToolPath.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiMergeToolPath] {
            const QStringList report = window.runExternalToolForDiagnostics(
                QStringLiteral("merge"), QString(), QString(),
                QString::fromStdString(uiMergeToolPath));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (uiTerminal) {
        QTimer::singleShot(1200, &window, [&window] {
            const QStringList report = window.terminalForDiagnostics();
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiTabsOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiTabsOp, uiTabsArgs] {
            const QStringList report = window.tabsForDiagnostics(
                QString::fromStdString(uiTabsOp), uiTabsArgs);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiRecentOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiRecentOp, uiRecentArgs] {
            const QStringList report = window.recentForDiagnostics(
                QString::fromStdString(uiRecentOp), uiRecentArgs);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (uiStartPage) {
        QTimer::singleShot(1200, &window, [&window] {
            const QStringList report = window.startPageForDiagnostics();
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiCreateRepoPath.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiCreateRepoPath, uiCreateRepoBranch] {
            const QStringList report = window.createRepositoryForDiagnostics(
                QString::fromStdString(uiCreateRepoPath),
                QString::fromStdString(uiCreateRepoBranch));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiMaintenanceOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiMaintenanceOp] {
            const QStringList report =
                window.maintenanceForDiagnostics(QString::fromStdString(uiMaintenanceOp));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiHooksOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiHooksOp, uiHooksArgs] {
            const QStringList report = window.hooksForDiagnostics(
                QString::fromStdString(uiHooksOp), uiHooksArgs);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiSignOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiSignOp, uiSignArgs] {
            const QStringList report = window.signingForDiagnostics(
                QString::fromStdString(uiSignOp), uiSignArgs);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiVerifySignature.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiVerifySignature] {
            const QStringList report =
                window.verifySignatureForDiagnostics(QString::fromStdString(uiVerifySignature));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiPreferencesOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiPreferencesOp, uiPreferencesArgs] {
            const QString key = uiPreferencesArgs.value(0);
            const QString value = uiPreferencesArgs.value(1);
            const QStringList report = window.preferencesForDiagnostics(
                QString::fromStdString(uiPreferencesOp), key, value);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiKeymapOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiKeymapOp, uiKeymapArgs] {
            const QStringList report = window.keymapForDiagnostics(
                QString::fromStdString(uiKeymapOp), uiKeymapArgs);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiSearchQuery.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiSearchQuery, uiSearchArgs] {
            const QStringList report = window.searchForDiagnostics(
                QString::fromStdString(uiSearchQuery),
                uiSearchArgs.value(0, QStringLiteral("text")), uiSearchArgs.value(1));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiRebaseEditorBase.empty()) {
        QTimer::singleShot(1200, &window,
                           [&window, uiRebaseEditorBase, uiRebaseEditorArgs] {
                               const QStringList report = window.rebaseEditorForDiagnostics(
                                   QString::fromStdString(uiRebaseEditorBase), uiRebaseEditorArgs);
                               for (const QString& line : report) {
                                   std::printf("%s\n", line.toUtf8().constData());
                               }
                               QCoreApplication::quit();
                           });
    }

    if (uiRebaseAmend) {
        QTimer::singleShot(1200, &window, [&window, uiRebaseAmendMessage] {
            const QStringList report =
                window.rebaseAmendForDiagnostics(QString::fromStdString(uiRebaseAmendMessage));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiDiffMode.empty() && shotPath.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiDiffMode] {
            if (uiDiffMode == "split") {
                window.setDiffSplitView(true);
            } else if (uiDiffMode == "file") {
                window.setDiffFileView(true);
            } else if (uiDiffMode == "blame") {
                window.setDiffBlameView(true);
            } else {
                window.setDiffSplitView(false);
            }
            std::printf("diff mode: %s\n", uiDiffMode.c_str());
            QCoreApplication::quit();
        });
    }

    if (!uiCompareBase.empty()) {
        QTimer::singleShot(1200, &window,
                           [&window, uiCompareBase, uiCompareTarget, uiComparePath,
                            uiCompareMergeBase] {
                               const QStringList report = window.compareForDiagnostics(
                                   QString::fromStdString(uiCompareBase),
                                   QString::fromStdString(uiCompareTarget),
                                   QString::fromStdString(uiComparePath), uiCompareMergeBase);
                               for (const QString& line : report) {
                                   std::printf("%s\n", line.toUtf8().constData());
                               }
                               QCoreApplication::quit();
                           });
    }

    if (!uiRestoreRev.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiRestoreRev, uiRestorePath, uiRestoreStaged] {
            const QStringList report = window.restoreFileForDiagnostics(
                QString::fromStdString(uiRestoreRev),
                QString::fromStdString(uiRestorePath), uiRestoreStaged);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiBlamePath.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiBlamePath, uiBlameRev] {
            const QStringList report = window.blameForDiagnostics(
                QString::fromStdString(uiBlamePath), QString::fromStdString(uiBlameRev));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiFileHistoryPath.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiFileHistoryPath] {
            const QStringList report =
                window.fileHistoryForDiagnostics(QString::fromStdString(uiFileHistoryPath));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiPreflightOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiPreflightOp] {
            const bool on = uiPreflightOp == "enable";
            window.setPreflightEnabled(on);
            std::printf("preflight: %s\n", window.preflightEnabled() ? "on" : "off");
            QCoreApplication::quit();
        });
    }

    if (!uiPreventionOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiPreventionOp] {
            const QStringList report =
                window.preventionForDiagnostics(QString::fromStdString(uiPreventionOp));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (uiLoadTraceSeconds > 0) {
        // The window is left alone, as a reader who has just opened it would leave it:
        // anything that pages history or blocks the GUI thread on its own shows up as
        // rows growing or a stall. A 20 ms tick that arrives late measures the stall.
        window.resize(1600, 1000);
        window.show();
        auto* clock = new QElapsedTimer;
        clock->start();
        auto* lastTick = new qint64(0);
        auto* worstStall = new qint64(0);
        auto* tick = new QTimer(&window);
        QObject::connect(tick, &QTimer::timeout, &window, [clock, lastTick, worstStall] {
            const qint64 now = clock->elapsed();
            *worstStall = std::max(*worstStall, now - *lastTick - 20);
            *lastTick = now;
        });
        tick->start(20);
        auto* report = new QTimer(&window);
        QObject::connect(report, &QTimer::timeout, &window,
                         [&window, clock, worstStall, uiLoadTraceSeconds] {
                             std::printf("t=%6lldms %s stall=%lldms\n",
                                         static_cast<long long>(clock->elapsed()),
                                         window.loadStateForDiagnostics().toUtf8().constData(),
                                         static_cast<long long>(*worstStall));
                             std::fflush(stdout);
                             *worstStall = 0;
                             if (clock->elapsed() >= uiLoadTraceSeconds * 1000LL) {
                                 QCoreApplication::quit();
                             }
                         });
        report->start(500);
    }

    if (uiReflogLimit > 0) {
        QTimer::singleShot(1200, &window, [&window, uiReflogLimit] {
            const QStringList report = window.reflogForDiagnostics(uiReflogLimit);
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiColumnsOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiColumnsOp] {
            const QStringList report =
                window.columnsForDiagnostics(QString::fromStdString(uiColumnsOp));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiSparseOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiSparseOp, uiSparseArg] {
            const QStringList report = window.sparseForDiagnostics(
                QString::fromStdString(uiSparseOp), QString::fromStdString(uiSparseArg));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiProfileOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiProfileOp, uiProfileArg] {
            const QStringList report = window.profilesForDiagnostics(
                QString::fromStdString(uiProfileOp), QString::fromStdString(uiProfileArg));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiGitflowOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiGitflowOp, uiGitflowArg] {
            const QStringList report = window.gitflowForDiagnostics(
                QString::fromStdString(uiGitflowOp), QString::fromStdString(uiGitflowArg));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiNotificationsOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiNotificationsOp] {
            const QStringList report =
                window.notificationsForDiagnostics(QString::fromStdString(uiNotificationsOp));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiLfsOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiLfsOp, uiLfsArg] {
            const QStringList report = window.lfsForDiagnostics(
                QString::fromStdString(uiLfsOp), QString::fromStdString(uiLfsArg));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (uiDateFormatRequested) {
        QTimer::singleShot(1500, &window, [&window, uiDateFormatOp] {
            const QStringList report =
                window.dateFormatForDiagnostics(QString::fromStdString(uiDateFormatOp));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    if (!uiTimelineOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiTimelineOp] {
            const auto print = [&window](const QStringList& report) {
                for (const QString& line : report) {
                    std::printf("%s\n", line.toUtf8().constData());
                }
                QCoreApplication::quit();
            };
            if (uiTimelineOp == "show") {
                print(window.timelineForDiagnostics(QStringLiteral("show")));
                return;
            }
            // Switching restarts the history, so the report has to wait for the
            // first page — otherwise it would describe a model with no rows yet.
            window.timelineForDiagnostics(QString::fromStdString(uiTimelineOp));
            window.whenCommitsAreLoaded(
                [&window, print] { print(window.timelineForDiagnostics(QStringLiteral("show"))); });
        });
    }

    if (!uiRerereOp.empty()) {
        QTimer::singleShot(1200, &window, [&window, uiRerereOp, uiRerereArg] {
            const QStringList report = window.rerereForDiagnostics(
                QString::fromStdString(uiRerereOp), QString::fromStdString(uiRerereArg));
            for (const QString& line : report) {
                std::printf("%s\n", line.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    // Headless dump of the commit context menu. A popup menu cannot be driven
    // from a test, so its contents are printed instead of clicked.
    if (menuMode) {
        QTimer::singleShot(1200, &window, [&window] {
            const QStringList entries = window.commitMenuEntriesForFirstLabelledCommit();
            if (entries.isEmpty()) {
                std::fprintf(stderr, "no commit carries a branch label\n");
            }
            for (const QString& entry : entries) {
                std::printf("%s\n", entry.toUtf8().constData());
            }
            QCoreApplication::quit();
        });
    }

    // Deterministic UI snapshot for headless verification (CI / offscreen).
    // The second timer matters: QTextEdit lays its document out on a later
    // event-loop turn, so grabbing immediately after selecting a commit would
    // capture an empty diff pane.
    if (!shotPath.empty()) {
        // A frame caught mid-fade is not a snapshot of the UI, it is a snapshot of
        // the clock. The capture is deterministic with the motion stopped.
        window.setGraphAnimationsEnabled(false);
        QTimer::singleShot(1200, &window, [&window, shotPath, graphWidth, selectRow, fileView, openDiff, hoverRow, hoverExpandRow, uiDiffMode] {
            if (graphWidth > 0) {
                window.setGraphColumnWidth(graphWidth);
            }
            if (selectRow >= 0) {
                window.selectCommitRow(selectRow);
            }
            if (hoverRow >= 0) {
                window.describeHoverRow(hoverRow); // marks the row; the badge is painted
            }
            if (hoverExpandRow >= 0) {
                window.hoverExpandForDiagnostics(hoverExpandRow); // builds the reveal overlay
            }
            if (fileView || uiDiffMode == "file") {
                window.setDiffFileView(true);
            } else if (uiDiffMode == "split") {
                window.setDiffSplitView(true);
            } else if (uiDiffMode == "blame") {
                window.setDiffBlameView(true);
            } else if (uiDiffMode == "wrap") {
                window.setDiffWrap(true);
            } else if (uiDiffMode == "unified") {
                window.setDiffSplitView(false);
            }
            if (openDiff) {
                window.openFirstFileDiff();
            }
            QTimer::singleShot(600, &window, [&window, shotPath] {
                QApplication::processEvents();
                const bool ok = window.grab().save(QString::fromStdString(shotPath));
                std::fprintf(ok ? stdout : stderr, "screenshot %s: %s\n",
                             ok ? "saved" : "failed", shotPath.c_str());
                QCoreApplication::quit();
            });
        });
    }
    return app.exec();
}
