# Changelog

All notable changes to GitMycelium are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- **Windows.** A release archive for Windows x86_64 (10 version 1903 or newer):
  a folder that runs where it is unpacked, updated from inside the app like the
  AppImage and the macOS bundle. git runs without a console window, the repository
  watcher follows the whole work tree (ReadDirectoryChangesW), Open Terminal opens
  Windows Terminal, PowerShell or cmd, and hooks are switched off by parking them as
  `<name>.disabled` (Windows has no executable bit). Paths outside ASCII work: every
  executable runs with UTF-8 as its code page. Preferences are an INI file under
  `%APPDATA%`.

### Fixed

- An untracked file in a repository with `core.filemode=false` was shown as
  executable when the file system said so.
- With `GIT_CONFIG_NOSYSTEM` set, libgit2 still read the system git config that the
  git binary skipped, so the two could disagree (on Windows about line endings).

## [1.2.0] - 2026-10-14

### Added

- **The selected commit's branch is traced in the graph.** Its line, from the
  branch tip down, is drawn wider and lighter with a glow, and its commits get a
  halo. Below the point where the branch was cut the trace follows the branch it
  came from, and from there that branch's own origin, so each ancestor branch is
  lit only from its fork down.
- **Side panels fold from a button.** The handle beside each side panel carries an
  arrow that folds the panel away and brings it back; a folded panel leaves its
  handle at the window edge. View → Details panel joins View → Branches panel.
- **Local ignore rules.** "Ignore… → Only for me" hides a file without touching the
  shared `.gitignore`: an untracked path goes into `.git/info/exclude`, a tracked
  file is hidden with skip-worktree (nothing is staged for anyone). Repository →
  Local Ignore Rules… lists both and adds or removes them.
- **Dragging a branch shows where it will land.** The label under the pointer
  pulses, with what releasing there offers beside it.
- Toolbar buttons are named under their icons.

### Changed

- The graph column opens at most five lanes wide; drag the header for more.
- Commit circles are larger, and their initials are sized to fit inside them.
- **The trunk is the straight line.** The default branch (`origin/HEAD`, else
  `main`/`master`/`trunk`) takes the leftmost lane and keeps it where branches
  meet; a branch cut from it bends into it at the fork instead of the trunk
  appearing to branch off the checked-out line.
- A stash is one node in the graph: the commits git keeps the index and the
  untracked files in are no longer drawn beside it. Every stash is drawn, not only
  the newest, and a stash is not a commit: its menu offers apply, pop and delete,
  and branches cannot be dropped on it or dragged from it.
- Remote branches whose tip is a commit on the main branch's own line (a release
  branch cut at a merge, a branch merged by fast-forward) no longer hang labels on
  the main branch; they stay in the branch tree. View → Show Merged Remote
  Branches brings the labels back.
- The hover list behind `+N` names a local branch once, with the cloud, instead of
  listing its `origin/` twin as another branch.

### Fixed

- **Opening an untracked file in the working tree took a second** on a large
  repository: the diff walked the whole work tree. It is now read from the file
  itself (about 20 ms on 300k files). Reading the index no longer forces a reload
  of an unchanged index file (170 ms each on a 40 MB index).
- **Checking out froze the window.** The checkout ran libgit2 on the GUI thread
  (1.6 s on 300k files, far longer on macOS). It now runs `git switch` (0.35 s) on
  a worker, and the window stays responsive.
- **The window grew wider while files were opened.** The file path above the diff
  asked for its full width, the diff toolbar for all its words, and the hidden
  merge editor for a 1600 px row of buttons. The path is elided, the toolbar drops
  its words when narrow, the merge editor's buttons wrap, and a hidden page no
  longer sets the window's minimum width. The side panels can now be widened as
  far as the window allows.
- **Changes made outside the window did not show up** until the repository was
  reopened. The watcher saw at most 512 directories and never a file's content
  changing; it now watches the whole work tree (inotify on Linux, FSEvents on
  macOS), leaves out what git ignores, and the refresh reloads the graph whenever
  a ref or HEAD moved — a stash, a commit or a checkout made in a terminal, and
  the stash commands inside the window, which left the graph stale. The file open
  in the diff is read again when it changes on disk.
- A refresh no longer throws the reader off the commit or comparison they are
  reading, nor off the working-tree file they chose. A refresh caused by a saved
  file costs about 10 ms of the window's time on 7000 refs (it was about 90): the
  branch tree and the labels are rebuilt only when the refs moved, and a linked
  worktree's status is read on the worker.
- Switching tabs kept the previous tab's file in the diff.
- A hover badge on a commit of a merged (and deleted) branch named the branch it
  was merged into; it now names only the line the commit is drawn on.
- The selection halo of a commit in the first lane was cut off on the left.
- A stash of new files showed no files: they live in the stash's third parent.
- Stash and Pop are greyed out when there is nothing to stash or pop.

### Downloads

- **Linux x86_64:** `GitMycelium-1.2.0-x86_64.AppImage`.
- **macOS (Apple Silicon):** `GitMycelium-1.2.0-arm64.dmg`.

## [1.1.2] - 2026-10-13

### Fixed

- **The macOS update stopped at 100%.** The disk image carried a license
  agreement, and mounting it waited for someone to accept it. The image no longer
  has one, and the updater answers the question if an image ever asks it. The
  update dialog now says when it is verifying and installing after the download.
- The release checks the in-app update on both platforms against the package it
  just built (`--update-install`).

