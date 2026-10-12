# Security policy

## Supported versions

Security fixes are made for the latest released version.

## Reporting a vulnerability

Please do **not** open a public issue for a security problem. Use GitHub's
[private vulnerability reporting](../../security/advisories/new) for this repository
instead. Include steps to reproduce and the versions involved. You should receive
an acknowledgement within a few days.

GitMycelium runs the `git` binary and the hooks, filters, credential helpers and
external tools that your git configuration names. A repository that you open can
therefore run code through the hooks and filters it sets up. That is how git
works, not a vulnerability. Only open repositories you trust.
