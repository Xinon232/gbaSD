#!/usr/bin/env python3
"""gbaSD's FatFs tests (fatfs/), in both switch configurations (ffbuild.py):
off (the four Butano apps' settings, every GBASD_FF_* switch off) and on
(gbamp3's settings, GBASD_FF_FAST_EXFAT_LOOKUP / FSIZE32 / OPEN_ENTRY on):

  1. exfat_repair/run.sh   exfat_repair_dir: 360 cases of entry sets torn by a
                           power cut, every cut point (fsck.exfat checked)
  2. stretch_cut.py        exFAT folder growth, power cut at every sector write,
                           fragmented and contiguous folder
  3. growth_cut.py         folder growth power cut on FAT12, FAT16, FAT32 and
                           exFAT, 512-byte and larger clusters (fsck, mtools /
                           exfat_read listings)
  4. remove_cut.py         delete / rename (same folder, across folders) of
                           files with long names over a sector boundary,
                           power cut at every sector write, FAT12/16/32 and
                           exFAT (no 8.3 alias left; fsck, mtools / exfat_read)
  5. ../exfat_kit/run.sh   the exFAT card-tool kit against the real mtools
Then, once: growth_cut.py --unfixed must fail on FAT (R0.15's folder growth,
GBASD_FF_FAT_STRETCH_FIX 0), remove_cut.py --unfixed must fail on FAT
(R0.15's delete order, GBASD_FF_FAT_REMOVE_ORDER_FIX 0), and equivalence.py
compares fatfs/ff.c with the apps' own FatFs copies (local check: SKIP
without the app checkouts).

  python3 tests/fatfs/run.py [--quick]     (--quick: 512-byte cards only in
                                            growth_cut and remove_cut, no kit
                                            self-test)
Linux tools: gcc, g++, exfatprogs, dosfstools, mtools; a part whose tools
are missing prints SKIP and is not run."""
from pathlib import Path
import os, platform, shutil, subprocess, sys, tempfile, time

HERE = Path(__file__).resolve().parent
R = HERE.parents[1]
sys.path.insert(0, str(HERE))
import ffbuild  # noqa: E402

QUICK = '--quick' in sys.argv[1:]
os.environ['PATH'] += os.pathsep + '/usr/sbin' + os.pathsep + '/sbin'


def step(label, cmd, env=None):
    t = time.time()
    print(f'== {label}', flush=True)
    p = subprocess.run([str(c) for c in cmd], cwd=R, env=env)
    print(f'   ({time.time() - t:.0f} s)', flush=True)
    if p.returncode:
        sys.exit(f'FAIL: {label}')


def main():
    if platform.system() != 'Linux' or not shutil.which('gcc'):
        print('SKIP: gbaSD FatFs tests need Linux and gcc')
        return
    missing = [t for t in ('mkfs.exfat', 'fsck.exfat') if not shutil.which(t)]
    if missing:
        print(f'SKIP: exFAT parts ({", ".join(missing)} missing: apt install exfatprogs)')
    py = [sys.executable, '-B']
    with tempfile.TemporaryDirectory() as t:
        for cfg in ffbuild.CONFIGS:
            conf = ffbuild.make_conf(Path(t) / cfg, cfg, mkfs=False) / 'ffconf.h'
            env = dict(os.environ, FFCONF=str(conf))
            step(f'[{cfg}] exfat_repair', ['sh', HERE / 'exfat_repair' / 'run.sh'], env)
            if not missing:
                step(f'[{cfg}] exFAT folder-stretch power cut', py + [HERE / 'stretch_cut.py', f'--config={cfg}'])
            step(f'[{cfg}] folder-growth power cut', py + [HERE / 'growth_cut.py', f'--config={cfg}']
                 + (['--quick'] if QUICK else []))
            step(f'[{cfg}] delete / rename power cut', py + [HERE / 'remove_cut.py', f'--config={cfg}']
                 + (['--quick'] if QUICK else []))
            if not QUICK:
                step(f'[{cfg}] exFAT kit self-test', ['sh', R / 'tests' / 'exfat_kit' / 'run.sh'], env)
    step('folder-growth power cut with R0.15\'s FAT order (must fail on FAT)',
         py + [HERE / 'growth_cut.py', '--unfixed', '--quick'])
    step('delete / rename power cut with R0.15\'s FAT delete order (must fail on FAT)',
         py + [HERE / 'remove_cut.py', '--unfixed', '--quick'])
    step('FatFs equivalence with the apps\' copies', py + [HERE / 'equivalence.py'])
    print('gbaSD FatFs tests PASS')


if __name__ == '__main__':
    main()
