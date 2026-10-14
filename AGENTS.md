# AGENTS.md — gitmycelium

Guidance for AI coding agents (and humans) working in this repository.

## What this is

A native, fast Git client. **C++20 / Qt 6**, no Electron, no
web view. Target scale: a ~1,000,000-commit monorepo with a few thousand branches.
The two features that matter most are **merge-conflict resolution** and **branch
management** — weight decisions accordingly.

Design documents live in `docs/`. Read the relevant one before changing an area:

| Doc | Topic |
|:--|:--|
| `docs/02-architecture.md` | stack, module layout, threading, risks |
| `docs/03-merge-conflicts.md` | merge editor + conflict engine design |
| `docs/04-branches.md` | branches, commit graph, worktrees |
| `docs/05-performance-monorepo.md` | performance requirements and acceptance targets |

## Build, test, run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

# Headless repository dump (no GUI) — the primary way to verify the git layer
./build/src/gitmycelium --probe /path/to/repo

# Headless rebase — the flow that has produced the most surprises, and one that
# cannot be driven through the window without a display. `--sweep` runs the automatic
# conflict pass too, exactly as the window does after a rebase stops.
./build/src/gitmycelium --rebase-onto main --sweep /path/to/repo

# Deterministic UI snapshot, works without a display — use this to verify UI changes
QT_QPA_PLATFORM=offscreen ./build/src/gitmycelium --shot /tmp/ui.png /path/to/repo

# Interactive run
./build/src/gitmycelium /path/to/repo
```

Requirements: CMake ≥ 3.21, Qt 6 (Core/Gui/Widgets/Test), libgit2 (pkg-config),
a C++20 compiler.

## Commits

The user gives standing approval to **commit the result of a completed stage/wave**
once it is verified (warning-clean build, `ctest` green, headless checks pass) — no
separate confirmation is needed for that commit. This covers `git add` + `git commit`
of the stage's own changes only. It does **not** cover pushing, amending, rebasing,
resetting or any other history rewrite: still ask before those. Conventional Commits
format (`<type>: <description>`), a body explaining the why.

## Module map and dependency rules

```
src/core      domain types (Oid, Ref, Commit, Diff, Conflict…)
src/git       libgit2 RAII wrappers, and the typed `git` subprocesses
              (cli.{h,cpp} runGit, remote.{h,cpp} RemoteOps, stash.{h,cpp} StashOps,
              worktree/submodule/ref_meta/patch_ops/maintenance)
