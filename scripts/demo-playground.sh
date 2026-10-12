#!/usr/bin/env bash
# Builds a throwaway repository set for trying gitmycelium by hand.
#
# Nothing here touches your real repositories: everything lives under
# ~/gitmycelium-demo and is deleted and rebuilt on every run.
#
#   ./scripts/demo-playground.sh
#
# Then open ~/gitmycelium-demo/work in gitmycelium.
#
# The set-up is deliberately shaped so that every button does something
# *successful*, which constrains the layout:
#
#   * a strictly-behind branch (main)  -> Fetch shows "down", Pull fast-forwards
#   * a strictly-ahead branch (feature/wip) -> "up", Push works
#   * two branches editing the same line -> a real merge conflict
#
# The two counters cannot live on one branch: being ahead *and* behind is a
# diverged branch, where git rejects both push and --ff-only pull. That is
# correct behaviour and useless as a first impression.
set -euo pipefail

ROOT="${1:-$HOME/gitmycelium-demo}"
REMOTE="$ROOT/remote.git"
WORK="$ROOT/work"
PEER="$ROOT/peer"
BASE=origin/main

echo ">> rebuilding playground in $ROOT"
rm -rf "$ROOT"
mkdir -p "$ROOT"

export GIT_AUTHOR_NAME="Demo Dev" GIT_AUTHOR_EMAIL="demo@example.com"
export GIT_COMMITTER_NAME="Demo Dev" GIT_COMMITTER_EMAIL="demo@example.com"
# Isolate from the user's real git config. Pointing GIT_CONFIG_GLOBAL at
# /dev/null fails: git tries to take a lock on it. Use a scratch file instead.
export GIT_CONFIG_GLOBAL="$ROOT/.gitconfig"
export GIT_CONFIG_SYSTEM=/dev/null
: > "$GIT_CONFIG_GLOBAL"
git config --global init.defaultBranch main
# Identity must live in the config, not only in the environment: the app reads
# the config when it creates a merge or rebase commit, and without it git
# refuses to commit at all.
git config --global user.name "Demo Dev"
git config --global user.email "demo@example.com"

# The "server". HEAD must point at the branch we push, otherwise clones end up
# with no checked-out branch and every later push fails.
git init -q --bare -b main "$REMOTE"

# --- the repository you will open ------------------------------------------
git init -q -b main "$WORK"
cd "$WORK"
git remote add origin "$REMOTE"

cat > app.conf <<'EOF'
listen = 8080
workers = 4
log_level = info
timeout = 30
EOF
cat > README.md <<'EOF'
# Demo project

A sandbox for exercising gitmycelium: branches, merges, conflicts and a remote.
EOF
git add -A && git commit -qm "Initial project skeleton"

printf 'line one\nline two\nline three\n' > config.txt
git add -A && git commit -qm "Add config.txt"

printf 'alpha\nbeta\ngamma\n' > data.txt
git add -A && git commit -qm "Add data.txt with the first dataset"

# This is the base every demo branch starts from.
git push -q --set-upstream origin main

# --- strictly ahead: Push has something to send -----------------------------
git checkout -q -b feature/wip
printf 'scratch = true\n' > scratch.txt && git add -A && git commit -qm "Start the scratch work"
git push -q --set-upstream origin feature/wip
printf 'scratch two\n' >> scratch.txt && git add -A && git commit -qm "Continue the scratch work"
# now feature/wip is 1 ahead of its upstream -> "up 1"

# --- clean merge and clean rebase demos ------------------------------------
git checkout -q -b feature/login "$BASE"
printf 'auth = true\n' > login.txt && git add -A && git commit -qm "Add login support"

git checkout -q -b feature/metrics "$BASE"
printf 'metrics = on\n' > metrics.txt && git add -A && git commit -qm "Add metrics collection"

# --- a real conflict: both branches edit the same line of app.conf ----------
git checkout -q -b feature/conflict-a "$BASE"
cat > app.conf <<'EOF'
listen = 8080
workers = 8
log_level = info
timeout = 30
EOF
git add -A && git commit -qm "conflict-a: raise the worker pool to 8"

git checkout -q -b feature/conflict-b "$BASE"
cat > app.conf <<'EOF'
listen = 8080
workers = 16
log_level = warn
timeout = 30
EOF
git add -A && git commit -qm "conflict-b: raise the worker pool to 16 and quieten the log"

git checkout -q main

# --- an integration branch with real merge commits --------------------------
# Without merges the graph is a set of straight lines, which makes it
# impossible to judge whether the branching structure is readable.
git checkout -q -b integration "$BASE"
git merge -q --no-ff feature/login -m "integration: merge login support"
git merge -q --no-ff feature/metrics -m "integration: merge metrics collection"
printf 'release = 1\n' > release.txt && git add -A && git commit -qm "integration: prepare the 1.0 release"
git checkout -q main

# --- a peer clone pushes to main, so we fall behind ------------------------
git clone -q -b main "$REMOTE" "$PEER"
cd "$PEER"
printf 'shared = yes\n' > shared.txt
git add -A && git commit -qm "Peer: add shared.txt"
git push -q origin main

cd "$WORK"

cat <<'EOF'

Playground ready.

  repository to open : ~/gitmycelium-demo/work
  its remote         : ~/gitmycelium-demo/remote.git   (a local bare repo, no network)
  peer clone         : ~/gitmycelium-demo/peer         (already pushed one commit)

Checklist — every step is expected to succeed:

  1. Fetch               status bar gains "down 1" (the peer's commit)
  2. Pull                shared.txt appears, "down" returns to 0
  3. Check out feature/wip (double-click)   status bar shows "up 1"
  4. Push                "up" returns to 0, git reports the push
  5. Drag feature/login onto main -> "Merge ... into main"   clean, no conflict
  6. Drag feature/metrics onto main -> "Rebase metrics onto main"   clean replay
  7. Drag feature/conflict-b onto feature/conflict-a -> "Merge..."   CONFLICT:
     the editor opens with Ours / Base / Theirs and an editable Result.
     Take theirs, save, then "Complete merge".
  8. Drag feature/login onto an older commit -> "Move ... to this commit"
  9. Double-click any branch to check it out

The "integration" branch has real merge commits, so the graph shows two lines
converging — that is the part worth judging visually.

Rebuild this playground at any time with ./scripts/demo-playground.sh
EOF
