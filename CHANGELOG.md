# Changelog

All notable changes to GitMycelium are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

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
