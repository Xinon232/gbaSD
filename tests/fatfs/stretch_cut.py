#!/usr/bin/env python3
"""Power cut while a folder grows by a cluster on an exFAT card (from
gbaplanner's tests/exfat, also in gbavocab and gbareader; here against gbaSD's
FatFs, and also on a contiguous card).

A card made with mkfs.exfat (512-byte clusters); fragmented: every other
cluster taken first, so the folder's clusters are FAT-chained; contiguous:
the folder stays in one piece (NoFatChain). gbaSD's FatFs (stretch_cut.c)
adds files to the folder until it grows by one cluster, from 1, 2 and 3
clusters; the power is cut after each sector write of that, in turn. After
each cut ("reboot"): fsck.exfat -n is clean (only the folder may hold one
more cluster than its size), the folder reads in FatFs and lists the same as
in tests/exfat/exfat_read.py (no FatFs code), files added after it (the
folder grows by two more clusters) read back, and the card is clean again.

Without the two exFAT changes in FatFs (create_chain: end the chain at a new
fragment before linking to it; dir_next: end an exFAT sub-directory at its
data length) a cut here left the folder unreadable or new entries invisible
on a PC.

  python3 tests/fatfs/stretch_cut.py [--config=off|on] [--layout=fragmented|contiguous] [FOLDER]
                                                  (run by tests/fatfs/run.py, both layouts)

Needs gcc and exfatprogs (mkfs.exfat, fsck.exfat)."""
from pathlib import Path
import os, re, shutil, subprocess, sys, tempfile

HERE = Path(__file__).resolve().parent
R = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(R / 'tests' / 'exfat_kit'))
import ffbuild  # noqa: E402
from exfat_read import Volume  # noqa: E402

ARGS = [a for a in sys.argv[1:] if not a.startswith('--')]
CFG = next((a.split('=', 1)[1] for a in sys.argv[1:] if a.startswith('--config=')), 'off')
LAYOUTS = [a.split('=', 1)[1] for a in sys.argv[1:] if a.startswith('--layout=')] or ['fragmented', 'contiguous']
FOLDER = ARGS[0] if ARGS else 'appdata'
SIZE = 4 << 20


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


def reader_listing(img):
    return sorted(f'{"D" if d else "F"} {0 if d else size} {name}' for name, d, size, first, cont in
                  Volume(str(img)).listdir('/' + FOLDER))


def fsck(img, allow_folder_trace):
    """[] when fsck.exfat -n finds the card clean (or only the folder's extra cluster)."""
    p = run(['fsck.exfat', '-n', img])
    if p.returncode == 0:
        return []
    bad, errors = [], 0
    for line in (p.stdout + p.stderr).splitlines():
        if 'ERROR' not in line:
            continue
        errors += 1
        if allow_folder_trace and f'/{FOLDER}: more clusters are allocated' in line:
            continue
        bad.append(line.strip())
    if errors and not bad:
        return []
    return bad or [f'fsck.exfat exit {p.returncode}: ' + (p.stdout + p.stderr).strip()[-300:]]


def main():
    work = Path(tempfile.mkdtemp(prefix='stretch-cut-'))
    try:
        tool = ffbuild.build(work / 'stretch_cut', [HERE / 'stretch_cut.c'], ffbuild.make_conf(work / 'conf', CFG))
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
        blank = work / 'blank.img'
        with blank.open('wb') as f:
            f.truncate(SIZE)
        p = run(['mkfs.exfat', '-c', '512', blank])
        if p.returncode:
            sys.exit(p.stdout + p.stderr)
        failures, cuts = [], 0
        for layout, start in [(lay, st) for lay in LAYOUTS for st in (1, 2, 3)]:
            base = work / f'base{start}.img'
            p = run([tool, 'prep', blank, base, FOLDER, start, layout], env=env)
            if p.returncode or fsck(base, False):
                sys.exit(f'prep {start}: {p.stdout}{p.stderr}{fsck(base, False)}')
            if Volume(str(base)).find('/' + FOLDER)[4] != (layout == 'contiguous' or start == 1):
                sys.exit(f'prep {start}: the folder is not {layout}')
            p = run([tool, 'cut', base, work / 'full.img', FOLDER, -1], env=env)
            m = re.search(r'WRITES (\d+)', p.stdout)
            if p.returncode or not m:
                sys.exit(f'clean stretch from {start}: {p.stdout}{p.stderr}')
            n = int(m.group(1))
            for k in range(n + 1):
                what = f'{layout}, folder {start}->{start + 1} clusters, cut after {k}/{n} writes'
                cut, after = work / 'cut.img', work / 'after.img'
                p = run([tool, 'cut', base, cut, FOLDER, k], env=env)
                if p.returncode:
                    failures.append(f'{what}: {p.stdout}{p.stderr}')
                    continue
                cuts += 1
                bad = fsck(cut, True)
                if bad:
                    failures.append(f'{what}: fsck.exfat after the cut: {bad}')
                p = run([tool, 'after', cut, after, FOLDER], env=env)
                ours = sorted(x[5:] for x in p.stdout.splitlines() if x.startswith('LIST '))
                if p.returncode:
                    failures.append(f'{what}: after reboot: {p.stdout.strip().splitlines()[-1:]}{p.stderr[-300:]}')
                    continue
                if ours != reader_listing(cut):
                    failures.append(f'{what}: folder lists differently in FatFs and exfat_read: '
                                    f'{sorted(set(ours) ^ set(reader_listing(cut)))}')
                ours2 = sorted(x[6:] for x in p.stdout.splitlines() if x.startswith('LIST2 '))
                if ours2 != reader_listing(after):
                    failures.append(f'{what}: after growing again, FatFs and exfat_read differ: '
                                    f'{sorted(set(ours2) ^ set(reader_listing(after)))}')
                bad = fsck(after, False)
                if bad:
                    failures.append(f'{what}: fsck.exfat after growing again: {bad}')
        for f in failures[:20]:
            print('FAIL', f)
        print(f'exFAT folder-stretch power cut (/{FOLDER}, {" + ".join(LAYOUTS)}, 512-byte clusters, '
              f'{CFG} switches): {cuts} cuts, {len(failures)} failures')
        sys.exit(1 if failures else 0)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
