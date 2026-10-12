# Changelog

All notable changes to GitMycelium are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

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