### Downloads

- **Linux x86_64:** `GitMycelium-1.1.2-x86_64.AppImage`.
- **macOS (Apple Silicon):** `GitMycelium-1.1.2-arm64.dmg`.

1.1.0 and 1.1.1 can update to this release from inside the app: the agreement was
in the image, not in their updater.

## [1.1.1] - 2026-10-13

### Fixed

- **Lag on large repositories.** Switching tabs and finishing any action could
  freeze the window for seconds. The GUI thread waited for the repository lock
  while a background history page held it, and every refresh re-read refs, status,
  stashes, worktrees, submodules and remotes on the GUI thread. A refresh now reads
  all of that in one pass on a worker, and the window answers from that snapshot
  until the next write. A commit's files and a file's diff load in the background
  too. On a 200k-commit, 100k-file, 3000-branch repository, opening it stalls the
  window for at most ~15 ms; switching to it takes ~100 ms.

### Changed

- What has not been read yet shows as skeleton rows (branches, the graph before
  its first page, the Commit Panel, a commit's files, the diff, the branch label)
  instead of an empty or stale view.

### Downloads

- **Linux x86_64:** `GitMycelium-1.1.1-x86_64.AppImage`.
- **macOS (Apple Silicon):** `GitMycelium-1.1.1-arm64.dmg`.

From 1.1.0 on, Help → Check for Updates installs this in place.

## [1.1.0] - 2026-10-13

### Added

- **In-app updates.** Help → Check for Updates finds a newer release and installs
  it in place: the AppImage file is replaced, and on macOS the app bundle is swapped
  once the app quits. The download is checked against the sha256 in the release's
  `update.json` before anything is replaced. The app checks once a day on launch;
  turn that off in Preferences → Appearance → Updates. `--update-check` prints the
  same comparison headlessly.
- Every release build carries the CI run number: `--version` and the About box show
  it (`1.1.0 (build 12)`), and on macOS it is the bundle's `CFBundleVersion`, so the
  system no longer confuses two builds of the same version (stale Finder icons, an
  older copy launched in place of the new one).

### Fixed

- `test_repo_watcher` no longer fails on macOS when FSEvents delivers the fixture's
  own writes late.

### Downloads

- **Linux x86_64:** `GitMycelium-1.1.0-x86_64.AppImage` (glibc 2.35 or newer).
- **macOS (Apple Silicon, macOS 12 or newer):** `GitMycelium-1.1.0-arm64.dmg`. Ad-hoc
  signed and not notarized: allow the first launch as described in
  [Opening the app on macOS](https://github.com/ndeana-21/GitMycelium#opening-the-app-on-macos).
  Later updates install from inside the app.

1.0.0 has no updater, so moving to 1.1.0 is one last manual download.

## [1.0.0] - 2026-10-13

The first public release: a fast, native Git client for large repositories, built
with C++20, Qt 6 and libgit2.

### Downloads

- **Linux x86_64:** `GitMycelium-1.0.0-x86_64.AppImage`. It runs on any distribution
  with glibc 2.35 or newer (Ubuntu 22.04+, Debian 12+, Fedora 36+, Arch). Make it
  executable and run it: `chmod +x GitMycelium-1.0.0-x86_64.AppImage && ./GitMycelium-1.0.0-x86_64.AppImage`.
  `git` 2.38 or newer must be installed.
- **macOS (Apple Silicon, macOS 12 or newer):** `GitMycelium-1.0.0-arm64.dmg`. The
  app is signed ad-hoc and not notarized, so macOS blocks the first launch: open
  it once, then click **Open Anyway** in **System Settings → Privacy & Security**
  (or run `xattr -dr com.apple.quarantine /Applications/GitMycelium.app`). See
  [Opening the app on macOS](https://github.com/ndeana-21/GitMycelium#opening-the-app-on-macos).
  `git` must be installed (for example with `xcode-select --install`).

Each file comes with a `.sha256` checksum.

### Highlights

- **Commit graph** over all refs in git's own topological order, paged as you
  scroll. One colour per branch, labels that group a local branch with its
  remote-tracking twin, and hover highlighting.
- **Drag and drop**: drop a branch on a branch to merge, rebase or check out, or on
  a commit to create a branch there, move the branch or check out a detached HEAD.
- **Merge conflict editor** with Ours, Base and Theirs panes and an editable Result.
  Choose per conflict or per line, see word-level changes, and let an automatic
  pass settle the trivial cases. Conflicts are predicted before you merge or rebase.
- **Interactive rebase editor**: reorder, squash, fixup, reword or drop commits,
  including the root commit.
- **Undo and redo** for operations that move refs.
- **Line-level staging**: stage, unstage or discard single lines or hunks. Commits
  can be signed (GPG or SSH) and carry co-author trailers.
- **Remotes**: clone (including shallow and partial clones), fetch, pull, push,
  force-with-lease, and optional auto-fetch.
- Worktrees, submodules, Git LFS, stash, tags, reflog, blame, file history,
  comparing two commits, hooks, sparse checkout and maintenance.
- Repositories in tabs, a command palette (`Ctrl+P`), a rebindable keymap, a
  monospace dark theme, a light theme and zoom.
- Headless `--probe` and `--shot` modes for scripting and debugging.

### License

Apache License 2.0. The binaries bundle Qt (LGPLv3) and libgit2 (GPLv2 with linking
exception); their license texts and source locations are included in the package.
See `THIRD_PARTY_NOTICES.md`.
