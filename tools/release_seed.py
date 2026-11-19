"""Write the generated headers a release build reads, without the game: generated/<version>/{layout.json,layout.h,patches.h}.

    python tools/release_seed.py [--version v143] [--out generated/v143]

tools/make_release.py and tools/make_release_linux.py compile the runtime library against one game version's
generated folder, but read only three files of it: layout.json and layout.h (recomp/layout.py: the guest base and size
of each module) and patches.h (recomp/patches.py: the patch set names, switches and defaults of our own
recomp/patches-<version>.txt and the patch page after main). The module sizes are the only numbers taken from the
game's files; they are listed below, so a cloud runner without any game files makes the same three files that
recomp/gen.py writes on a machine with them. --check compares the files with an existing generated folder.

The generated C itself (the lifted code) is never needed: the player's setup lifts and compiles it.
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'recomp'))
sys.path.insert(0, str(ROOT / 'tools'))
import layout  # noqa: E402
import patches  # noqa: E402
from check_content import GENERATED_MARKER  # noqa: E402

# Module image sizes of each update's ExeFS (recomp/layout.py layout(): Nso(path).size), in load order.
SIZES = {
    'v143': [('main', 0x3adf000), ('subsdk0', 0x66c000), ('sdk', 0xc55000)],
}


def mods(version):
    out, base = [], layout.FIRST_BASE
    for name, size in SIZES[version]:
        out.append((name, base, size))
        base = layout.align_up(base + size)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--version', default='v143', choices=sorted(SIZES))
    ap.add_argument('--out', type=Path)
    ap.add_argument('--check', type=Path, help='compare with this generated folder instead of writing')
    a = ap.parse_args()
    m = mods(a.version)
    table = patches.load(ROOT / 'recomp' / f'patches-{a.version}.txt', m)
    tmp = (a.check and ROOT / 'build' / 'release-seed-check') or a.out or ROOT / 'generated' / a.version
    layout.write(m, tmp)
    (tmp / 'patches.h').write_bytes(patches.header_text(table, GENERATED_MARKER).encode())   # as gen.py write_if_changed
    if a.check:
        bad = [f for f in ('layout.json', 'layout.h', 'patches.h')
               if (tmp / f).read_bytes() != (a.check / f).read_bytes()]
        if bad:
            sys.exit(f'release_seed: differs from {a.check}: {", ".join(bad)}')
        print(f'release_seed: {a.check} matches')
    else:
        print(f'release_seed: wrote {tmp}')


if __name__ == '__main__':
    main()
