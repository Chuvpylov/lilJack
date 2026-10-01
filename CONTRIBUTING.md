# Contributing to lilJack

Thank you. Releases are cut from `main` with `tools/sync_public.sh`, which
refuses commits that carry private content, so the rules below keep that path clean.

## Before you open a pull request

1. Build and run both suites:
   ```bash
   bash tests/run_liljack_native_tests.sh
   bash tests/run_liljack_python_tests.sh
   ```
2. Add or extend a test for behaviour you change. A fix that has no failing test
   before and passing test after is hard to review.
3. Keep paths generic. No absolute paths from your machine, no hostnames, no
   LAN addresses, no tokens. `tools/scan_private.sh` runs the same checks the
   maintainers run before a sync; run it on your commits.
4. Visual work is sixel-first. Include a capture from a sixel terminal; the
   ANSI cell path is a fallback, not the target.
5. One topic per pull request, with a short description of what was checked and
   what was not verified.

## Layout that must stay stable

`liljack_app/`, `tests/`, `toolbox/liljack_*.py` and `plugins/liljack/` keep the
stable paths; that is what lets `git cherry-pick` move commits between branches. `vendor/hui` exists only here; changes to HUI itself
go to the HUI project.

## Sync for maintainers

`tools/sync_public.sh <range>` cherry-picks a commit range onto the release
branch and refuses any commit that trips `tools/scan_private.sh`.

## Reporting a security issue

Email is not monitored; open a private security advisory on GitHub instead.

## License of contributions

By opening a pull request you offer your change under the MIT license, the
same license as the repository, so it can be merged and released without
further paperwork.
