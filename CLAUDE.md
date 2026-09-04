# Project notes for Claude

This is a fork of the Metroid: Zero Mission decompilation (`metroidret/mzm`) with
a 3DS port under `platform/3ds/` and tooling under `tools/`.

## Language: English in code

All **comments, function names, and variable names** must be written in English.
This applies to everything we own or add:

- `platform/3ds/` (the port)
- `tools/` (build scripts, the layer workbench, etc.)
- `docs/` and any new files

**Exception:** the upstream decompilation itself — `src/`, `include/`, `asm/`,
`data/`, `sound/`, and the other files that come from `metroidret/mzm`. Leave
those as they are; do not retranslate or churn them.

No project-wide sweep is wanted. Parts of `tools/layer-workbench/` (`build_maps.py`,
`mzmdata.py`, `serve.py`, `README.md`, `index.html`) are still in Spanish from an
earlier convention. Convert a file's comments/identifiers to English **as you
touch it** for other reasons, not as a standalone task. User-facing UI strings and
prose docs can stay as they are until their file is otherwise being reworked.

## Git remotes: `origin` is the one that matters

`origin` is `tinaut1986/mzm` (this project). `upstream` is `metroidret/mzm`, the
decompilation this repo forked from — it is a **different project**, and nothing
we do here is ever aimed at it. Never open a PR, push, or target an issue
against `upstream` unless explicitly told to.

Because GitHub knows this repo is a fork, `gh` resolves a bare command to the
fork *parent* by default. `remote.origin.gh-resolved=base` is set in
`.git/config` to override that, so plain `gh run list` / `gh pr create` /
`gh release view` act on `tinaut1986/mzm`. That setting is local config, not
checked in — if a fresh clone starts resolving to `metroidret/mzm` again, run:

```sh
gh repo set-default tinaut1986/mzm
```

## Release process (3DS port)

Version numbers are meaningful, not sequential: a **minor** bump marks a
milestone (v0.4.0 followed v0.3.4 because the port became playable start to
finish), a **patch** is an ordinary fix round. Ask before choosing a minor bump;
never infer one. The next release branch is `release/vX.Y.(Z+1)`.

`main` means **stable**. A `release/*` branch is where work accumulates, and it
stays off `main` for as long as there are known things left to resolve — being
on `release/*` is itself the statement "not stable yet".

### Publishing

Two paths. Which one applies depends on whether the branch is ready to be called
stable, so **ask** rather than assuming.

**Beta** — tag the `release/*` branch without merging. Publishes a pre-release;
work continues on the same branch.

```sh
git tag -a v0.4.4 -m "v0.4.4"
git push origin release/v0.4.4
git push origin v0.4.4          # -> GitHub pre-release, "Beta v0.4.4"
```

**Stable** — merge into `main` first, then tag the merge commit.

```sh
git checkout main
git merge --no-ff release/v0.4.4
git tag -a v0.4.4 -m "v0.4.4"
git push origin main            # main FIRST -- see below
git push origin v0.4.4          # -> GitHub release, "Release v0.4.4"
```

A tag that already shipped as a beta is promoted in place: once its commit is
reachable from `main`, re-run **Build 3DS Release** from the Actions tab and
pick **the tag itself** as the ref, not `main`. The workflow derives the release
name from `git describe` on a manual run, so dispatching on `main` resolves to
`vX.Y.Z-1-g<hash>` (the merge commit is one commit past the tag) and publishes a
second, wrongly-named page instead of promoting the existing one. Dispatching on
the tag gives the bare `vX.Y.Z`, and `main` now reaches it, so the same page
flips to stable.

### How the CI picks the channel

`.github/workflows/build-release.yml` runs on any `v*` tag push and on manual
dispatch. It marks the GitHub Release as a pre-release when **`main` cannot
reach the built commit** — not by whether a tag was pushed:

| Built from | Channel |
|---|---|
| Tag on an unmerged `release/*` branch | `Beta` (pre-release) |
| Tag on `main`, or on a commit merged into `main` | `Release` |
| Manual dispatch on any branch | `Beta` (pre-release) |

**Push `main` before the tag.** The tag build resolves the channel against
`origin/main`, so a tag that arrives first cannot see the merge and publishes as
a beta. Also note Actions uses the workflow file **at the tagged commit**, so a
change to the workflow only takes effect for tags cut after it landed.

### After tagging

Rename the release branch to the next patch version and delete the old remote
branch — the tag identifies that line from then on:

```sh
git branch -m release/v0.4.4 release/v0.4.5
git branch --unset-upstream                  # the rename keeps the old tracking ref
git push -u origin release/v0.4.5
git push origin --delete release/v0.4.4
```

The version baked into the build (`make print-version`, and the FTP upload
filename) is derived from git: an exact tag on `HEAD` gives `vX.Y.Z`, anything
else on a release branch gives `vX.Y.Z-dev.<commits>+<hash>`.
