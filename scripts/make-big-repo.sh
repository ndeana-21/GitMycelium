#!/usr/bin/env bash
# Builds a synthetic repository for the benchmarks: one branch with many commits,
# and branches that fork off it so the graph has more than one line.
#
# Usage: make-big-repo.sh <dir> [commits] [branches] [files]
#
# Nothing here is a test. It exists so the numbers in docs/05 can be reproduced
# without a real million-commit repository to hand. git fast-import writes the
# history; a shell loop committing one at a time would take hours where this takes
# seconds.
set -euo pipefail

dir="${1:?usage: make-big-repo.sh <dir> [commits] [branches] [files]}"
commits="${2:-100000}"
branches="${3:-1000}"
files="${4:-200}"

export GIT_AUTHOR_NAME=T GIT_AUTHOR_EMAIL=t@t
export GIT_COMMITTER_NAME=T GIT_COMMITTER_EMAIL=t@t

rm -rf "$dir"
git init -q -b main "$dir"

awk -v n="$commits" -v branches="$branches" -v files="$files" '
BEGIN {
    ts = 1600000000
    base = 1000000
    for (i = 1; i <= n; i++) {
        path = "dir" (i % 100) "/file" (i % files) ".txt"
        content = "line " i "\n"
        printf "blob\nmark :%d\ndata %d\n%s\n", i, length(content), content
        printf "commit refs/heads/main\nmark :%d\n", base + i
        printf "author T <t@t> %d +0000\ncommitter T <t@t> %d +0000\n", ts + i, ts + i
        msg = "commit " i
        printf "data %d\n%s\n", length(msg), msg
        printf "M 100644 :%d %s\n\n", i, path
    }
    step = int(n / (branches > 0 ? branches : 1)) + 1
    for (b = 0; b < branches; b++) {
        fork = (b + 1) * step
        if (fork > n) {
            break
        }
        printf "reset refs/heads/b%d\nfrom :%d\n\n", b, base + fork
        printf "commit refs/heads/b%d\n", b
        printf "author T <t@t> %d +0000\ncommitter T <t@t> %d +0000\n", ts, ts
        msg = "branch " b
        printf "data %d\n%s\n\n", length(msg), msg
    }
}' | git -C "$dir" fast-import --quiet

git -C "$dir" reset --hard -q main

echo "made $dir: $(git -C "$dir" rev-list --count main) commits on main, $(git -C "$dir" branch | wc -l) branches"
