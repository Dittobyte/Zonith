"""Build and run tests/nanvec_check.c: the vector NaN fix of runtime/simd_fp.h vslow against its lane-by-lane path.

Usage: nanvec_check.py
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
from common import MSYS_BIN, compiler  # noqa: E402


def main():
    work = ROOT / 'build' / 'test' / 'nanvec'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('nanvec_check.exe' if os.name == 'nt' else 'nanvec_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    for opt in ('-O0', '-O1'):
        subprocess.check_call([compiler(), opt, '-fno-strict-aliasing', '-march=x86-64-v3', '-I', str(ROOT / 'runtime'),
                               str(ROOT / 'tests' / 'nanvec_check.c'), '-o', str(exe)], env=env)
        print(opt, end=': ', flush=True)
        if subprocess.run([str(exe)], env=env).returncode:
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
