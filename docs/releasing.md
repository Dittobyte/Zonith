# Releasing

A release is the Windows setup zip and the Linux AppImage, both built on GitHub's runners by
`.github/workflows/release.yml`. The release carries the setup and a toolchain, never game data: the player's machine
lifts and compiles the game (`check_content` runs over both folders before they are packed).

## Steps

1. Bump `VERSION` (and the README badge) in a pull request and merge it.
2. Actions → **Release** → *Run workflow* on `main`, with the notes (short Added / Changed / Fixed one-liners) and
   *publish* ticked. Without *publish* the run only builds, and keeps both files as the run's artifacts.

With *publish* the run stops at once when the tag `v<VERSION>` exists. Otherwise it builds both files, makes the
release as a draft on the commit it built, attaches them, checks their sizes on GitHub, and only then publishes it,
which creates the tag. A failed build or upload never leaves a published release.

## What the runners build from

- **Generated headers.** The runtime library compiles against `generated/<version>/layout.json`, `layout.h` and
  `patches.h`, never against lifted code. `tools/release_seed.py` writes all three without game files: the module
  sizes it lists (the only numbers taken from the game's ExeFS) and `recomp/patches-<version>.txt`. When a game update
  changes a module size, add it to `SIZES`; `release_seed.py --check generated/<version>` compares the seed with a
  folder that `recomp/gen.py` lifted.
- **Execution profile.** `tools/release_pgo.py` takes `data/pgo-*.profdata` from the latest published release's
  Windows zip. A retrained profile (docs/pgo.md) reaches the workflow by shipping once in a release made by hand:
  `tools/make_release.py` and `tools/make_release_linux.py --pgo` read `build/pgo`.
- **Everything else** comes from the pinned downloads of the release tools and `tools/fetch_refs.sh`.

## By hand

The same two tools build a release on a developer machine (`docs/installer.md`, "Release layout" and "Linux
release"); a lifted `generated/<version>` there also allows `--prove`, which the runners cannot run.
