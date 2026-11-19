"""Put the execution profiles of the latest published release into build/pgo, for the release workflow.

    python tools/release_pgo.py [--repo owner/name]

They are data/pgo-<version>.profdata and .json of that release's Windows zip; a local release uses the ones tools/pgo.py
merged instead. Without a published release the build goes on without a profile, as tools/make_release.py allows.
Needs gh (GH_TOKEN). Stdlib only.
"""
import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY'))
    a = ap.parse_args()
    if not a.repo:
        sys.exit('release_pgo: pass --repo or set GITHUB_REPOSITORY')
    dl, out = ROOT / 'build' / 'pgo-dl', ROOT / 'build' / 'pgo'
    shutil.rmtree(dl, ignore_errors=True)
    dl.mkdir(parents=True)
    out.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(['gh', 'release', 'download', '-R', a.repo, '--pattern', '*-windows.zip', '--dir', str(dl)])
    zips = sorted(dl.glob('*-windows.zip'))
    if r.returncode or not zips:
        print('release_pgo: no published release to take the profiles from: building without one')
        return
    with zipfile.ZipFile(zips[0]) as z:
        for n in z.namelist():
            p = PurePosixPath(n)
            if p.parent.name == 'data' and p.name.startswith('pgo-') and p.suffix in ('.profdata', '.json'):
                (out / p.name).write_bytes(z.read(n))
                print(f'release_pgo: {p.name} from {zips[0].name}')
    shutil.rmtree(dl)


if __name__ == '__main__':
    main()