src/repo      RepoSession — the ONLY gateway to git state
src/graph     DAG lane layout (GraphLayout) — Qt-free
src/theme     palette, glyphs, embedded SVG icons
src/model     Qt item models (CommitModel and friends), word/diff helpers
src/update    release manifest + self-update (Qt Core/Network, no git)
src/ui        widgets + GraphDelegate (commit-graph painting)
src/app       main()
```

Hard rules:

- `core`, `git` **must not include Qt headers**. They use `std::` types only. Qt types
  (`QString`, `QVector`) enter at `repo`/`model` and above.
- `ui` never calls libgit2 or spawns `git` directly. Everything goes through `RepoSession`.
- The GUI thread must never execute a git operation. Blocking work goes to
  `QtConcurrent`/worker threads and reports back via signals.

## Threading contract (important)

libgit2 forbids concurrent calls on the same repository handle. Therefore:

- `RepoSession` guards every libgit2 call with an internal `QMutex`.
- Long reads (`requestCommits`) run on `QThreadPool::globalInstance()` and emit
  `commitsLoaded`, delivered as a queued signal to the receiver's thread.
- Every use of the handle happens under that mutex, and `~RepoSession` clears the
  handle before returning. `isOpen()` and `paths()` are lock-free (an atomic and the
  dirs captured at `open`): they are what the GUI asks most, and a locked answer waits
  behind whatever page of history a worker is walking.
- The GUI thread must not wait on that mutex either. A refresh reads one
  `SessionSnapshot` on a worker (`requestSnapshot` → `snapshotReady`); while it is
  current, `info`/`refs`/`status`/stashes/worktrees/… answer from it without the lock.
  Every write moves the epoch on (the write helpers do it under the lock), which
  retires the snapshot. File and commit reads the window needs go through
  `RepoSession::readAsync`.
- If you add a new git-backed read, route it through `RepoSession` and lock the mutex.
  If the window reads it on every refresh, put it in `SessionSnapshot` as well.
  Do not create a second `git_repository*` for the same path.

## Performance rules

- Never load a whole history. `CommitModel` pages via `canFetchMore`/`fetchMore`; keep it
  that way. Acceptance targets are in `docs/05-performance-monorepo.md`.
- Batch git CLI calls; never one process per branch or per file.
- Diff, blame and syntax highlighting are computed lazily, for visible ranges.
- Measure in Release: the Debug build's walk was 4–5× slower than `-O2`
  (`cmake -B build-rel -DCMAKE_BUILD_TYPE=Release`, then `build-rel/bench/gitmycelium-bench`
  and `scripts/make-big-repo.sh`).

## Known pitfalls

### History walk and paging

- Never load the whole history; page it on demand. `CommitModel::canFetchMore` is always false and `CommitGraphView::fetchIfNearEnd` asks for the next page when the reader is within two screens of the end (scrollbar `valueChanged`/`rangeChanged`, and queued after rows land, after `executeDelayedItemsLayout()`). QTreeView fetches on every layout and a landing page causes a layout, so a model that answered true walked the whole history while the window sat idle (303k rows in 27 s, stalls to 157 ms). Do not call `indexAt`/`visualRect` from a `rowsInserted` handler either: it forces that layout synchronously. `test_graph_paging`; `--ui-load-trace <s>` prints rows and stalls of an idle window.
- Every stash is a tip, not only `refs/stash`: the older ones are its reflog (`stashEntries`), walked from `walkStarts` and labelled `stash@{n}` (`RefKind::Other`, full name `refs/stash@{n}`; test a stash with `fullName` prefix `refs/stash`). A stash is not a branch: `buildCommitMenu` offers apply/pop/delete and copies only (`isStashCommit`), `startDrag` refuses its label, a drop on it is refused. A stash made with untracked files keeps them only in its third parent; `changedFiles`/`commitFileDiff` add that tree (`stashUntrackedTree`).
- A stash is drawn with its first parent only: its other parents (the index, the untracked files) are how git stores it. `TopoWalk::start` takes them as `firstParentOnly` and `Repository::commits` trims the stash's `parents`, so the helper commits never appear (a stray "index on …" commit beside every stash). `test_topo_walk` drops `refs/stash^2` from git's list.
- Do not build a revwalk per page: `Repository::commits()` caches the walk between pages (`takeFromWalk`, which holds one commit back so `hasMore` stays exact) and drops it via `invalidateCommitWalk()` whenever a ref or HEAD may have moved (session calls it from `guarded`/`recorded`, the window from `refreshPanels` for external changes). A walk per page was
  quadratic (~72 ms/page, 7.2 s for 100 pages on 100k/1000 branches). `tests/test_history_paging.cpp` pins contiguous pages vs `git rev-list`, an exact `hasMore` on a page boundary, and that a dropped walk sees a moved ref.
- The history walk is git's own `--topo-order`, not libgit2's sorted revwalk (which parses the whole history first — 3.4 s / 830 MB on a million commits). `git/topo_walk` is an independent implementation of git's incremental topological walk (same order, tie-breaks included); child counts are computed only down to the shallowest generation reached, read from the commit-graph (`git/commit_graph`,
  which maps the file or the split chain and refuses anything untrusted). Order is git's tie-break: every ref *by name*, then HEAD, stable-sorted by date (exactly `--all`). `test_topo_walk` compares row-for-row with `git rev-list --topo-order --all` (no graph, a graph, an out-of-date split chain, a broken file, the graph alone). A fixture whose commits share
  a second has its order decided by ref names, so graph tests stamp an increasing date (`stampNextCommit`); a commit outside the graph is read raw via `parseCommitHeader` (not `git_commit_lookup`, which caches every parsed commit — most of a gigabyte with no graph).
- No graph means a full walk: a session that read ≥10 000 commits outside the graph runs `git commit-graph write --reachable --split` once, on a worker, with no session lock. Opt-in via `RepoSession::setAutoCommitGraph`, turned on only by the interactive launch (`MainWindow::setBackgroundIndexingEnabled`); a test or `--shot` must not change the repository.
  `core.commitGraph=false` turns off read and write.
- A history page carries the generation it was asked for; `skip` is not enough. `CommitModel::reset` bumps `m_generation`; `requestCommits` returns it in `CommitPage::generation`; `onPage` drops anything else *before* touching `m_loading`. A page failure travels in `CommitPage::error`, not the session's shared `requestFailed` (which a failed search also
  emits).
- A commit search cannot run on the GUI thread, and a reveal cannot conclude "not found" while a page is in flight. `RepoSession::requestCommitSearch` runs on the pool; `Repository::searchCommits` uses `git log --all -i -F` (message/author are literals; without `-F`, `--grep` is a regex) and `git rev-parse` for a hash, with `\x1f` between fields.
  `CommitModel::canFetchMore()` is false both at exhaustion and while loading; page for a specific commit with `isLoading()`/`hasMore()`.
- A working-tree compare is `git diff <rev>`, which lists tracked changes only; `workdirChangedFilesAgainst` leaves untracked files out (matching the binary), and `status()` still shows them. Do not add `INCLUDE_UNTRACKED`.
- What a rebase did is in the reflog: `git reflog --all --date=iso` shows `rebase (start): checkout <branch>`, `rebase (pick): <subject>`, `rebase (finish): refs/heads/X onto <sha>`, `rebase (abort)`.

### Graph layout and painting

- The commit-graph delegate paints per row; long edges render only because `GraphNode::activeLanesBelow` lists every lane alive below the row. Keep it populated.
- `GraphLayout::append` must continue from the live-lane state the last row left (`processRows`), touching only the new rows; re-laying out all rows is O(rows×lanes) and quadratic. Three traps: the first new row's `activeLanesAbove` is the previous row's `activeLanesBelow`; an edge whose parent is not loaded parks in `m_pendingEdges` and learns `toRow` on
  arrival; `updateLaneOrder` remaps only the new rows when existing lane importance order is unchanged, else every row (nodes already hold display lanes, so the remap goes through `m_laneOrderInv`). `setCommits`/`setTipColors`/`setTipPriority` still rebuild from scratch. `incrementalPagesMatchOneShotOnAWideGraph` compares both paths field by field.
- A row's live lanes are shared, not copied: `GraphNode::activeLanesAbove/Below` are immutable, sorted `LaneSet`s handed to every row with the same set. A copy per row was rows×lanes (2.3 GB at 20k rows / 3000 branches). `updateLaneOrder` remaps each distinct set once and caches the original *beside* its remap (freed, the address could be reused and
  mistaken). The delegate skips verticals past the column's right edge; with thousands of lanes the clip discards them only after each is stroked.
- A lane's colour cannot live on the lane index — lanes are recycled. Snapshot per row as `GraphLane{lane, colorIndex}`.
- Draw a line across the full row height to join with the row above; `GraphNode::activeLanesAbove` tells the delegate which lines already existed, so a line starting at a commit begins at its node.
- Draw the meeting point of branches at the *parent* row, not the child: a line stays in its own lane and bends into the node where branches actually meet (`GraphNode::converging`). No `ancestorOf`-style reasoning; convergence follows from which lanes were awaiting the commit.
- A straight line is not an edge: the first parent continues in the same lane and is drawn by `activeLanesBelow`; emitting an edge for it draws the segment twice.
- Do not hand a lane to a new line in the row where it just ended; `burned` marks such lanes.
- Never draw both a curve and a vertical for the same born lane: skip the `activeLanesBelow` vertical when the lane is born at this row (a merge's extra parent already gets a curve from the node).
- A commit can be a branch tip and still sit on another branch's line: the node takes the label's colour while the line below keeps the lane colour.
- The selected commit's branch is traced (`graph/branch_path`): up its own lane to the tip (a lane born as a merge's extra parent ends the climb), then down the first-parent chain, which crosses into each branch it was cut from at the fork. Segments are rows, never lanes (the lane order is a permutation a page may change); the delegate reads the lane at paint time and bends the line into the node where the lanes differ. `CommitModel::branchPath()` re-traces lazily after any graph change (`m_pathDirty`); `setPathAnchorRow` is set from `onCommitSelected`. `test_branch_path`.
- The graph column auto-fits at most `kDefaultGraphLanes` (5) lanes; the reader widens it.
- Lane order follows importance, not the walk: the trunk (`RepoInfo::trunkBranch`: what `origin/HEAD` names, else `main`/`master`/`trunk`, local or `<remote>/<name>`), then HEAD, pinned, the branch last clicked, then the rest; a stash ranks below everything (`GraphLayout::setTipPriority` + `applyLanePriority`, a permutation after layout). A tip's priority also applies to the lane it *lands on*, not only lanes created for it. A working-tree row ranks as its parent's tip.
- Where lanes meet, the commit continues the *most important* lane awaiting it (`mostImportantLaneAwaiting`, leftmost on a tie), and the others bend in. Taking the lowest natural lane put HEAD's line through the fork, so `master` looked cut from the checked-out branch. `aForkStaysOnTheMoreImportantLine`.
- Narrowing the graph column must not rescale it: lanes keep their pitch, nodes their size; a translucent strip covers the right edge (capped ≈ one node across), drawn over lines only (nodes come after, so a commit is never hidden by its own strip). A node whose lane is behind the strip is dimmed; a node outside the column is pulled to the edge. With <2
  lanes of room the column is *collapsed*: all nodes in one left column, full strength, explicitly exempt from dimming.
- Clip the graph delegate to its cell (otherwise a wide graph paints over the messages); repaint on resize. `--graph-width <px>` pins the column for headless snapshots.
- Verify graph changes against data, not a screenshot: `--probe` dumps lanes, colours, edges and meeting points per row; compare with `git log --all --graph --oneline`.
- The history must be walked over *all* refs (`refs/heads/*`, `refs/remotes/*`, `refs/tags/*`), not from HEAD (a graph from HEAD alone is one line).
- Graph keyboard: Alt+Up/Down and Ctrl+Home. The parent is found by searching the loaded range (not the next row); the child by walking *up* (the graph is newest-first); nearest child above wins. `--ui-navigate <row> <parent|child|tip>`.
- `setCurrentIndex()` throws a multi-selection away (its click command is `ClearAndSelect`). Move the current row with `QItemSelectionModel::NoUpdate`, *before* applying the selection. This applies in the commit graph and the branch rail (`onRefContextMenu`).

### Graph animation

- `CommitModel::reset()` is asynchronous: rows empty, the first page seeds on a worker, the new graph appears at `onPage`, not `modelReset`. Capture the old snapshot at `modelAboutToBeReset` and diff at the **first** `rowsInserted` after it (diffing at `modelReset` sees "everything vanished").
- Do not animate pagination: `fetchMore` appends `rowsInserted` at the tail. Only a row-0 change (the working-tree row appearing/leaving) and the first page after a reset animate; the rest call `GraphAnimation::baseline` silently.
- An item-view delegate cannot draw outside its own cell; the view grabs the graph cell of every visible row into a `QPixmap` before the change and blits it at the interpolated position. The delegate skips that row (`animationIsMorphing`).
- Animation ghosts live in viewport coordinates: `scrollContentsBy` drops them.
- `--shot` and headless runs must disable motion (`setAnimationsEnabled(false)`); the one check that needs it (`--ui-graph-animation`) turns it back on and reads transitions, not pixels (`lastAnimationTrace`).
- A per-frame animation must not walk the history: look up by oid/lane in hash maps and do not invalidate `headLineageSet`.

### Refs, pills and labels

- Never put a decorated string into a model's DisplayRole and then use it as data (`★ main` fed to `git_branch_lookup`). Display text lives in DisplayRole, the real name in a dedicated role; `RefModel::refShortName()` is the only accessor callers should use.
- Reference groups start collapsed; `refreshPanels()` expands the tree.
- A local branch and its remote-tracking counterpart are one branch: `RefPillLayout::arrange` groups `<remote>/<localName>` with the local and shows the local name with a cloud icon (`RefPill::hasRemote`/`remoteRect`); the pair does not count in the `+N`. Names behind `+N` live in `RefPill::collapsedNames` (the hover reveal), one per other branch: a paired local stands for its twin (`collapsedWithRemote`, drawn with the cloud) and the shown pill's own twin is not listed.
- `RefPillLayout::arrange` takes `trackings` right after `kinds` with no default: omitting it does not compile, so the painted pill and the hit-tested one cannot disagree. A missing entry (fewer trackings than labels) is "no badge", never a crash. The badge sits between name and kind icon and is reserved in the pill's width (`badgeRect` never overlaps
  `textRect`); `RefPillLayout::trackingText` is the one spelling of the arrows ("↑","↓","↑↓").
- The delegate and the graph view take their four parallel lists from one model call: `CommitModel::refPillData(row)` builds `labels`/`colors`/`kinds`/`trackings`; `RefsDelegate::paint` and `CommitGraphView::rowPills` (hit test, hover badge, drop flight) both call it. Do not copy the loop.
- Label geometry is shared between painting and hit-testing (`RefPillLayout`).
- `paintRefPill` is the one drawing of a label; the delegate and the reveal overlay both call it (the remote cloud and checked-out tick live inside).
- A remote branch whose tip is on the trunk's first-parent line (and is not the trunk, and has no local twin) carries no label: `CommitModel::refsAt` serves `m_shownRefs`, `m_refsByCommit` minus those, rebuilt lazily (`ensureShownRefs`, dirty with the graph). Release branches cut at a merge hung on `master` and read as its. Colours and lane priority still read `m_refsByCommit`. View → Show Merged Remote Branches (`setShowMergedRemotes`, `ui/showMergedRemotes`).
- A ghost badge is not invented: `hoverLabelAt` names the topmost label on the commit's own line (`nearestLabelledChildRow` climbs the commit's lane while it is active above) and returns nothing when there is no honest answer. A line born as a merge's extra parent starts at the merge, so a merged-and-deleted branch has no name — walking children through the merge named the branch it was merged into. Its double-click emits `branchCheckoutRequested`, the same signal a real label emits. The badge is drawn for the hovered row or the current row.
- A shared commit belongs to the branch whose line it is drawn on, not to HEAD: follow `nearestLabelledChildRow` (the commit's own lane). Do not reintroduce a `headLineageSet` ancestor shortcut.
- Glyphs live in `gmc_theme`, not `gmc_ui` (the model needs them and `gmc_model` cannot link `gmc_ui`); same for the palette.
- Pins live outside the model: `RefPins` (per repository, keyed by canonical path), handed to the model as a name list on each rebuild.
- `QStandardItem` has no expanded state; expansion belongs to the view and `MainWindow` re-applies it after every rebuild. A pinned folder's subtree is built from names with the pin's own prefix stripped (else `Pinned/feature/feature/login`).
- An enumerator and a nested type cannot share a name: use `SectionItem`/`FolderItem`/`RefItem`.

### Commit graph hover and dimming

- Dimming a row must be done twice: text columns read `Qt::ForegroundRole`; `GraphDelegate`/`RefsDelegate` ignore it and ask `CommitModel::isDimmedRow` directly. Dim by opacity so the layout does not move.
- The hover highlight is a bounded ancestor walk: `ancestorSet` returns the tip and every loaded ancestor, and returns *empty* past the cap rather than an arbitrary slice. `setHighlightedBranch` treats empty as "no highlight".
- `setHoverRowForDiagnostics` must set the hovered ref as well as the row, so the headless path exercises view → `refHovered` → model highlight → delegate dimming.
- A graph screenshot is not evidence of dimming; measure luminance (crop the row, compare min/max between hovered and un-hovered).
- The reveal overlay is viewport-anchored: `m_expandPills` is computed once per hover and shared by painting and hit-testing (`refAt`/double-click/drag). `scrollContentsBy`/`leaveEvent` clear it. It must survive the trip from the pill to the stack (`m_expandKeepOpen` = pill ∪ stack ∪ gap; closing on leaving the pill alone makes revealed branches
  unreachable). Suppress the ghost badge while hovering (`m_expandHovering`). A drag captures the pressed label on mouse-down (`m_pressedRef`) because moving to start it may close the overlay; `startDrag` carries it.

### History filter and visibility

- Hiding a branch is *not* pushing its ref, and never `git_revwalk_hide` (that treats everything reachable as seen, taking shared ancestors and the merge base). `HistoryFilter` (`core/types.h`) names the refs to push; `visibleRefs` empty means every ref.
- A filter that reaches the walk but not `refsByCommit` leaves labels for lines not drawn. Both `Repository::commits` and `Repository::refsByCommit` take the same `HistoryFilter`.
- HEAD is never filtered and a tag is never hidden; both rules live in `MainWindow::historyFilter`, not the `RefVisibility` store. Solo wins over hide.
- `CommitModel::fetchMore` must forward the stored filter. The model default is `AllRefs`; the `HistoryFilter{}` default is `HeadOnly` (preserving the old `Repository::commits` default).
- The ref tree must mark branches the graph filters out (hidden, and under solo non-soloed, are dimmed; HEAD never).
- A branch filter is a mode, not a snapshot: Focus on Current Branch and the presets recompute from the refs every refresh (`visibilityWantedRefs`), so newly matching branches appear on their own. HEAD and the target are never filtered.

### Conflicts: automatic sweep

- git reports a conflict when the two sides changed neighbouring lines; `conflict_auto` merges those by diffing both sides against the base and applying both disjoint change sets.
- A change that replaces nothing with nothing is not a change: `changesFromBase` can emit a degenerate `[i,i)` change where a side ran out of text. Drop degenerate changes before asking "did both sides move", or a none-side counts as moved and the merge takes the other side whole.
- The automatic sweep is deliberately narrow: identical sides, a side equal to the base, trailing whitespace, and disjoint change sets. It does NOT join two different additions (two added lines can be halves of one statement).
- A deletion and an insertion at its edge are not disjoint: `changesCollide` treats a point at the edge of a deletion as colliding; adjacency is only harmless between replacements.
- A conflict's neighbours decide whether it is ours to settle, and a rebase step hides it. `conflict_auto`'s "one side never moved" shortcuts are ordinary in a merge, but in a rebase step `AutoOptions::rebaseStep` "theirs" is the snapshot of the commit being replayed, so a hunk whose our-side equals the base is left to the reader — checked *before* the
  disjoint-changes rule. (Their side equal to the base still settles: keeping ours applies nothing.) `aRebaseStepDoesNotTakeTheReplayedSnapshotWholesale`.
- A change list built from an LCS must record where it ended in the base: `Change::baseEnd` left at 0 makes a replacement look like an insertion and duplicates the base lines.
- `GK_TRACE_SWEEP=1` prints what the sweep decided: per file the parsed sides and reason, per change the range, line count and removal flag.
- The region parser is separate from the sweep: `core/conflict_regions` only says what the regions are (sides, balance, raw bytes), never which side wins; `conflict_auto` keeps its own decision ladder and the editor calls the parser.

### Conflicts: markers and editor

- A marker is exactly N marker characters then a space or end of line, N set by the opening `<<<<<<<`. `core/conflict_regions` owns the rule (`conflictOpeningSize`, `conflictMarkerOf`); the editor's parser and the sweep both use it. `conflictMarkedContent` draws markers one longer than the longest marker-character run any side starts a line with
  (`safeConflictMarkerSize`) because a 7-char underline *is* a 7-char marker. `hasConflictMarkers` accepts any length and counts `|||||||`/`>>>>>>>` as well as `<<<<<<<`, but not a lone `=======` (a legitimate heading underline).
- A conflict with one binary side is binary.
- A binary conflict has no text, and the text path stages an empty file over it. `Repository::keepConflictSide` reads the chosen index stage's raw bytes (`conflictStageBytes`); `resolveConflict` writes with `std::ios::binary`. `ConflictView` refuses `trySaveResolution` and disables editor/Save when `m_binary`; whole-side buttons call `keepSide`.
  `test_conflict_binary` pins the staged oid against `rev-parse :N:path`; `test_conflict_view` drives the pane.
- The editor builds its cards from the index stages, not the file on disk: `Repository::conflictMarkedContent` runs `git_merge_file` in DIFF3 so the base section is always present. A file whose stages yield no text is binary; the editor shows each side's size and offers whole-side choices only.
- Resolving must leave git's resolve-undo record: adding over a conflicted path moves the stages into REUC (as `git add` does); `git_index_conflict_remove` first would drop them. That record is what `git update-index --unresolve` ("Mark as conflicted") restores and `resolvedConflictPaths` lists; read it only while an operation is open or something is
  conflicted. `aResolvedFileCanBeMarkedConflictedAgain`.
- Saving a conflict with markers left is refused: `trySaveResolution` checks `hasConflictMarkers` first; an unresolved region keeps its original bytes (`ConflictRegion::raw`).
- Staging a working-tree resolution must read the file, not the editor's copy: `stageWorkingTree` re-reads at staging time and refuses while markers remain.
- `git merge --squash` writes no `MERGE_HEAD` (with or without a conflict); it leaves `SQUASH_MSG`/`MERGE_MSG`, stages the change and moves neither HEAD nor the ref. `mergeInProgress()` cannot see it; ask `squashInProgress()` (`SQUASH_MSG` exists). Every guard refusing a second operation must name the squash.
- A squash ends as an ordinary single-parent commit; `git commit` removes `SQUASH_MSG`, `git_commit_create` does not. `commitSquash` writes the commit (`createCommit`, one parent) and removes `SQUASH_MSG`/`MERGE_MSG`/ `MERGE_MODE`; the message comes from `SQUASH_MSG` with `#` lines dropped.
- A squash with no conflicts belongs in the commit panel, not a conflict page; keep the page only once it is already up for it (or it still has conflicts).

### Conflicts: page and operation state

- Keep the conflict page visible while `mergeInProgress()`, not merely while conflicts exist, or "Complete merge" vanishes at the moment it becomes usable.
- A sticky page needs a remembered way out. `ConflictView` emits `closeRequested`; the window sets `conflictPageDismissed` per context, which `refreshPanels` respects; `showConflictsPage()` clears it (Branch menu, operations that start on a conflict, and the "already open" guards reopen it), so nothing else must reset it. Closing leaves the operation open on
  disk. Headless: `--ui-conflict close|refresh`; `conflictForDiagnostics` reports `page: CONFLICT|history`; `closingTheMergeEditorLeavesItClosed` clicks the real button.
- The page stack is `m_stack`, not `m_centerStack` (the centre holds the graph and diff view; the conflict and start pages are on `m_stack`).
- A conflict screen with no conflicts must say so, naming the operation: rebase stopped and waiting to continue, merge ready to complete, or nothing conflicted.
- A conflicting `git stash pop` leaves unmerged entries with no `MERGE_HEAD`; open the conflict page by looking at `conflicts()`, not `mergeInProgress()`/`rebaseInProgress()`. git keeps the stash.
- `sequencerInProgress` and the rebase guards are all the same guard: an operation left open on disk (by us, git or another client) must be visible and lead to the conflict page, never a modal refusal nobody can act on.
- Starting an operation while one is already open is a dead end: check for an open rebase or merge first and lead to the conflict page. `checkoutBranch` and `rebaseOnto` also refuse explicitly (git declines switching branches with a rebase open, and a second rebase on top). `--ui-branch-drop <src> <target> [pick]` runs the chain headlessly.
- The modal pre-flight is skipped on the drop paths: `mergeBranchInto`/`rebaseBranchOnto`/`squashMergeBranch` take a `preflightShown` flag.
- A headless driving loop must not reload the page between steps: `conflictForDiagnostics` reloads only when nothing is loaded yet (`reload()` resets every region choice; `--ui-conflict` makes several calls in a row).

### Word diff

- `wordDiff` is token-level and single-line; a conflict hands over blocks. `model/word_diff::emphasisAgainstReference` aligns lines first (a line-level LCS, capped at 4·10⁶ cells) then diffs paired lines word by word; past the cap it returns *empty* (plain text, never a guess). An empty reference is add/add: every line emphasised whole.
- Emphasis offsets are into the side being drawn: use the `after` side (`wordDiff(ref, side).after`); the `before` ranges describe the reference.
- An absent conflict side must not be emphasised: `ConflictView::showEntry` emphasises only when all three stages hold text; a side showing the absent-notice is plain.
- The merge editor's panes and cards share the helper (whole-file Ours/Theirs vs Base; per-region; add/add compares the sides against each other).

### rerere

- rerere is CLI-only (`RerereOps`). Both `git rerere status` and `remaining` list *unresolved* conflicts (`remaining` is the subset not applied from memory); once rerere applies a recorded resolution the path drops out of **both** lists even though `git ls-files -u` still shows the conflict. "rerere applied this" = conflicted in the index, in neither list,
  no markers on disk. Do not read `status` as "everything rerere knows".
- Without `rerere.autoupdate` the index stays conflicted while the working tree is resolved; offer staging the working-tree file, not redisplaying markers. `ConflictView` recomputes this per file and labels the button.
- rerere's on/off lives in the git config, not `QSettings`: read `rerere.enabled`/`rerere.autoupdate` on every repository open (a settings mirror would lie when the CLI or another client changes it).

### Pre-flight

- Pre-flight is advisory and runs before anything moves: per-repo toggle, off by default, the prediction touches neither working tree nor index, and `mergeBranchInto`/`rebaseBranchOnto` ask before the checkout/merge. If the prediction fails, the operation goes ahead with a status note. `--ui-merge-preview <ours> <theirs>`, `--ui-preflight <enable|disable>`.
- `git merge-tree` output is not all paths and its messages are localised: `previewMerge` passes `--no-messages` (informational lines would read as file names) and `--name-only`. Empty stdout when git refuses the pair (one side reachable from the other) means `available=false`, not "clean".

### Rebase and rewrite

- A rewrite needs a base, and the first commit has none — that base is `--root`. `rootBelow` returns the oldest selected commit's parent, which does not resolve for the root; name the root (`kRebaseRoot`) and run `git rebase -i --root`, whose todo still starts with a `pick` (the root) and folds the *second* commit with `squash` (a `squash` as the first line
  has nothing below it). Headless: `--ui-rebase-editor root …`.
- Every rewrite builds a full plan: `rebaseCandidates` must name every commit the range holds, so a single-commit squash builds the whole `base..HEAD` plan and marks the rest `Keep`; a one-line plan is refused, not truncated. The first commit can be dropped: a root rebase whose first line is `drop` leaves its child as the new root.
- The todo is rebuilt from the plan, so the plan is validated against the range first: `rewriteHistory` compares the step set with `rebaseCandidates` and refuses a mismatch (a missing line is silent loss); `rebasePlanRefusal` refuses a first squash/fixup, everything dropped, and a reword with no message, so the editor greys out Run with the reason.
- Anything written into a todo must be one of git's own words (`pick`, `squash`, `fixup`, `drop`, `exec`): git silently drops unknown lines and the commits with them.
- Squash, fixup and reword are `git rebase -i` with a generated todo; the editors git opens are answered by scripts in `.git/gitmycelium-rewrite`.
- Matching hashes must be by prefix: git's todo carries abbreviated hashes, and a plan keyed by full ones matches nothing.
- `)"` inside a shell snippet terminates a C++ raw string unless the literal has its own delimiter; the sequence editor is `R"SH(…)SH"` for that reason.
- Every path git hands to the shell is quoted: `sequence.editor`/`core.editor` via `shellQuoted`, and an `exec` line names its scripts with `$(git rev-parse --git-path gitmycelium-rewrite/…)`, not an absolute path (a repo path with a space broke every squash/reword/reorder).
- A reword is a pick plus an `exec` that amends it (git's own reword opens an editor on a file whose relation to the commit a script cannot read).
- `--ui-rebase-editor <base> [plan]` eats every following word up to the next flag; pass the repository path first.
- A per-row control in a drag-reorderable list must be model data, not a widget: a `setItemWidget` combo does not move reliably with an internal move. The rebase editor's Action column is a `QStyledItemDelegate` returning a `QComboBox` reading/writing the item's `Qt::UserRole`, so the data moves with the row.
- A rebase stopped for `edit` exits 0 and leaves no conflicts, but it is still open; code reading "rebase in progress and nothing conflicted" as a failure misreports it. The operation page keys off `rebaseInProgress()`.
- An `edit` step is finished with `git commit --amend`, not `git_commit_amend` (which takes the commit's own tree; the point is the reader's staged changes). `amendStoppedCommit` shells out, editor silenced. It is `guarded`, never `recorded`.
- A rebase stopped for `edit` has a detached HEAD, so `headBranch` reads empty (honest state); the commit to amend is `headId`. `--ui-rebase-amend [message]`.

### Rebase semantics (why the binary)

- The rebase runs in the git binary (`rebaseOnto`/`rebaseContinue`/`rebaseAbort` shell out, like fetch/push): libgit2 has no equivalent of git's skipped cherry-picks and replays an already-applied commit, which can conflict with its own original. `runRebaseLoop` is left in place and unused; delete it when nothing wants the libgit2 path.
- libgit2 does not do `git rebase`'s up-to-date check: given a branch already based on its target it rewrites every commit (same trees, new ids). `rebaseOnto` compares the merge base against the target first and returns `UpToDate` without creating a rebase. Counting commits cannot catch this; comparing the tip id can.
- libgit2's rebase does not skip steps that carry nothing: `GIT_EAPPLIED` (-18) and an index whose tree matches HEAD's both count as a skip. Otherwise an already-applied patch fails ("cannot commit a rebase step") and leaves an empty conflict screen with `rebaseInProgress()` true.
- The rebase agrees with `git rebase` down to the tree id: `ourRebaseLandsOnExactlyTheTreeGitLandsOn` runs both and compares `rev-parse <branch>^{tree}` and the commit count. Trees, not "similar trees".
- Rebasing the same branch onto the same target repeatedly must stay stable: six rounds, same commit count, no conflicts asked.
- A branch that adopts another branch's commits is git's behaviour, not a bug: rebasing across four branches replays all four (new ids) on the fifth; nothing is lost or invented.
- Settle "does your client ask wrongly" with a matrix over ordered branch pairs (run `git rebase <target>` in one copy, the engine in another; compare exit code and conflicted files). WE-ASK (we stop where git goes through) and WE-SILENT (we go through where git stops) are the two verdicts that matter.
- libgit2 caches the index, so a conflict written by the git binary is invisible; every `git_repository_index` call is followed by `git_index_read`. Forgetting one shows as "the rebase stopped and nothing asks for me". Reads pass `force = 0` (reloaded only when the file's stamp changed; the binary writes by rename, so the inode moves); a forced read costs 170 ms on a 40 MB index, and `indexFileContent`/`conflicts` ran on every click and refresh. Writes keep `force = 1`.
- `git_merge`/`git_rebase` leave state on disk; freeing the handle does not remove it. A conflicting rebase stays open for `rebaseContinue()`, which reopens it with `git_rebase_open`.

### Cherry-pick, revert, merge

- Cherry-pick and revert run in the git binary: libgit2 applies the change and leaves `CHERRY_PICK_HEAD`/`REVERT_HEAD` behind after it *succeeds*, so the repository looks mid-operation and the guard refuses a second operation.
- A merge runs in the git binary (`git merge --ff-only`, or `--no-ff --no-edit`); libgit2's merge ran no rerere, no `.gitattributes` merge driver, no clean/smudge filter, no hook and no signing. `mergeBranch` keeps libgit2 only for the side-effect-free up-to-date/fast-forward analysis.
- `abortMerge` calls the operation's own abort (`merge --abort`, `cherry-pick --abort`, `revert --abort`) and `reset --merge` for a squash or a conflicting stash pop; with nothing open it does nothing. `git_reset(HARD)` threw away unrelated uncommitted edits and left sequencer markers behind.
- A merge stopped by a refusing hook leaves `MERGE_HEAD` with no conflicts and is reported `Conflicted` with count 0; the conflict page offers to complete it.
- A merge's source is any revision git can merge, not a local branch: `mergeBranch` resolves through `resolveRevisionToCommit`; the "Merge Branch Into Current…" picker lists remote-tracking branches after the local ones. `aRemoteTrackingBranchCanBeMerged`.
- Pass a revision through `resolveRevisionToCommit`, not `parseOid` (id-only; a tag or branch name becomes "malformed object id 'v1.0'"). `canFastForward`, `createBranch`, `moveBranch` and `fastForwardBranch` accept whatever the caller has.

### Branch operations

- libgit2 refuses to delete the checked-out branch only via our own guard (`git_branch_delete` succeeds); check HEAD's shorthand first.
- `git_branch_set_upstream` takes the remote-tracking branch ("origin/main"), not the remote, and answers `GIT_ENOTFOUND` when that branch does not exist yet; `Repository::setUpstream` turns that into a sentence pointing at fetch or `push --set-upstream`. `createTrackingBranch` deletes the branch on failure.
- A branch operation that can destroy work must refuse, with losing it a separate explicit call: `moveBranch`/ `deleteBranch`/`mergeBranch` each have a safe form and a `…Forced` form; the UI asks before the forced one. The fast-forward path advances the current branch, never `git_repository_set_head(theirRef)`.
- A mass operation is one operation-log entry: `deleteBranches` loops inside a single `recorded()`, so one undo restores the whole set. The batch takes a `base` ("would deleting this lose work" is asked of the target, not of HEAD).
- Force is applied per branch, only to those computed unmerged: `deleteBranches` re-asks `branchMergedInto`. Do not treat `deleteBranch`'s refusal as "retry with force" (it may have failed for a protected name or a lock).
- Sections, folders and the stash/worktree/submodule rows are selectable: `selectedLocalBranches()` filters to `RefModel::isRef()` + `RefKind::LocalBranch`.
- Ref names are a directory tree, so a branch cannot also be a folder (`feature` and `feature/x`); `createBranch` checks the two collisions and says which.
- `fail()` must not append libgit2's last error blindly (it is literally "no error" when the failure was ours).

### Staging, patch and diff

- A hunk that drops a change in the middle is no longer a hunk: `git apply` needs consecutive old-side (context + removals) and new-side line numbers. `core/patch_selection::renderPatch` splits a hunk as needed when an unselected change sits between two selected ones.
- Line staging goes through a generated patch and git's own parser: `renderPatch` writes a unified diff; `Repository::applyPatchToIndex` reads it with `git_diff_from_buffer` and applies with `git_apply(..., GIT_APPLY_LOCATION_INDEX)`.
- `git_reset_default`'s null target means "remove from the index", not "HEAD"; `unstageFile` resolves HEAD and passes its commit.
- `git_index_add_all` takes new and modified files, not deletions (a deleted file is not in the working directory); `stageAll` runs `git_index_add_all` and `git_index_update_all` (the second removes entries whose file is gone).
- A missing path is not a staging error: `git_index_add_bypath` answers `GIT_ENOTFOUND` for a path neither on disk nor tracked; `stageFile` treats that as "nothing to stage".
- Stash exists only in the `git` binary (`StashOps` shells out; libgit2 has no stash). It changes the working tree and index but moves no pointers, so it goes through `guarded`, *not* `recorded`.
- `git stash apply` takes a commit-ish, but `pop`/`drop`/`branch` take only `stash@{n}`; `StashOps::selectorFor` re-reads the list and resolves the oid to its current index at call time.
- `git stash list` is parsed from `--format=%gd %H %gs`, space-separated (the first two fields contain no spaces).
- "Only for me" must not change what anyone else gets: the exclude scope writes `.git/info/exclude` for an untracked path and marks a tracked file skip-worktree (`Repository::setSkipWorktree`), never `git rm --cached` (that stages a deletion for everyone). Repository → Local Ignore Rules… (`LocalIgnoreDialog`, written by `applyLocalIgnore`) adds/removes exclude lines and un-hides skip-worktree files (`ls-files -v`, `S`).
- Ignoring a path is two actions: the pattern goes into `.gitignore` (or `info/exclude`), and an already-tracked file must be untracked too. `Repository::untrackPathspec` runs `git rm -r --cached`, asking `ls-files` first (`git rm` on a matching-nothing pathspec is a localised fatal error). The pathspec is `:(literal)…` for a file/folder (a name with `*` or
  `[` is a name, not a pattern) and a glob for an extension/folder.
- `GIT_DIFF_INCLUDE_UNTRACKED` alone gives a delta with no hunks; an untracked file's content needs `GIT_DIFF_SHOW_UNTRACKED_CONTENT`, and libgit2 does not match an untracked file against a pathspec, so `workdirFileDiff` drops the pathspec only for an untracked path (`git_status_file`).
- An untracked file is diffed from its own bytes (`untrackedFileDiff`, `git_patch_from_buffers`): libgit2 does not match an untracked file against a pathspec, and without one `git_diff_index_to_workdir` walked the whole work tree (1 s on 300k files).
- A working-tree path can sit in both Staged and Unstaged, so the diff must be told which: the panel's `fileSelected`/`fileActivated` carry a `staged` flag; `renderFile(path, workingTree, staged)` picks `stagedFileDiff` (HEAD→index, offers Unstage) or `workdirFileDiff` (index→workdir, offers Stage). The flag is part of the render cache key.
- Discarding a hunk is a reverse working-tree apply and is not undoable: `applyPatchToWorkdir` takes the reverse of `workdirFileDiff`; `applyDiscardRequest` confirms first; only the *unstaged* side offers the control (a staged change is unstaged, not discarded).
- A bool cannot say "discard": `applyHunkSelection` takes `HunkApply{Stage, Unstage, Discard}`; the diff it re-derives depends on the mode, and the anchor is checked after that so a click cannot act on a hunk that moved.
- `git apply` and `git am` are two features: a plain diff belongs in the working tree; `git format-patch` output is a mailbox carrying author/date/message. `PatchOps::isMailbox` reads the first non-empty line (`From <sha>` vs `diff --git`); mailbox → `git am --3way`, diff → `git apply`. Keep git's own output as the failure message.
- A patch test cannot assert the commit id (`git am` stamps the committer to now); assert tree, author, message, parent.
- An untracked `*.patch` inside the repository shows in `git status`; write test patch files into a second temp dir.

### Diff rendering and view

- `fillFileDiff` must carry `\ No newline at end of file` (`DiffLine::noNewline`); libgit2 emits it as a separate line with a `*_EOFNL` origin, and dropping it gives a staged blob a trailing newline the file does not have.
- libgit2 sets `GIT_DIFF_FLAG_BINARY` while a patch is generated, so read the delta flag *after* `fillFileDiff` builds it.
- `parseUnifiedDiff` mirrors libgit2's shape, not git's raw text: a creation or deletion names the path on *both* sides (oid zero on the absent one), so `--- /dev/null` yields `oldPath == newPath`; a line's number on the side it is not on is `-1`. Checked field by field against libgit2 in `test_unified_diff`.
- `core/diff_rows::alignedRows` (Qt-free) decides which removal pairs with which addition and where a filler belongs; both the split and unified renderers read it, so the two modes cannot tell different stories.
- Both split panes must emit the same number of blocks per hunk: every hunk emits one header line and one line per aligned row per side; an empty cell is a filler line, never skipped. `splitPaneBlocks()`.
- Two synchronized scrollbars drive each other unless a flag stops the loop: `m_syncingScroll` guards the round trip and must be cleared even when the guarded set is a no-op.
- Syntax highlighting produces runs, not HTML: `ui/syntax_highlight` tokenises a line into coloured runs and carries block state (`/*…*/`, Python triple quotes) between lines; `renderHighlightedLine` turns runs plus an optional emphasis range list into HTML, cutting at every boundary and wrapping each piece once (a changed word in a string keeps both
  attributes). Escaping happens in the renderer, after highlighting.
- The file view keeps one `HighlightState` for the whole file; the diff view keeps one per hunk (the lines between hunks are lines it never sees).
- `QHash::value()` returns a copy; returning `const T&` from such a lookup dangles. Look up an iterator and return a pointer to the value, or return by value.
- Word wrap needs the widget and the markup to agree: `setLineWrapMode(WidgetWidth)` plus `white-space:pre-wrap`; `setWrap` sets both, and it is a toggle, so it must re-render whichever view is showing.
- The minimap marks come from the rendered line map (`m_lineMap`), not a second reading; `DiffMinimap` hides when there is nothing to mark.
- Blame is addressed by line number, not order: `git blame --line-porcelain` reports `finalLine`; `--line-porcelain` repeats the header per line, so each group is self-contained.
- A file's history needs `--follow`, which only the binary has; the format puts a tab between fields (subject and path can hold spaces).
- A blame link carries a commit id, so parse it before the hunk-index path: `onAnchor` calls `toInt()` on `gk://hunk/N` and `gk://discard/N`; handle `gk://blame/<sha>` first.
- `git restore --source=<rev>` is the binary's job (filters, index or working tree).
- Commit selection auto-selects the first changed file, which renders its diff; do not clear the diff view after `showCommitFiles()`.

