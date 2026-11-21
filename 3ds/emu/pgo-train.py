#!/usr/bin/env python3
"""pgo-train.py: remakes armGBA's profile in pgo/ from a training run in an Azahar instance.

    pgo-train.py --id N            (run from the repo; N = an Azahar instance of your own, 1-20)

1. The app with armGBA counting what it does, from 3ds/ctr in the devkitARM image:
   make CORE=armgba BENCH=pgogen PGO=gen BUILD=build/gen MGBA_BUILD=...   (BUILD relative: GCC names the files by it)
2. This script: plays the stretches of pgo/training.txt (bench_jobs.py's probe syntax) in Azahar; the app writes its
   .gcda files to /gcov on the SD card when the bench ends; they replace pgo/*.gcda.
3. make PGO=use then builds armGBA's files from them. Redo all of it after armGBA's code changes (a function whose
   code or line number changed loses its profile, so anything above it in its file counts; make PGO_QUIET= shows them)."""
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
PGO = f'{REPO}/3ds/emu/pgo'
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
    app = f'{REPO}/3ds/ctr/gba-ds-bench-{TAG}.3dsx'
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
