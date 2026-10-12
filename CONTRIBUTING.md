# Contributing to GitMycelium

Thank you for your interest in improving GitMycelium.

## Reporting bugs

Open an issue with:

- your OS and versions of Qt, libgit2 and git (`git --version`);
- what you did, what you expected, and what happened;
- if possible, the output of `gitmycelium --probe /path/to/repo` on a repository that
  reproduces the problem (remove anything private from it first).

Please report security problems privately, as described in [SECURITY.md](SECURITY.md),
not in a public issue.

## Development setup

See the build instructions in [README.md](README.md). A Debug build is the default
for development:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Read [AGENTS.md](AGENTS.md) before changing anything substantial. It describes the
module boundaries, the threading contract and many pitfalls we have already run into.

## Pull requests

- Keep the build free of warnings (`-Wall -Wextra`) and `ctest` passing.
- Any behaviour change in the git layer needs a test in `tests/` that runs against
  a real temporary repository created with the `git` binary. Do not mock libgit2.
- For a UI change, check it with an offscreen snapshot
  (`QT_QPA_PLATFORM=offscreen ./build/src/gitmycelium --shot ui.png <repo>`).
- `src/core` and `src/git` must not include Qt headers, and the GUI thread must
  never run a git operation.
- Use [Conventional Commits](https://www.conventionalcommits.org/) for commit
  messages (`feat: …`, `fix: …`), and explain *why* in the body.
- Do not add dependencies under licenses that are incompatible with Apache-2.0
  (for example GPL-only libraries linked into the application).

## License of contributions

GitMycelium is licensed under the Apache License, Version 2.0. As stated in its
section 5, any contribution you intentionally submit for inclusion is licensed under
the same terms, without any additional terms or conditions. By submitting a pull
request you confirm that you have the right to license your contribution this way.