### Compare

- A comparison is not a commit: the details panel gets `showComparison`, not a synthesized `CommitInfo` (a made-up author/date would be a lie, and an amend control would rewrite the wrong thing). The window keeps `m_compareBase`/`m_compareTarget`; `renderFile` asks `fileDiffBetween`.
- The render cache key must tell a comparison from a commit: `cmp:<base>..<target>` (else a path already rendered for a commit swallows the comparison).
- The comparison cache key carries the mode: `cmp:`/`cmp3:`/`wip:` prefixes, so switching two-dot/three-dot on the same file is not swallowed.
- `changedFilesBetween` diffs commit trees, so the pair order carries the sign; reversing base/target swaps added and removed.
- The three-dot range is composed, not a new git call: `A...B` is `mergeBase(A,B)..B`; `showComparisonMergeBase` resolves the merge base first and hands the pair to the ordinary two-tree read. Only the heading (`a...b`) differs and it is passed as a label rather than re-derived.

### Filtered files and LFS

- A checkout is `git switch` (`switchWithGit`), never `git_checkout_tree`: libgit2 took 1.6 s on 300k files where the binary took 0.35 s, and the binary also runs filters and the post-checkout hook. Undo's checkout and moving the checked-out branch are `readTreeWithGit` for the same reason, filter or not. The window runs it through `runCheckout`, on a worker in async mode (`m_checkoutRunning` refuses a second one); a checkout that finishes after a tab switch resets that tab's history.
- libgit2 does not run external clean/smudge filters, so staging and checkout of a filtered file go through the binary. `externalFilterConfigured` looks for any `filter.<name>.{clean,smudge,process}`; `pathUsesExternalFilter` reads the path's `filter` attribute. `stageFile`/`resolveConflict` use `git add` for such a path; `stageAll` uses `git add -A`
  whenever any filter is configured; `checkoutBranch`/`checkoutCommit` use `git switch` (which also runs the post-checkout hook git-lfs installs). Other working-tree rewrites take the binary too: undo's checkout and moving the checked-out branch (`readTreeWithGit` = `read-tree -m -u HEAD <target>`, `--reset` for the forced move), discarding a filtered file
  (`git checkout -- <path>`) and a hard reset. `test_filters`.
