"""Make a GitHub release of the remote main: the Windows zip and the Linux AppImage, built on this machine.

    python tools/release.py --notes NOTES.md [--remote private] [--publish] [--skip-linux]

The release cannot be built on GitHub's own runners: tools/make_release.py and tools/make_release_linux.py compile the
runtime library against a lifted game version (generated/v121 or v143), which never leaves the machine that holds the
player's own game files. So this runs on that machine, by hand or from .github/workflows/release.yml on a self-hosted
runner there.

Steps:
  1. Fetch <remote> main and take its commit; the version is VERSION at that commit. Stop when tag v<version> exists.
  2. Check that commit out in .worktrees/release-<version> (detached), linked to the main checkout's ignored data as
     tools/worktree.py does, so the build sees generated/, refs/, toolchain/ and the venvs without copying them.
  3. Build the shader translator (tools/build_xlate.py) and the Windows release (.venv-py314: the MSYS2 packages are
     .tar.zst, which needs Python 3.14's tarfile) with its zip.
  4. Build the AppImage in WSL (tools/release_wsl.sh), unless --skip-linux.
  5. Make the GitHub release as a DRAFT on that commit with the notes, attach the assets and check that GitHub lists
     every one of them at its local size. Only then, with --publish, publish it; that creates the tag. Without
     --publish the draft waits for a person.
  6. Remove the worktree.

Needs gh logged in (or GH_TOKEN), MSYS2 UCRT64 at C:\\msys64, WSL with Ubuntu for the AppImage, and build/pgo's
execution profiles when the release should carry them. Stdlib only.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import worktree  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MSYS = Path(r'C:\msys64\ucrt64\bin')
WSL_DISTRO = os.environ.get('ZONITH_WSL_DISTRO', 'Ubuntu-24.04')


def run(cmd, cwd=None, env=None, capture=False):
    print('+ ' + ' '.join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, text=True,
                       capture_output=capture)
    if r.returncode:
        sys.exit(f'failed ({r.returncode}): {" ".join(str(c) for c in cmd)}\n{(r.stderr or "") if capture else ""}')
    return r.stdout if capture else ''


def wsl_path(p):
    p = Path(p).resolve()
    return f'/mnt/{p.drive[0].lower()}{p.as_posix()[2:]}'


def gen_version(main):
    for v in ('v121', 'v143'):
        if (main / 'generated' / v / 'layout.json').is_file():
            return v
    sys.exit('neither generated/v121 nor generated/v143 exists: extract a game version and lift it first')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--notes', type=Path, required=True, help='release notes (Markdown)')
    ap.add_argument('--remote', default='private', help='git remote of the GitHub repository (default private)')
    ap.add_argument('--repo', help='owner/name on GitHub (default: from the remote URL)')
    ap.add_argument('--publish', action='store_true', help='publish the draft once every asset is attached')
    ap.add_argument('--checkout', default=os.environ.get('ZONITH_CHECKOUT'),
                    help="the checkout that holds the ignored data (generated/, refs/, toolchain/, the venvs); default "
                         "this script's own main checkout, or $ZONITH_CHECKOUT (set it on a self-hosted runner)")
    ap.add_argument('--skip-linux', action='store_true', help='leave out the AppImage')
    a = ap.parse_args()
    if not a.notes.is_file():
        sys.exit(f'{a.notes} not found')

    main_co = Path(a.checkout).resolve() if a.checkout else worktree.main_checkout(ROOT)
    url = worktree.git(main_co, 'remote', 'get-url', a.remote).stdout.strip()
    repo = a.repo or url.removesuffix('.git').split('github.com')[-1].strip('/:')
    worktree.git(main_co, 'fetch', '-q', a.remote, 'main')
    commit = worktree.git(main_co, 'rev-parse', f'{a.remote}/main').stdout.strip()
    version = worktree.git(main_co, 'show', f'{commit}:VERSION').stdout.strip()
    tag = f'v{version}'
    tags = run(['gh', 'api', f'repos/{repo}/git/matching-refs/tags/{tag}', '--jq', '.[].ref'], capture=True).split()
    if f'refs/tags/{tag}' in tags:
        sys.exit(f'{tag} already exists on {repo}: bump VERSION first')
    print(f'release {tag} of {repo} at {commit[:12]}', flush=True)

    wt = main_co / '.worktrees' / f'release-{version}'
    if wt.exists():
        worktree.unlink_data(wt)
        worktree.git(main_co, 'worktree', 'remove', '--force', str(wt))
    worktree.git(main_co, 'worktree', 'add', '--detach', str(wt), commit)
    worktree.link_data(main_co, wt)
    try:
        out = main_co / 'build' / 'release' / tag
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        env = dict(os.environ, PATH=f'{MSYS}{os.pathsep}{os.environ["PATH"]}')
        win_zip = out / f'TotK-Setup-{version}-windows.zip'
        run([wt / '.venv' / 'Scripts' / 'python.exe', 'tools/build_xlate.py'], cwd=wt, env=env)
        run([wt / '.venv-py314' / 'Scripts' / 'python.exe', 'tools/make_release.py',
             '--out', out / 'TotK-Setup-windows', '--zip', win_zip], cwd=wt, env=env)
        assets = [win_zip]
        if not a.skip_linux:
            run(['wsl.exe', '-d', WSL_DISTRO, '--', 'bash', wsl_path(wt / 'tools' / 'release_wsl.sh'),
                 wsl_path(main_co), commit, gen_version(main_co), wsl_path(out)])
            assets.append(out / 'TotK-Setup-x86_64.AppImage')
        for p in assets:
            if not p.is_file():
                sys.exit(f'{p} was not built')

        drafts = run(['gh', 'release', 'list', '-R', repo, '--json', 'tagName,isDraft', '--jq',
                      f'.[] | select(.isDraft and .tagName == "{tag}") | .tagName'], capture=True).split()
        if drafts:
            run(['gh', 'release', 'edit', tag, '-R', repo, '--notes-file', a.notes.resolve()])
            run(['gh', 'release', 'upload', tag, '-R', repo, '--clobber', *assets])
        else:
            run(['gh', 'release', 'create', tag, '-R', repo, '--draft', '--target', commit,
                 '--title', f'Zonith {version}', '--notes-file', a.notes.resolve(), *assets])
        listed = run(['gh', 'release', 'view', tag, '-R', repo, '--json', 'assets', '--jq',
                      '.assets[] | "\\(.name) \\(.size)"'], capture=True).splitlines()
        sizes = dict(line.rsplit(' ', 1) for line in listed)
        for p in assets:
            if sizes.get(p.name) != str(p.stat().st_size):
                sys.exit(f'{p.name}: GitHub lists {sizes.get(p.name)} bytes, local {p.stat().st_size}: left as draft')
        if a.publish:
            run(['gh', 'release', 'edit', tag, '-R', repo, '--draft=false'])
            print(f'published {tag}')
        else:
            print(f'draft {tag} is ready: publish it on GitHub, or run again with --publish')
    finally:
        worktree.unlink_data(wt)
        worktree.git(main_co, 'worktree', 'remove', '--force', str(wt))


if __name__ == '__main__':
    main()
