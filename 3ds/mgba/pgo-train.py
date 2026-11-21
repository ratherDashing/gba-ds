#!/usr/bin/env python3
"""pgo-train.py: remakes the libmgba profile in pgo/ from a training run in an Azahar instance.

    pgo-train.py --id N            (run from the repo; N = an Azahar instance of your own, 1-20)

1. libmgba with -fprofile-generate: MGBA_PGO=gen build-libmgba.sh DEPS (in the devkitARM image), build folder build-pgogen.
2. The app linked with it: make -C 3ds/ctr CORE=mgba BENCH=pgogen XLIBS=-lgcov BUILD=... MGBA_BUILD=DEPS/mgba-3ds/build-pgogen
3. This script: plays the stretches of pgo/training.txt (bench_jobs.py's probe syntax) in Azahar; the app writes its
   .gcda files to /gcov on the SD card when it ends; they replace pgo/*.gcda.
4. MGBA_PGO=use build-libmgba.sh then builds from them. Redo all of it after any change to libmgba's sources or patches
   (a function whose code changed loses its profile)."""
import glob
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(subprocess.check_output(
    ['git', '-C', REPO, 'rev-parse', '--git-common-dir'], text=True).strip())))
TOOLS = f'{ROOT}/project-notes/tools'
PGO = f'{REPO}/3ds/mgba/pgo'
TAG = 'pgogen'
HANG = 600  # seconds without a new result line before the run is called hung
DUMP_WAIT = 150  # seconds after the last job for the app to write its .gcda files


def main():
    args = sys.argv[1:]
    if len(args) != 2 or args[0] != '--id':
        sys.exit(__doc__)
    n = args[1]
    env = dict(os.environ, AZ_ID=n)
    subprocess.run([f'{TOOLS}/emu.sh', 'stop'], env=env, capture_output=True)
    sd = f'{ROOT}/project-data/azahar/inst/{n}/home/.local/share/azahar-emu/sdmc'
    bench = f'{sd}/3ds/gba-ds-bench-{TAG}'
    shutil.rmtree(bench, ignore_errors=True)
    os.makedirs(bench)
    shutil.rmtree(f'{sd}/gcov', ignore_errors=True)
    os.makedirs(f'{sd}/gcov')
    roms = f'{sd}/3ds/gba-ds-bench-roms'
    os.makedirs(roms, exist_ok=True)
    subprocess.run(['python3', '-I', f'{TOOLS}/bench_jobs.py', '--out', bench, '--probe', f'{PGO}/training.txt'], check=True)
    for line in open(f'{bench}/jobs.txt'):
        if line.startswith('job '):
            rom = f'{roms}/{line[4:].split("|")[1].strip()}'
            if not os.path.lexists(rom):
                os.symlink(f'{ROOT}/roms/{os.path.basename(rom)}', rom)
    app = f'{ROOT}/project-data/chamomile/{TAG}.3dsx'
    results = f'{bench}/results.txt'
    t0 = time.time()
    subprocess.run([f'{TOOLS}/emu.sh', 'start', app], env=env, check=True)
    last, changed = -1, time.time()
    while True:
        time.sleep(10)
        text = open(results, errors='replace').read() if os.path.exists(results) and os.path.getmtime(results) > t0 else ''
        if 'bench end' in text:
            break
        if len(text) != last:
            last, changed = len(text), time.time()
        elif time.time() - changed > HANG:
            subprocess.run([f'{TOOLS}/emu.sh', 'stop'], env=env)
            sys.exit('the training run hung')
    time.sleep(DUMP_WAIT)
    subprocess.run([f'{TOOLS}/emu.sh', 'stop'], env=env)
    files = glob.glob(f'{sd}/gcov/**/*.gcda', recursive=True)
    if not files:
        sys.exit('no .gcda files were written')
    for old in glob.glob(f'{PGO}/*.gcda'):
        os.remove(old)
    for f in files:
        shutil.copy(f, PGO)
    print(f'{len(files)} profile files in {PGO} after {time.time() - t0:.0f} s')


if __name__ == '__main__':
    main()