- An LFS file's content comes from an external filter, so libgit2 shows the 120-byte pointer; `GitLfsOps` shells out to `git-lfs`. Everything under LFS is `guarded`, never `recorded`.
- `git lfs install` writes the pre-push hook; `GitLfsOps::install` runs `--local` (a client must not rewrite the reader's global config) and the push path uploads explicitly with `git lfs push`; an empty ref becomes `--all` (`git lfs push <remote>` with no ref is an error).
- A local bare repository does carry LFS objects; `test_lfs` pushes to a bare remote, clones it and compares the body byte for byte. `git-lfs` is optional: every LFS test checks availability and skips, and the parsers are tested on their own.

### Worktrees

- A linked worktree is its own repository handle: libgit2 opens the worktree path and reports the git dir as `.git/worktrees/<name>`. Refs and objects live in the common dir, but HEAD, the index and every operation marker (`MERGE_HEAD`, `SQUASH_MSG`, rebase state) live in the worktree's own dir, so operations must not cross. `test_worktrees`.
- Worktree operations go through the binary and through `guarded`, never the oplog (`git worktree add -b`, locking with a reason, pruning). The filesystem is outside the oplog snapshot, so undoing "create worktree" would remove the branch and leave the directory behind.
- `git worktree remove` refuses a dirty worktree; the window tries without `--force` and asks once, explicitly, before discarding. Do not default to `--force`.
- `--ui-worktree <op> [args…]` eats every following word that is not a flag; pass the repository path first. Its `add <path> <branch> [start]` decides `-b` from whether the branch already exists.
- The main worktree is the first entry of `git worktree list`.
- A linked worktree is a second checkout, not a second repository: key per-*repository* settings (pins, visibility, column layout, auto-fetch, pre-flight) by the common dir (`git_repository_commondir`), and per-*checkout* state by the worktree's own dir (`git_repository_path`). `RepoInfo` carries both. `RefPins::load`/`RefVisibility::load` take a legacy key
  and `migrateRepositorySettings` copies `QSettings` once.
- A worktree directory *inside* the main checkout makes the main checkout dirty (`wt/` is untracked content), so a test checking a clean checkout puts the worktree in a separate temp dir.
- A linked worktree's status comes from the binary (`git -C <path> status --porcelain`), not a second `git_repository*`. In porcelain `??` is untracked, X is the index status and Y the working tree — counting lines would call a staged-and-modified file one change instead of two.
- A worktree's WIP node: `wipRow()` is the current checkout's node (or -1); `isWipRow(row)`/`WipRole` cover every node, so `wipRow() == 0` is not "a WIP exists". Each node's parent is its own worktree's HEAD, read from `git worktree list`.

### Submodules

- `git submodule status` prints the checked-out commit, not the recorded gitlink (only `-`, not initialized, prints the recorded one). The recorded gitlink must come from the index (`git ls-files --stage`, mode `160000`, tab-separated); a nested submodule's gitlink lives in its own parent's index, so read it once per level. `test_submodules`.
- `.gitmodules` is git-config, not a file to parse by hand: `git config -f .gitmodules --get-regexp`, splitting the key on the *last* dot (a name may contain dots, e.g. `sub.lib`), and prefixing a nested `.gitmodules`' relative paths with the containing submodule's path.
- Submodules go through the binary and through `guarded`, not the oplog (`add`/`update`/`sync`/`deinit` touch `.gitmodules`, `.git/config`, the parent's index and a nested checkout).
- `git submodule init --recursive` does not exist; recursive registration is `git submodule update --init --recursive`. A local-path remote needs `protocol.file.allow=always` (or `GIT_ALLOW_PROTOCOL=file`) for the file transport, which git refuses by default for submodules; the product must not weaken that guard, so tests set it for their own process only.
- `git submodule deinit` refuses a submodule with local changes; try without `--force` and ask once before discarding.
- A submodule's `.git` is a file, not a directory (a pointer into `.git/modules/<name>`): ask `QFileInfo`, not `QDir(path).exists()`.
- libgit2's status does not report a gitlink whose checkout moved (dropping `GIT_STATUS_OPT_EXCLUDE_SUBMODULES` is not enough). `Repository::status()` computes pointer changes explicitly: staged from `git_diff_tree_to_index` (gitlink deltas only), unstaged/conflicted from `SubmoduleOps::list` (`git submodule status`). A repository without `.gitmodules` never
  runs that binary; a submodule's *own* dirt is not a pointer change.
- A submodule is a gitlink, so `git_index_add_bypath` cannot stage it: `stageFile` routes a registered submodule (or an index entry in `GIT_FILEMODE_COMMIT`) to `git add -- <path>`, and `discardFileChanges` to `git submodule update -- <path>`. `.gitmodules` is an ordinary file and keeps the normal path.
- A pointer into a by-value `std::vector` temporary dangles (`findLib(session.status())` produced a garbled oid that still ended in the right 8 hex characters). Bind the vector to a local before taking a pointer into it.
- The submodule auto-update hook is triggered by the operation, not by a timer or a refresh: after a fetch/pull, a checkout, and a merge/rebase that *completed* — never one that stopped on a conflict. Opt-in per repository, off by default (it can hit the network and move nested checkouts).
- A background auto-update failure goes to the status bar, never a modal (`onSubmodulesUpdated` reports git's last line).
- `git submodule update` without `--remote` checks out the recorded commit; with it, the tracked branch. Both preferences live in `QSettings` under the common git dir, so a worktree inherits them.

### Status and refresh

- The status is the binary's: `Repository::status()` runs `git status --porcelain=v2 -z --untracked-files=all --renames --ignore-submodules=all`, parsed in `core/porcelain_status`. On a 100k-file tree 18–40 ms vs libgit2's 170 ms (~7 ms with `core.fsmonitor`). One deliberate difference: a path added with `git add -N` is an *unstaged* addition (libgit2 called
  it staged-added and unstaged-modified). Submodule pointer changes are appended separately. A refresh reads the status **once** and hands it to the WIP row, Commit Panel and status bar (`worktreeWip`, `showWorkingTreeChanges`, `refreshStatusBar`).
- A refresh must stay cheap: in async mode its reads are off the GUI thread and applying a snapshot is ~100 ms on a 100k-file/3000-branch repo when the refs moved (ref tree rebuild, labels, palette commands) and ~10 ms when only the work tree did: `RefModel::rebuild` returns early for the same refs and HEAD, `CommitModel::refreshRefs` runs only when `syncHistoryWithRefs` says they moved, and a linked worktree's `git status` is in the snapshot (`SessionSnapshot::linkedWip`; a prunable or missing directory is not asked); synchronously it was ~120 ms on the GUI thread on a 100k-file/3000-branch repo (89 ms on the 1M synthetic, 33 ms on git/git). `glyphIcon` keeps the pixmaps it paints (the branch tree asked for the same three icons per row); `SubmoduleOps::list` returns at once without a `.gitmodules`; `refreshPanels` reloads the merge editor only when the status
  shows a conflict, an operation is open, or the editor is on screen or holds something. The reads are still synchronous; moving them to a worker remains.
- The watcher is the only *push* signal (libgit2 1.9 has no fsmonitor): `ui/repo_watcher` watches the whole work tree, contents included — inotify on Linux (every directory, added on a worker, new directories as they appear; budget half of `max_user_watches`), FSEvents on macOS, ReadDirectoryChangesW on Windows — plus the git dir, the common dir and `refs/`. What git ignores (`RepoSession::ignoredPaths`, `ls-files -o -i --directory`) is not watched or reported. It debounces (300 ms, at most 2 s for a file written without pause) and emits `changed(gitState)`. Elsewhere it falls back to QFileSystemWatcher, which sees entries come and go in 512 directories but not a file's content. Qt's directory watch has no `IN_MODIFY`: an editor saving in place went unseen, and a monorepo has 50k directories. Watching starts asynchronously (`ready()`); a test waits for it.
- Every refresh decides whether the history moved: `syncHistoryWithRefs` hashes HEAD and every ref and reloads the graph (`reloadHistory`, the selection put back quietly once its row is loaded) when the hash moved and nothing reset the model since. An operation that resets the model itself is not reset twice; one that only called `refreshPanels` (the stash commands did) or a change made in a terminal is caught here. `aStashMadeOutsideReloadsTheGraph`, `aStashMadeInTheWindowReloadsTheGraph`.
- A refresh does not move the reader: the panels switch to the working tree only when no commit or comparison is selected, and `WorkingTreePanel::setChanges` keeps the chosen file (choosing it again re-reads its diff, which is how a file edited elsewhere updates on screen). `anEditMadeOutsideReachesTheOpenDiff`.

### Promisor / partial clones

- libgit2 cannot read partial (promisor) clones: when `RepoInfo::partialClone` is true, read through the CLI (`src/git/cli`, `git cat-file`, `git log`, `git fetch`).
- On a `--filter=blob:none` clone, libgit2 opens it and `commits`, `refsByCommit`, `status` and tree diffs (`changedFilesBetween`) work (trees present, only blobs filtered). `fileAt` returns success with empty bytes (a silent empty file); `fileDiffBetween` throws `GitError: object not found`. The binary lazy-fetches on demand and the object becomes local.
  `fileAtCli` returns the real bytes. `test_partial_clone`.
- `fileAtCli` detects binary by git's own test: a NUL in the first 8000 bytes (not a length compare — `runGit` returns exact bytes now). Binary is returned as no text, the same answer libgit2 gives via `git_blob_is_binary`.
- The CLI diff path must pass `--no-renames`: `buildCommitDiff`/`diffTrees` set only `GIT_DIFF_INCLUDE_TYPECHANGE`, so libgit2 reports a rename as a delete plus an add, while `git diff` detects renames by default. `diffCliText` forces `--no-renames` (plus `--no-color --no-ext-diff --src-prefix=a/ --dst-prefix=b/` and `-c core.quotepath=false`) to keep the
  promisor path's shape.
- The file list goes through `git diff --name-status --no-renames` too: on a `--filter=tree:0` clone there are no trees, so libgit2 cannot compare two commits ("cannot read the tree"). `changedFiles`/`changedFilesBetween` take the same promisor branch and parse `--name-status` (`parseNameStatus`). Graph and refs stay on libgit2 (they already work on both
  clones).
- The Commit Panel's diffs need the index's blobs: `stagedFileDiff` and a tracked file's `workdirFileDiff` take `indexDiffCli` (`git diff [--cached] -- <path>`, `diffCliText`'s flags) when `partialClone`; an untracked file has no index side and stays on libgit2.
- `--filter=blob:none` on a *local path* clone is silently ignored (git's local transport hardlinks and drops the filter). A promisor clone needs a `file://` URL and `uploadpack.allowFilter=true`; `RepoInfo::partialClone` reads the promisor config, so a local-path clone that asked for `partial` reports `false`.

### CLI and subprocesses

- `runGit` is `posix_spawnp`, not a shell, and stdout/stderr are separate streams. Arguments go to git unquoted (`git -C <dir> …`); stdin is `/dev/null`; `out` is stdout byte for byte and `err` is stderr. The child gets `LANGUAGE=en` (English messages without changing `LC_CTYPE` for hooks/editors), `GIT_OPTIONAL_LOCKS=0` and `GIT_TERMINAL_PROMPT=0` (a
  credential prompt fails instead of waiting on the GUI's terminal). It leads its own session (`POSIX_SPAWN_SETSID`): ssh asks about an unknown host key on `/dev/tty`, so with no controlling terminal it uses `SSH_ASKPASS` or fails at once (`gitHasNoControllingTerminal` compares session ids). A failure message is `failureText(run)` — stderr's first line, or
  stdout's when stderr is empty. A report of a *successful* command must still show `out` and `err` together (fetch/push/gc talk on stderr).
- `pipe2` is Linux/FreeBSD only, and a pipe end must not be left open in the child. `runGit` creates its two pipes with `makePipe` (`pipe2(O_CLOEXEC)` where available, else `pipe` + `fcntl`); to avoid a thread-spawned git inheriting the write end, the child closes all four ends explicitly through `posix_spawn_file_actions_addclose` (`closeInChild`, atomic
  with the spawn). Only fds above `STDERR_FILENO` are closed there, or one could be the descriptor just `dup2`'d onto stdout.
- `pclose()` returns a wait status, not an exit code; `runGit()` goes through `WEXITSTATUS`.
- Existing parsers keep their `\x1e`/`\x1f`/tab delimiters (tested, no reason to churn); `-z` is available to new code (the old NUL truncation is gone).
- Network operations go through the `git` binary, never libgit2 (credential helpers, SSH agent, proxy, hooks).
- `git pull` on a diverged branch needs the reconciliation named: `PullMode::Merge` passes `--no-rebase`, `FfOnly` `--ff-only`, `Rebase` `--rebase`.
- `git push <tag>` can be read as a branch name; `pushTag` pushes `refs/tags/<name>` explicitly and `pushTags` uses `--tags`.
- `--force-with-lease` without an explicit expected value uses the remote-tracking ref as the lease; `PushMode::ForceWithLease` passes the bare flag deliberately. Do not "fix" the refusal into `--force` (it discards work never read). `--ui-sync push-force`.
- The remote picker's default follows the branch's upstream: `updateRemotePicker` keeps the reader's choice while it exists, else prefers the remote `UpstreamInfo::name` starts with, then `origin`, then the first configured remote.
- Toolbar buttons are captioned under the icon (`ToolButtonTextUnderIcon`, one word via `setIconText`); the pull/push `QToolButton`s set the style themselves (a widget added to a toolbar does not follow it). Stash is enabled only with something in the status, Pop only with a stash (`refreshStashes`).
- The pull and push buttons are `QToolButton`s (`setDefaultAction`), not `QAction`s; `updateSyncActions` is the single authority on what is enabled (both the default action and the force entry). The pull mode lives in the button's menu and is a session choice.
- An auto-fetch tick is a background fetch and must not behave like a click: skipped while `m_syncRunning`, only starts if the repository is non-bare and has remotes, and sets `m_silentSync` so a failure lands in the status bar. `onSyncFinished` reads and clears that flag. The timer is per repository (`QSettings ui/autofetch/…`), five minutes, off until
  asked; a selection naming every remote is stored as the empty list. `setAutoFetchIntervalMs` is the test seam.
- Headless network checks use a local bare repository as the remote: `--ui-sync <fetch|fetch-all|pull|pull-merge|pull-rebase|push|push-force|push-tags|push-tag>`, `--ui-set-upstream`, and `--ui-autofetch <enable|disable|select|now> [remote,…]`; `test_remotes` sets up a bare `origin.git` and `backup.git`.
- A clone runs before there is a repository, so it cannot go through `RepoSession`: `git/clone` shells out; `MainWindow::startClone` runs it on `QThreadPool` and marshals back with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`, guarded by `m_cloneAlive`. The CLI does not buy an interactive password prompt (no tty, git's own message).
- A background tab's finished signal must still clear the window's global running flags (per window, not per context): look the context up with `contextOf(session)` on every signal (a closed tab has none) and clear the flag, or the next operation is refused forever. `closingATabWhileItFetchesClearsTheRunningFlag` slows the remote with
  `remote.origin.uploadpack = "sleep 1; git-upload-pack"`.

### Undo / oplog

- The undo log records pointers, not files: `repo/oplog` snapshots `RefState` (every direct ref + HEAD + the index tree) before/after each completed operation; `Repository::applyRefDelta` restores it through one ref transaction, touching only the refs the operation moved. It refuses if the touched refs no longer hold what the operation left.
- A commit or reset moves the same branch's tip, so the working tree is deliberately left alone (changes stay staged). Only a change of HEAD's *place* (another branch, a detached commit) runs a safe checkout, and it runs *first*, before HEAD or any ref moves (`anUndoThatCannotCheckOutMovesNothing`). Content a `--hard` reset or a discard destroyed cannot be
  recovered.
- An operation that stops on a conflict is recorded only when it finishes; its start is a pending state, so an abort (which lands back on that start) records nothing.

### Session and workers

- The window stalled for seconds on a large repository not on its own reads but on the session lock: `isOpen()`/`info()` locked `m_mutex` while a worker held it for a whole history page (0.7 s on 200k commits without a commit-graph), and the GUI asked dozens of times per action. Measure GUI-thread lock waits, not just the cost of each read; `--ui-load-trace` shows the stall, a stack at the long wait shows the caller.
- `SessionSnapshot` is valid while `snapshot->epoch == m_snapshotEpoch`. A write increments the epoch *inside* the lock (`guarded`, `recorded`, `runStash`/`runWorktree`/`runSubmodule`, the direct stage/discard/undo writes, `open`/`close`); `withDirectory` and the git-binary operations (`*Blocking`: fetch, push, LFS, maintenance) also bump after their work, since it runs outside the lock. `installSnapshot` compares under the lock, so a snapshot read across a write is thrown away (and read again, up to four times). A new write path that forgets the bump serves stale data until the next refresh.
- Async reads are a window mode, not a session mode: `MainWindow::setAsyncReadsEnabled` is on only for the interactive launch (and `--ui-load-trace`). Tests and `--ui-*` checks refresh synchronously (`primeSnapshot` then `applyPanels`) so they see what they just did; `test_async_reads` covers the other path. In async mode `refreshPanels()` only asks; `applyPanels()` runs on `snapshotReady` for the active tab, and a snapshot retired by a write since is not applied (that write asked for its own).
- A snapshot reads the libgit2 parts under the session lock and the git-binary parts after releasing it (`Repository::worktreeStatus`, stashes, submodules, remotes, worktrees): `git status` alone is half a second on 300k files, and a diff the reader clicked waited behind it. A write in between moves the epoch and the snapshot is not installed.
- In async mode a refresh does not retire the old snapshot first: reads keep answering from it (instantly, slightly stale) until the new one is installed. A tab switch applies the cached snapshot at once, then reads again; it clears the diff, puts back the tab's own page (graph or file) and re-reads the tab's own file (`aTabSwitchDoesNotKeepTheOtherTabsFile`). `repositoryFingerprint` reads HEAD and file times itself, never the session (it runs on every window activation), and is taken when a refresh is *requested*, or an activation during the first read starts a second one.
- What is not read yet shows as skeleton rows (`ui/skeleton`, an overlay on the view's viewport, still when headless): the branch tree, the graph before its first page (`CommitModel::loadingChanged`), the Commit Panel or the commit's files, the diff, the branch label. `loadingStateForDiagnostics` reports each. A request carries a token (`m_documentToken`, `m_commitFilesToken`); an answer for an older one is dropped, and a tab switch moves both on.

- A session read must not throw into the window: synchronous reads (`info`, `status`, the diffs, `blame`, `commitsBlocking`) go through `RepoSession::readOr`; a failure returns the empty answer and emits `readFailed` (shown as a tab error notification). Emit after the session lock is released, or a receiver that reads the session again deadlocks on the
  non-recursive mutex.
- `RepoSession::headTipId()` must not lock the mutex and then call `info()` (double lock; a non-recursive `QMutex` waits forever with no message). Call `Repository::info()` directly under the one lock.
- Every background task goes through `RepoSession::startWorker`; a fire-and-forget worker must not emit on a destroyed session (`m_alive` read from freed memory still reads true). `~RepoSession` waits on an in-flight count (`m_workersInFlight` + `m_workersDone`, held by an RAII `WorkerSlot` so an early return cannot leak it), not under `m_mutex` (or a
  worker's own `info()` deadlocks behind the destructor). A context's session is owned through `RetireSession`/`retire()`: idle deleted at once, busy marked retired, the last `WorkerSlot` queues `deleteLater` after the finished signal it posted.
- `RepoSession::resolveRevision` takes anything git understands, so a check cannot disagree with git about what a revision names.

### Commands, keymap and menus

- A command must not be run by reference to its registry entry: running it refreshes the window and rebuilds the registry, destroying the closure mid-call. `runCommand` copies the `std::function` first; `installCommandShortcuts` retires old `QShortcut`s with `deleteLater` (the one being replaced may be the one emitting). An ASan build (`-fsanitize=address`)
  catches the heap-use-after-free.
- A command is data before it is a button: `ui/command_palette.{h,cpp}` holds a `CommandRegistry` of `Command{id,title,category,shortcut,enabled,run}`; the window rebuilds it every refresh (one branch command per branch). The palette *chooses* but does not *run* (records the id; the window runs after `exec()` returns, so a command that opens a dialog does
  not open under the modal palette). Do not grow a second list of commands. `--ui-command <query> [run]`.
- A keymap that only changes what the palette shows changes nothing: shortcuts are hardcoded on actions and `QShortcut`s. The `MainWindow` keeps `id → QAction*` (menu commands) and `id → QShortcut*` (the rest); `refreshCommands()` (build + rebind) replaced every `buildCommands()` call. An empty override means "unbound", not "default"; `reset` returns the
  default. `Ctrl+P` and `Esc` are outside the registry and reserved.
- Menu entries carry their own key in `QAction::data()`; the dispatcher switches on that key, not pointer identity.
- A `QMenu` submenu is not the menu: `QMenu::addMenu()` returns a submenu, and adding entries with the parent helper puts them one level up. `buildCommitMenu` keeps two helpers, `add` (top level) and `addTo` (explicit target). `--menu` prints the resulting tree.
- A popup menu cannot be driven from a test; `--menu` prints its contents (enabled/disabled, keys). Same idea as `--probe`/`--shot`: make the thing observable headlessly.
- A headless `--ui-*` path that can open a modal must be in `main.cpp`'s headless condition, or the run hangs (the watchdog only closes modal widgets once `setHeadless(true)` has run). `--ui-hooks` and `--ui-sign` included.
- An operation with several entry points must be wired at all of them: interactive rebase existed in the commit `QMenu`, its fallback and the palette while the badge list's entry was disabled and the view's `case RefOperation::InteractiveRebase` was an empty `break;`. `test_badge_flight` asserts the entry is enabled and reports itself.
- The window's own paths need a headless way in: `--ui-commit-op <key> <ref>` runs one commit-menu entry exactly as the menu does; `--ui-branch-drop` the badge. `setHeadless` answers confirmations and closes modal dialogs as they appear, recording their text (a modal wait in a headless run is a hang).
- The daily cycle has a headless way in: `--ui-commit-panel <message>` stages every change and commits through the same calls the Commit Panel's buttons reach (`commitPanelForDiagnostics`), then prints HEAD and the remaining status. The panel's signals are in `test_panels`; the session calls they land on are in `test_staging`. `--shot` with no `--select-row`
  shows the opening view (working tree and commit panel).
- Headless flag argument traps: `--ui-branch-drop <src> <target> [pick]`, `--ui-visibility <op> [branch]`, `--ui-worktree`, and `--ui-rebase-editor <base> [plan]` all eat every following word that is not a flag. Pass the repository path first, or leave the optional argument out.

### Drop UI and badge flight

- A popup shown from inside a drop is closed by the release that ended the drag: the operation list beside a lifted badge is a child of the viewport, clamped inside it (not a `Qt::Popup`), and the panel ignores a focus change for the first quarter second after it is shown (focus returning to the view is part of ending a drag).
- A list that opens beside a badge near the edge cannot be a child of the viewport (children are clipped); it is a `Qt::Popup` in global coordinates, which also gives outside-click dismissal.
- A drag over a branch label says where it would land: `dragMoveEvent` sets `m_dropTarget` (the pill from `rowPills`), painted as a pulsing outline plus "Release: merge / rebase onto X"; cleared on leave, drop and off-label. `dropTargetForDiagnostics`; `aDraggedBranchShowsWhereItWouldLand`.
- A badge parks beside the other badge by geometry, not text: `parkedBadgeCentre` is derived from the target pill's rectangle, and the same `badgeWidthFor` feeds both placement and painting.
- A badge lifted off its row must always come home: `ui/badge_flight.{h,cpp}` is the state machine (lift, park, land, land again, land before any lift, land mid-flight) with no widgets in it; `landImmediately()` covers paths where no animation can run (the view hiding, the window going away).
- A panel the reader must answer reports exactly one signal (answered, dismissed, closeEvent, outside-click), guarded by a flag, or the badge lands twice or never.
- There are two drop menus, and the prediction belongs in both: the badge list (`GraphOperationPanel`, primary) and the window's `QMenu` (fallback, ref-tree). The view holds no `RepoSession`; it emits `dropOperationsOpened` and the window answers with `setDropPreview` synchronously (the list measures around the line before it is shown).
- A list label that says the opposite of the handler is a bug with no compiler: labels are derived from `(source, target)` order.
- `QCursor::pos()` is not reliable while a drag is in flight; the drop event carries the position, recorded as received (`ui/drop_point.h`). Both drop menus (`refDroppedOnRef`, `refDroppedOnCommit`) use it. Testing the protected `dropEvent` needs a subclass (a synthetic `QDropEvent` sent to the viewport is not routed in).
- Qt draws nothing while dragging unless the `QDrag` has a pixmap: `RefsDelegate::dragPixmap()` renders the same chip the label column draws.
- A `connect()` to an object that does not exist yet only prints a warning; wire after the object exists, and test the chain (a signal that fires is not the same claim as a button that works).
- A widget added to a layout before it was created is a stray child in the corner (`addWidget(nullptr)` only warns). Test the layout claim (position/width), not `!isHidden()`.
- A widget's minimum width is the window's: a `QLabel` holding a long path, a row of worded buttons, or a page hidden in a `QStackedWidget` (which asks every page for its minimum) all forced the window wider, and it jumped as files opened. Paths go in an `ElidedLabel`, long button rows in a `FlowLayout`, the diff toolbar drops its words below its full width (`DiffView::fitToolbar`), and `sizeToCurrentPage` gives hidden pages an `Ignored` policy. Check `minimumSizeHint()` of the window after a change, not just a screenshot.
- A side panel folds by its splitter size, not `setVisible`: QSplitter hides a hidden widget's handle with it, and the handle (`ui/panel_splitter`, an arrow button halfway down) is the way back. `PanelSplitter::isPanelCollapsed` is the state (not `sizes()`, which is all zeros before the window is laid out); the diff page folds the branch rail through the same call (`m_foldingPanel` keeps that from reading as the reader's choice). A test looking for the window's columns asks for the `PanelSplitter`; the merge editor has a three-pane splitter too.
- `isVisible()` is false for any widget whose window has not been shown; for "is this panel up" ask `isHidden()`.
- Two layouts on one widget is a `QLayout` warning, not a compile error; a nested layout goes in with `new QFormLayout` (no parent) + `addLayout`.
- `slots`, `signals` and `emit` are Qt macros; naming a local variable `slots` breaks compilation with a nonsense error.
- `QWidget::event()` returns bool, not void.
- `QAbstractItemView` emits `clicked` even when the delegate consumed the release; `FileActionDelegate` records the click (`takeButtonClick`) and the panel's `clicked` handler drops it. The action runs on the next turn.

### Working-tree (WIP) node

- The WIP row lives *inside* the model's row list at index 0 (`CommitModel` stores the synthetic `//wip` in `m_rows[0]`), keeping `commitAt`, `rowForCommit`, the delegate and the drop paths stable; only the paging arithmetic learns it (`realRowCount()` is what `fetchMore` asks from and `onPage` checks a page against). `setWorkingTreeChanges`
  inserts/updates/removes the row from status, and its parent is HEAD read from the refs (not the loaded rows). A branch dropped on it is refused; selecting it shows the working tree; `selectedCommitIds()` skips it. `--ui-context-menu 0 0`.
- The WIP node wears the colour of the branch it sits on: `CommitModel::rebuildTipColors` gives it the parent tip's colour, so it is called again from `setWorktreeWip` (not only `refreshRefs`). `GraphDelegate` draws the `WorkDir` ring in `node->colorIndex`, not `theme::accent()`. `test_wip_node` paints the row and reads the ring pixel back; its worktree case
  adds a third branch.

### Commit panel and widgets

- The Commit Panel is its own widget stacked with the commit view; the stack's minimum width is the largest page's (`ui/working_tree_panel` + `CommitDetailsPanel`). `bothFacesShareOneMinimumWidth`.
- The drag handle is one widget: `ui/grip_splitter` (hairline + grip) is used by both faces. `CommitDetailsPanel` splits the page into the message and everything else; the author block, file-list controls and files are **one** splitter child; `setStretchFactor` is 0/1.
- The file-list sort is a `QToolButton` (objectName `commitSort`) cycling Path A→Z, Path Z→A, Group by status, with `Icon::SortAscending`/`Icon::SortDescending`/`Icon::Folder` and a tooltip naming the current and next order; it has no menu. `theCommitPanelHasAGripUnderTheMessageAndAnIconSort`.
- `size` is already a `QWidget` property (`setProperty("size", "small")` fails silently); the QSS key is `buttonSize`.
- A themed icon cannot be a stylesheet image (embedded SVGs carry `stroke="currentColor"`, which Qt does not resolve, and a stylesheet image is never retinted). `rebuildTabBar` installs its own `QToolButton` (objectName `tabClose`, `setThemedIcon(Icon::Close)`) per tab, finding its tab by comparing `tabButton(i, QTabBar::RightSide)` at click time (the strip
  is movable).

### Theme and icons

- The app icon has one source, `packaging/icons/gitmycelium.svg` (window icon, Linux install, AppImage). The macOS `.icns` is built in CI from `packaging/icons/gitmycelium-1024.png`, which must be regenerated whenever the SVG changes, at Apple's 824-on-1024 grid: `magick -background none -density 1200 packaging/icons/gitmycelium.svg -resize 824x824 -gravity center -extent 1024x1024 -depth 8 PNG32:packaging/icons/gitmycelium-1024.png`. A stale PNG showed the old icon in the Dock while the running window showed the new one.
- Chrome icons are embedded SVGs: `theme/icons.{h,cpp}` maps `gk::Icon` to Lucide (ISC, `third_party/lucide`) SVGs, recoloured at render time (the colour is substituted before rendering and overwritten with `SourceIn`). Because `gmc_theme` is a static archive, `theme/icons.qrc` is added to every executable that renders icons, not to the library.
- The base font and corner radius are theme facts, not per-widget ones: the application font is set once in `main.cpp`, the QSS carries no global `font-size`, and geometry tokens `theme::radius()`, `controlHeight()`, `rowHeight()` exist so a hand-painted chip and a styled button agree.
- The look follows the opencode TUI: one monospace family for the whole window (`theme::uiFontFamily`, set on the application font, not in the QSS), `background` for the canvas, `panel` for side panels and dialogs, `surfaceAlt` for elements. A branch label is measured and painted with `theme::labelFont()` everywhere (`RefPillLayout`, the delegate, the reveal overlay, the drag chip); a font derived from `pointSizeF()` was 7pt and disagreed with the hit test. Branch colour 0 is not the accent (`theWorkingTreeRingWearsTheBranchColour` must be able to tell them apart).
- The application font is set in pixels, so `pointSizeF()` is -1 and scaling it asks Qt for a negative size. Scale `pixelSize()` when set, else `pointSizeF()`, and drive the test through a pixel-sized application font.
- The theme is mutable global state: `gk::theme` holds the active `ThemeMode` and zoom and every accessor reads the active set. The default is **Dark** (a test changing the mode must put it back), and anything that cached a colour or pixmap must be rebuilt by `MainWindow::applyAppearance()`. Saved keys are `ui/theme` and `ui/zoom`.
- A `QIcon` does not recolor itself, so chrome icons go through a registry: `ui/themed_icons` remembers each icon with its `IconTint` role and `retintThemedIcons()` redraws it after `theme::setMode`. Build long-lived chrome with `setThemedIcon`/`addThemedAction`; transient menus are fine.
- Zoom is one scale: `theme::setZoomPercent` scales the base font and `radius`/`controlHeight`/`iconSize`/`rowHeight`. Use `theme::rowHeight()`, `theme::monoFontPixelSize()`/`baseFontPixelSize()` and `theme::scaledPixels()`; a hardcoded row height or a literal `font-size` clips text.
- QSS placeholders are replaced by exact string; `%(radiusSmall)px` does not match a replace of `%(radiusSmall)s`, and an unregistered `%(…)` is left in the sheet and fails to parse. Register every new placeholder with `)px` and grep `theme::styleSheet()` for `%` before trusting it. `QToolButton[variant="menu"]` (`min-width`, `padding-right`,
  `%(controlHeight)px`) reserves room for the pull/push arrow.
- An avatar's colour is a hash of the author, not `qHash`: `theme::authorColor` takes FNV-1a of the lower-cased email (name when none) to a hue. The graph node is that colour inside a ring of the branch's colour.

### Settings and tabs

- `QSettings` is the one source of truth for preferences; `ui/settings_store` is a facade so the dialog and the window cannot build the same key two ways. The key shape is `ui/<group>/<commonDir>` (and `ui/<group>` with no repository) — exactly what the window wrote before. The dialog writes only on accept and the window re-reads. rerere is the exception (it
  lives in the git config).
- A settings store that re-reads its table must move the old key, not copy it twice (the pins/visibility JSON would grow a second copy a later write cannot tell from a real entry).
- One window, many repositories: `RepoContext` owns its session, `CommitModel`, `RefModel`, pins, visibility, and the per-repo scalars (selection, rendered path, reveal, compare, filter, diff codec). `m_session`/`m_commitModel`/`m_refModel` and those scalars are the working copy loaded/saved by `adoptContext`/`saveActiveState`. Anything per-repository that
  is a scalar must be added to both, or it leaks between tabs; `test_tabs`.
- A context owns its models, so detach views before dropping it (`setModel(nullptr)` in `~MainWindow`, `closeContext`, `keepOnlyContext`, `closeAllContexts`).
- Rebinding a view to the *same* model must be a no-op: `QAbstractItemView::setModel` returns early when the model is unchanged, so disconnect-then-`setModel` loses Qt's own `rowsInserted`/`modelReset`/`dataChanged` slots. Guard `CommitGraphView::setModel`, and make `currentRowChanged` with `Qt::UniqueConnection` (the selection model survives a rebind).
  `test_graph_view_bind`; `--shot` on a dirty repo is the regression check.
- Flushing the debounced column save on a switch must know the outgoing context was primed (`active()->primed`), or opening into the empty starting context writes the default layout over the saved one.
- A tab's identity is its checkout path, not the common git dir (two worktrees are two tabs). Restoring a session opens only the *active* tab; the rest are paths with no session until activated.
- Tab persistence is off unless the interactive application asks: `saveTabs` writes only when `m_tabsPersistence` is on, the run is not headless, and the window is visible. Aliases are stored per repository (`ui/tabalias/<commonDir>`). `--ui-tabs list|open|activate|close|rename`; `test_tabs`.
- A tab is named by the repository it holds (`context.path`), not by whether it has been read: a restored tab knows its path before its session. "New Tab" means *no path at all*, and `onNewTabRequested` reuses a tab with an empty path (never merely an unopened one). `activateContext` relabels the active tab once more (the open and the alias). The empty
  starting context is on the strip once the constructor calls `rebuildTabBar()`. `tabsForDiagnostics` reports the strip's own text (`bar N`) beside the context label (`tab N`); `theEmptyStartingTabIsOnTheStrip`, `restoredTabsCarryTheirNamesBeforeTheyOpen`, `thePlusDoesNotReuseARestoredTab`, `aRememberedAliasNamesTheTabThatOpens`.
- The tab strip is a toolbar added first with `addToolBarBreak()` so the main toolbar is laid out below it; `theTabStripIsAboveTheMainToolbar`.
- A tab with no repository shows the start page, not an empty graph. The middle stack has three faces (history, conflicts, start); `showEmptyTab` selects the third and fills it from the recent store. The start page holds no session: it emits what the reader asked for (open / clone / create / a recent row) and the window performs it.
- "Last used" is open and tab activation, nothing finer: the recent store is touched in `activateContext`, not from `refreshPanels` and not on every git call. Writing it follows the tab-set rule (off when persistence is off or the run is headless).
- Creating a repository is a local write, so it is libgit2, and it is a `static` on the session: `Repository::init` uses `git_repository_init_ext` with `MKPATH | NO_REINIT`; `RepoSession::createRepository`. The repository is created and closed again; the caller opens it through the normal context path.
- An unborn branch reports no `headBranch` (libgit2's `git_repository_head` fails until the first commit); do not invent a branch name in `info()`.

### Signing and hooks

- libgit2 signs nothing: `git_commit_create` writes an unsigned commit even with `commit.gpgsign=true`. A signed commit or amend goes through the git binary (`Repository::createCommit(msg, sign)` / `amendHeadMessage(msg, sign)`), which knows `gpg.format`, `user.signingkey`, `gpg.program` and the agent.
- libgit2 runs no hooks, so a repository with a commit hook commits through the binary. `createCommit`, `commitMerge`, `commitSquash` and `amendHeadMessage` take `commitWithGit` when signing is on *or* `pre-commit`/`prepare-commit-msg`/`commit-msg`/`post-commit` is enabled (`commitHooksEnabled`, via `HooksOps`). The commits that finish an operation read
  `commit.gpgsign` themselves (`gpgSignConfigured`). `git commit` also consumes `MERGE_HEAD`/`SQUASH_MSG`, so the same call finishes a merge or a squash. The message is passed verbatim, no trailing newline added.
- A signed reword must keep the commit's tree: `git commit --amend --only` (no paths) keeps it, and `--cleanup=verbatim` keeps the message byte for byte. A test that stages a change before amending catches a rewrite that folds it in.
- `git commit -S` reads gpg status, not just exit code: a stand-in `gpg.program` must print `[GNUPG:] SIG_CREATED …` on stderr or git reports "gpg failed to sign the data". Stronger: a generated SSH key (`gpg.format=ssh`, `gpg.ssh.allowedSignersFile`); only a real signature makes `%G?` answer `G`, and a `gpgsig` git cannot parse answers `N` — assert `==
  "G"`, not `!= "N"`.
- `%G?` runs the signature check (invokes gpg/ssh); never ask it while walking history or painting the graph. `Repository::commitSignature` is called once for the selected commit; `%GS`/`%GK` name signer and key (empty for unsigned).
- A hook's directory is `git rev-parse --git-path hooks` (honours `core.hooksPath` and a linked worktree); `.sample` is not a hook; state is the executable bit. Toggling a hook writes the filesystem, so it is `guarded`, never recorded. `--ui-hooks` and `--ui-sign` open a modal, so both are in `main.cpp`'s headless condition.

### Commit message

- `runGit` has no stdin, so `git interpret-trailers` works on a temp file: build the co-author block with `--in-place --if-exists addIfDifferent` (stops the same person twice) over a file under the git dir, then read it back. The result goes into the message field, not beside it.
- `commit.template` is a path and `configValue` does not expand it: read with `git config --path --get` (`--path` expands `~/`) and resolve a still-relative value against the work tree. The template only fills an *empty* message field.

### External tools and terminal

- The external tool choice is git config, not `QSettings`: `diff.tool`/`merge.tool` via `ExternalToolOps`. The run must **not** hold the session mutex (a tool stays open for minutes): `RepoSession::runExternalDiffTool` reads `info()` once then shells out, and the window puts the call on `QThreadPool` (`m_workerAlive`).
- A custom `difftool.<name>.cmd` is exported (`$LOCAL`/`$REMOTE` in the environment); a custom `mergetool.<name>.cmd` is only eval'd (`$BASE`/`$LOCAL`/`$REMOTE`/`$MERGED` defined in git's own shell, so a script sees them empty and the copy must be inline, e.g. `cp "$LOCAL" "$MERGED"`). Both need `--no-prompt`; `mergetool.<name>.trustExitCode=true` makes git
  take the tool's exit code.
- Every terminal emulator spells "start in this directory" differently: `buildTerminalLaunch` gives known ones their flag (konsole `--workdir`, gnome-terminal/xfce/tilix/alacritty `--working-directory=`, kitty `--directory`) and everything else a portable `-e sh -c 'cd "$1" && exec "${SHELL:-sh}"'` (directory as an argument, so spaces need no quoting). An
  unrecognised `$TERMINAL` that does not resolve is skipped. `--ui-terminal` prints the chosen command without launching it.

### Updates

- The updater reads `update.json`, which the release workflow writes beside every release (`releases/latest/download/update.json`): version, `github.run_number` as the build, and per platform (`linux-x86_64`, `macos-arm64`) the URL, size and sha256. A same-version re-run is an update because the build moved; `isNewer` compares version, then build. `GMC_BUILD_NUMBER` (CMake cache, 0 locally) feeds `--version`, the About box and macOS `CFBundleVersion`. macOS keys its icon and launch caches on bundle id + `CFBundleVersion`, and two builds that both said `1.0.0` showed the old icon and launched an older copy.
- Nothing is replaced until the download matches its sha256. An AppImage (`$APPIMAGE`) is downloaded beside itself and `rename(2)`d over (the running one keeps its mount). A `.app` is copied out of the mounted DMG with `ditto` into a hidden sibling, `codesign --verify`'d, and swapped by a detached `/bin/sh` that waits for our pid to exit (frameworks and plugins load lazily, so a bundle must not change under a running process). A copy that is neither only links to the release page.
- A DMG must carry no license agreement: CPack turns `CPACK_RESOURCE_FILE_LICENSE` into one under the pre-3.24 policy (`cmake_minimum_required(3.21)`), and `hdiutil attach` then waits on stdin for "Y" — the app sat at 100% until the 2-minute tool timeout. `CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE OFF`; `runTool` still feeds "Y" and closes stdin. The release runs the whole path on both platforms (`--update-install` from a `file://` manifest naming the package just built) and checks `hdiutil imageinfo` for no agreement.
- The check is network I/O, not git: `QNetworkAccessManager`, so packages must carry a TLS backend. The release smoke test runs `--update-check` from inside the package and accepts a manifest or the 404 of a release without one; anything else (no TLS) fails. Only the interactive launch checks (`setUpdateChecksEnabled`), at most once per 20 h (`ui/updates/last-check`), off with `ui/updates/check`. `GMC_UPDATE_MANIFEST` points it at a `file://` manifest (`test_updater`, `test_update_dialog`).

### Platforms and packaging

- macOS is a supported target: the bundle is CMake's `MACOSX_BUNDLE` (`GitMycelium.app`, icon `gitmycelium.icns`); `macdeployqt` must run; the bundle is signed ad-hoc (`codesign --force --deep --sign -`, no Apple account) *before* `cpack -G DragNDrop`, so `CPACK_STRIP_FILES` is OFF on Apple (rewriting the binary after signing invalidates it). The `.icns` is
  built in CI from `packaging/icons/gitmycelium-1024.png` (`iconutil` is macOS-only). A downloaded copy is quarantined and not notarized: since macOS 15 right-click → Open no longer passes Gatekeeper, the reader allows it once in System Settings → Privacy & Security → Open Anyway (or `xattr -dr com.apple.quarantine`). README "Opening the app on macOS" is the one place that says how; notarization needs a paid Developer ID.
- QScintilla is GPLv3 (do not add without an explicit decision); Qt 6 itself is LGPLv3 (link dynamically only).
- The name a person reads is `GitMycelium`; on disk it stays `gitmycelium`. Window titles, the start page, the About box, the stash placeholder, the merge-commit message, the reflog messages and the fallback signature say GitMycelium; the executable, the `.desktop` `Exec`/`Icon`/`StartupWMClass`, the macOS `OUTPUT_NAME "GitMycelium"` bundle and internal
  paths (`gitmycelium-rewrite`, `gitmycelium-sign`) are lowercase. `QCoreApplication::setOrganizationName`/`setApplicationName` are **not** touched (the settings dir/file — changing either resets every preference); `setApplicationDisplayName` is the knob. `theWindowIsNamedGitMycelium`, `theTitleNamesTheApplication`.

### Windows

- A Windows child gets one command line, which it splits again: `runGit` builds it with the C runtime's quoting (`appendQuoted`: quote when there is a space, tab or quote; double backslashes only before a quote) and passes UTF-8 arguments as UTF-16. `argumentsArriveExactlyAsGiven` round-trips spaces, quotes, trailing backslashes and Cyrillic through a `printf` alias.
- Handle inheritance on Windows is all-or-nothing per process, so `runGit` names the three handles the child may inherit (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`); otherwise a git spawned by another thread keeps our pipe's write end open and the read never ends (the POSIX twin is `closeInChild`). The child has no console (`CREATE_NO_WINDOW`), so a credential or host-key question fails instead of waiting.
- Paths are UTF-8 `std::string`s everywhere below Qt; on Windows that is true only because every executable carries `packaging/windows/gitmycelium.manifest` (`activeCodePage` UTF-8, Windows 10 1903+), which makes the ANSI code page UTF-8 for `std::filesystem`, `fopen` and `argv`. A new executable target needs the manifest (tests get it from the loop at the end of `tests/CMakeLists.txt`). MSVC needs `/utf-8` for the literals.
- Windows refuses to replace a file while a view of it is mapped, and `git commit-graph write` renames over the graph we read: `CommitGraph` reads the file into memory there instead of `mmap`.
- git for Windows runs any hook file that exists (no executable bit): a disabled hook is parked as `<name>.disabled`, and a freshly written hook is parked, as a fresh file on POSIX is not executable (`hooks.cpp`, `runsAsHook` in `test_hooks`).
- The watcher on Windows is one recursive `ReadDirectoryChangesW` per root (the work tree, and a git dir outside it), classified by the same `WatchRoots::classify` as FSEvents. A zero-byte result is an overflow and reports git state. wine does not deliver subtree changes, so only a real Windows (CI) checks it.
- `QSettings` on Windows is an INI file (`setDefaultFormat` in `main`), not the registry: keys carry repository paths, which the registry compares without case and splits at every slash. Tests on Windows get an INI store per process from `tests/windows_settings_sandbox.cpp` (`XDG_CONFIG_HOME`, how a Linux test isolates it, means nothing there, and the registry is shared by every test running at once).
- libgit2 finds Git for Windows' system config through `PATH` and ignores `GIT_CONFIG_NOSYSTEM`; with `core.autocrlf=true` there it wrote CRLF where the binary, told to skip that config, did not. `ensureInitialized` empties the system and ProgramData search paths when the variable is set.
- `core.filemode=false` (what `git init` writes on Windows) means no file is executable: `untrackedFileDiff` asks `trustsFileMode`.
- The Windows release is a folder (`GitMycelium.exe` + `windeployqt` + `git2.dll` + `platforms/qoffscreen.dll` for headless runs) in a zip. `detectInstallTarget` treats a folder with `Qt6Core.dll` beside the executable as an installed copy; the update unpacks with Windows' own `tar` into `.<folder>.update`, and a detached PowerShell script swaps it in after our process exits (a folder holding a running executable cannot be renamed).
- The executable is a GUI-subsystem program; `attachParentConsole` in `main` attaches to the console it was started from, so `--probe` and friends print in cmd. Output already redirected is left alone.
- A Windows build can be checked on Linux: `clang-cl` + `lld-link` with the MSVC CRT and SDK from `xwin`, Qt for `msvc2022_64` from aqtinstall (`QT_HOST_PATH` pointing at a matching Linux Qt), libgit2 cross-built the same way, and the tests run under wine with PortableGit on `WINEPATH`. wine's msys runtime prints a "Cygwin WARNING" on every shell git starts, so tests that read git's stderr or run shell scripts (rebase, octopus merge, difftool) fail there and only there.

### Build, Qt, C++ and test hygiene

- `core` and `git` must not include Qt headers; this is what keeps the git layer testable and UI-agnostic.
- `QStandardItemModel`/`QFont` live in Qt6::Gui, not Qt6::Widgets; targets that touch them must link `Qt6::Gui`.
- Qt Test `init()`/`cleanup()` must be private slots, and each test needs its own fresh repository (a shared `QTemporaryDir` silently breaks `git commit`).
- `&parseOid(x)` does not compile (`parseOid` returns by value); bind it to a local first.
- A widget that is never shown has a viewport a few pixels wide, and a click aimed at a row lands outside it (silently selecting nothing). Tests that click need `resize()`, `show()` and `qWaitForWindowExposed()`.
- `--shot` grabs on the second event-loop turn: `QTextEdit` lays its document out lazily, so grabbing in the same turn captures an empty diff pane.
- A check that compares two sides but not the starting point can pass on nothing: also compare against the untouched repository and say "nothing happened" instead of "OK".
- A refactor is only as safe as the fixture. Check expectations against real git: a merge into a detached HEAD is allowed; a branch another branch also points at can be deleted (git's rule looks at HEAD, not reachability); nested branch names are refused by git.
- Cherry-picking onto the same parent within the same second produces the same commit id (author, message, tree, parent, timestamp all match); tests must assert authorship and content.
- `git reset --soft` keeps the index, `--mixed` keeps the working tree — do not reset hard between assertions about them.
- An untracked `*.patch` inside the repository shows up in `git status`; write test patch files into a second temp dir.
- A patch test cannot assert the commit id (`git am` preserves author/author-date but stamps the committer to now).
- Measure with a histogram, not by dumping pixels (`magick ... txt:-` output for a normal-sized region is truncated, silently turning a pixel count into a count of the first few kilobytes).
- Maintenance mutates the object store and the oplog restores pointers: gc/fsck go through `guarded`, never `recorded`, and run long, so they belong on a worker with the graph's busy sweep, never on the GUI thread.
- The multi-pack-index has nothing to write while every object is loose: `git multi-pack-index write` refuses with "no pack files to index", so `Optimize` runs `gc` first. The other tasks are git's own verbs (`commit-graph write --reachable --changed-paths`, `maintenance run`); a refusal is returned as git's output, not a thrown error.

## Testing expectations

- Every git-layer behavior gets a test in `tests/` using a real temporary repository created with the `git` binary — no mocks of
  libgit2.
- Tests must not depend on the machine's git config: `tests/CMakeLists.txt` points every test at its own `GIT_CONFIG_GLOBAL` with a test identity. A CI runner has no `user.name`, and the window's `git merge`/`git am`/`git commit` failed with "Committer identity unknown" while the fixtures' own commits (which pass an identity) succeeded. It sets `GIT_CONFIG_NOSYSTEM=1` (the Ubuntu runner's git-lfs lives in `/etc/gitconfig`, so `lfsConfigured()` was true before `install` and a clone smudged content a test expected as a pointer; reproduce with `GIT_CONFIG_SYSTEM=<file with filter.lfs>`) and `QT_QPA_PLATFORM=offscreen` (a runner has no display), so a test that reaches a confirmation must call `MainWindow::setHeadless(true)` or it waits on a dialog forever. Check with `env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR=<empty dir> HOME=<empty dir> ctest`: unsetting `WAYLAND_DISPLAY` alone is not enough, Qt still finds `wayland-0` in `XDG_RUNTIME_DIR`.
- Keep `--probe` working; it is the fastest way to inspect real repository state and is used as a CI smoke test alongside `--shot`.

## Style

- C++20, `std::` containers, no exceptions crossing Qt signal/slot boundaries (`GitError` is caught in `RepoSession`).
- 4-space indent, `m_` prefix for members, `gk` namespace for all first-party code.
- Warnings are on (`-Wall -Wextra`); keep the build warning-clean.
- **Code is self-documenting; comments are the exception.** No comment that restates what the code already says. A comment is
  allowed only to explain logic that is genuinely confusing — and if logic needs a comment, prefer decomposing it into well-named
  functions over explaining it, without losing performance or behaviour. This "known pitfalls" section is the exception that proves
  the rule: it records a *why* the code cannot show.
