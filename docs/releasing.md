# Releasing

A release is the Windows setup zip and the Linux AppImage of `main`, built on the machine that holds the game files.
GitHub's own runners cannot build it: `tools/make_release.py` and `tools/make_release_linux.py` compile the runtime
library against a lifted game version (`generated/v121` or `v143`), and nothing derived from the game leaves that
machine. The release itself carries no game data (`check_content` runs over both folders).

## Steps

1. Bump `VERSION` (and the README badge) in a pull request and merge it.
2. Run the **Release** workflow (Actions, *Run workflow*) with the notes, or run it by hand:

   ```
   .venv\Scripts\python tools\release.py --notes notes.md [--publish]
   ```

`tools/release.py` takes the remote's `main`, checks it out in `.worktrees/release-<version>` linked to the checkout's
ignored data, builds the translator, the Windows zip and, in WSL, the AppImage (`tools/release_wsl.sh`), makes the
GitHub release as a draft, attaches both files and checks their sizes on GitHub. Only then does `--publish` publish
it, which creates the tag `v<version>`. A run stops before building when that tag already exists, and a failed upload
leaves the draft unpublished. Release notes are short Added / Changed / Fixed one-liners.

## The self-hosted runner

The workflow runs on a runner labelled `zonith-release` on the machine with the game files. Set it up once:

1. GitHub: *Settings → Actions → Runners → New self-hosted runner*, Windows x64. Follow its download and
   `config.cmd` steps, adding `--labels zonith-release`. Start it with `run.cmd`, or install it as a service.
2. In the runner's folder, add `ZONITH_CHECKOUT=<path of this checkout>` to its `.env` file.
3. The checkout needs what a hand-made release needs: MSYS2 UCRT64 at `C:\msys64`, `.venv` and `.venv-py314`, one
   lifted game version in `generated/`, `refs/` (`tools/fetch_refs.sh`), the execution profiles in `build/pgo`, and
   WSL with Ubuntu 24.04 (`ZONITH_WSL_DISTRO` picks another). `release_wsl.sh` installs the .NET 10 SDK into WSL's
   `~/.dotnet` and a Python with Pillow on its first run.
