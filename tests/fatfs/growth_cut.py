#!/usr/bin/env python3
"""Power cut while a folder grows, on FAT12, FAT16, FAT32 and exFAT.

For each card (mkfs.fat / mkfs.exfat, 512-byte and larger clusters) the
free space is first filled with text and freed (old file data, as on a used
card), a folder is filled to the end of its cluster(s), and one more file
with a long name is created, so FatFs grows the folder by a cluster
(dir_alloc -> dir_next with stretch). The power is cut after each sector
write of that, in turn (growth_cut.c). After each cut ("reboot", as the apps
start: exfat_repair_dir on the folder first):

  - FAT: the card as the cut left it must already pass the checks below
    (exfat_repair_dir does nothing on FAT, checked). exFAT: when the new
    entry block spans two sectors, a cut between them leaves it half written
    (fsck.exfat: bad checksum), which exfat_repair_dir completes or removes;
    the checks then run on the repaired card (the count is printed);
  - fsck.fat -n / fsck.exfat -n: clean, or only the leftovers listed in
    ALLOWED (lost clusters: taken, linked from nowhere; never a damaged
    folder or file);
  - an independent listing of the folder (mtools mdir -b for FAT,
    tests/exfat_kit/exfat_read.py for exFAT, no FatFs code) is exactly the
    old entries, or the old entries plus the new file;
  - FatFs lists the same, the new file (if listed) is empty or exact, and
    FatFs grows the folder by two more clusters; those files read back after
    a remount and the listings still agree; fsck as above.

Two layouts per card: the new entry block starts the new cluster ("full"),
or spans the end of the old one and the new one ("partial").

R0.15's FAT code linked the new cluster before clearing it: a cut in between
showed the old text as garbage entries (mdir lists names that were never
written, fsck.fat finds bad names). With GBASD_FF_FAT_STRETCH_FIX 0 this test
must fail (--unfixed checks that it does).

  python3 tests/fatfs/growth_cut.py [--config=off|on] [--unfixed] [--quick]

Needs gcc, dosfstools + mtools for FAT, exfatprogs for exFAT (each part
skipped when its tools are missing)."""
from pathlib import Path
import os, re, shutil, subprocess, sys, tempfile

HERE = Path(__file__).resolve().parent
R = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(R / 'tests' / 'exfat_kit'))
import ffbuild  # noqa: E402
from exfat_read import Volume  # noqa: E402

ARGS = sys.argv[1:]
CFG = next((a.split('=', 1)[1] for a in ARGS if a.startswith('--config=')), 'off')
UNFIXED = '--unfixed' in ARGS
QUICK = '--quick' in ARGS
FOLDER = 'appdata'
NEW = 'the new entry that makes the folder grow.txt'
os.environ['PATH'] += os.pathsep + '/usr/sbin' + os.pathsep + '/sbin'
os.environ['MTOOLS_SKIP_CHECK'] = '1'

# (label, kind, image MB, mkfs options)
CARDS = [('FAT12 512 B clusters', 'fat', 1, ['-F', '12', '-s', '1']),
         ('FAT12 4 KB clusters', 'fat', 8, ['-F', '12', '-s', '8']),
         ('FAT16 512 B clusters', 'fat', 8, ['-F', '16', '-s', '1']),
         ('FAT16 2 KB clusters', 'fat', 16, ['-F', '16', '-s', '4']),
         ('FAT32 512 B clusters', 'fat', 36, ['-F', '32', '-s', '1']),
         ('FAT32 1 KB clusters', 'fat', 70, ['-F', '32', '-s', '2']),
         ('exFAT 512 B clusters', 'exfat', 8, ['-c', '512']),
         ('exFAT 4 KB clusters', 'exfat', 16, ['-c', '4096'])]
LAYOUTS = [(1, 'full'), (2, 'partial')]
if QUICK:
    CARDS = [c for c in CARDS if '512 B' in c[0]]

# fsck reports a cut may leave, all harmless (nothing visible, nothing
# damaged; a PC's fsck frees them):
ALLOWED = [
    # FAT: a cluster taken (marked end-of-chain) but linked from nowhere: the
    # new folder cluster when the cut came before the link reached the card,
    # or the new file's data cluster when it came before the file's entry did.
    re.compile(r'^Reclaimed \d+ unused clusters? \(\d+ bytes\)'),
    re.compile(r'^Free cluster summary wrong'),          # FAT32 FSINFO free count (a hint, recomputed)
    # FAT: the cut came between the write of a FAT sector and of its copy in
    # the second FAT. Both are intact; the first is the newer one, and FatFs,
    # fsck.fat and PCs use the first.
    re.compile(r'^FATs differ but appear to be intact\.'),
    re.compile(r'^Using first FAT\.'),
    # FAT: an entry block spanning two sectors (here: the old cluster's end
    # and the new cluster) whose long-name entries reached the card but not
    # its short-name entry: no file, the entries are skipped by every reader
    # (R0.15 writes any block over a sector boundary this way, growth or not).
    re.compile(r'^Orphaned long file name part '),
    re.compile(r'^A new long file name starts within an old one\.'),   # the same leftover, a new block after it
    re.compile(r'^Leaving filesystem unchanged\.'),
    # exFAT: the folder's FAT chain / bitmap holds one more cluster than its
    # data length (the cut came after the cluster was taken and cleared,
    # before the new length was written), or a cluster marked in the bitmap
    # that nothing uses.
    re.compile(rf'^ERROR: /{FOLDER}: more clusters are allocated'),
    re.compile(r'cluster is marked as allocated, but not used|bitmap'),
]


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


def fsck(kind, img):
    """Reports fsck makes that are not in ALLOWED ([] = clean or harmless)."""
    p = run(['fsck.fat', '-n', img] if kind == 'fat' else ['fsck.exfat', '-n', img])
    if p.returncode == 0:
        return []
    out = (p.stdout + p.stderr).splitlines()
    lines = [l.strip() for l in out if l.strip()]
    if kind == 'fat':
        # skip the banner and the summary line; continuation lines of an allowed report too
        body = [l for l in lines[1:] if not re.match(r'^\S+: \d+ files, \d+/\d+ clusters', l)]
        bad, keep = [], False
        for l in body:
            if any(a.search(l) for a in ALLOWED):
                keep = True
                continue
            if keep and (l.startswith(('Auto', 'Not auto-correcting', '1)', '2)')) or 'vs. really' in l):
                continue
            keep = False
            bad.append(l)
        return bad
    bad = [l for l in lines if 'ERROR' in l and not any(a.search(l) for a in ALLOWED)]
    return bad if bad or any('ERROR' in l for l in lines) else [f'fsck.exfat exit {p.returncode}: {lines[-3:]}']


def listing(kind, img):
    """The folder's names, read without FatFs; None when the folder cannot be read."""
    if kind == 'fat':
        p = run(['mdir', '-b', '-i', img, f'::/{FOLDER}'])
        if p.returncode:
            return None
        return sorted(l.split('/', 2)[2] if l.startswith('::/') else l for l in p.stdout.splitlines() if l.strip())
    try:
        return sorted(name for name, d, size, first, cont in Volume(str(img)).listdir('/' + FOLDER))
    except Exception as e:  # noqa: BLE001 - a folder the reader cannot walk is a failure
        return [f'<exfat_read: {e}>']


def tools_missing(kind):
    need = ['mkfs.fat', 'fsck.fat', 'mdir'] if kind == 'fat' else ['mkfs.exfat', 'fsck.exfat']
    return [t for t in need if not shutil.which(t)]


def main():
    work = Path(tempfile.mkdtemp(prefix='growth-cut-'))
    failures, cuts, ran, skipped, torn = [], 0, 0, set(), 0
    try:
        conf = ffbuild.make_conf(work / 'conf', CFG)
        tool = ffbuild.build(work / 'growth_cut', [HERE / 'growth_cut.c', ffbuild.F / 'exfat_repair.c'], conf,
                             ['GBASD_FF_FAT_STRETCH_FIX=0'] if UNFIXED else [])
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
        for label, kind, mb, opts in CARDS:
            miss = tools_missing(kind)
            if miss:
                skipped.add(f'{kind} ({", ".join(miss)} missing)')
                continue
            ran += 1
            blank = work / 'blank.img'
            blank.unlink(missing_ok=True)
            with blank.open('wb') as f:
                f.truncate(mb << 20)
            p = run(['mkfs.fat', *opts, '-S', '512', blank] if kind == 'fat' else ['mkfs.exfat', *opts, blank])
            if p.returncode:
                sys.exit(f'{label}: mkfs failed: {p.stdout}{p.stderr}')
            card_cuts = 0
            for start, layout in LAYOUTS:
                base = work / 'base.img'
                p = run([tool, 'prep', blank, base, FOLDER, start, layout], env=env)
                if p.returncode or fsck(kind, base):
                    sys.exit(f'{label} prep {layout}: {p.stdout}{p.stderr}{fsck(kind, base)}')
                old = listing(kind, base)
                assert old and NEW not in old, old
                p = run([tool, 'cut', base, work / 'full.img', FOLDER, -1], env=env)
                m, g = re.search(r'WRITES (\d+)', p.stdout), re.search(r'GREW (\d+)', p.stdout)
                if p.returncode or not m or not g or g.group(1) != '1':
                    sys.exit(f'{label} {layout}: clean growth: {p.stdout}{p.stderr}')
                if listing(kind, work / 'full.img') != sorted(old + [NEW]) or fsck(kind, work / 'full.img'):
                    sys.exit(f'{label} {layout}: clean growth does not list old + new')
                n = int(m.group(1))
                for k in range(n + 1):
                    what = f'{label}, {layout}, folder {start}->{start + 1} clusters, cut after {k}/{n} writes'
                    cut, after = work / 'cut.img', work / 'after.img'
                    p = run([tool, 'cut', base, cut, FOLDER, k], env=env)
                    if p.returncode:
                        failures.append(f'{what}: {p.stdout}{p.stderr}')
                        continue
                    cuts += 1
                    card_cuts += 1
                    raw_bad, raw_seen = fsck(kind, cut), listing(kind, cut)
                    # The apps' start: exfat_repair_dir on the folder (exFAT: an entry
                    # block the cut left half written is completed or removed; FAT: no-op).
                    fixed = work / 'fixed.img'
                    p = run([tool, 'repair', cut, fixed, FOLDER], env=env)
                    m = re.search(r'REPAIRED (-?\d+)', p.stdout)
                    if p.returncode or not m:
                        failures.append(f'{what}: exfat_repair_dir: {p.stdout}{p.stderr[-300:]}')
                        continue
                    repaired = int(m.group(1))
                    if kind == 'fat' and (repaired or fixed.read_bytes() != cut.read_bytes()):
                        failures.append(f'{what}: exfat_repair_dir changed a FAT card')
                    if repaired:
                        torn += 1
                    elif raw_bad or raw_seen not in (old, sorted(old + [NEW])):
                        # without a repair the card as the cut left it must already be right
                        failures.append(f'{what}: after the cut: fsck {raw_bad[:4]}, listing '
                                        f'{"ok" if raw_seen in (old, sorted(old + [NEW])) else "WRONG"}: '
                                        f'not written {sorted(set(raw_seen or []) - set(old) - {NEW})[:4]}, '
                                        f'missing {sorted(set(old) - set(raw_seen or []))[:4]}')
                        continue
                    cut = fixed
                    bad = fsck(kind, cut)
                    if bad:
                        failures.append(f'{what}: fsck after the cut: {bad[:4]}')
                    seen = listing(kind, cut)
                    if seen not in (old, sorted(old + [NEW])):
                        extra = sorted(set(seen or []) - set(old) - {NEW})
                        failures.append(f'{what}: the folder lists neither the old nor the new entries '
                                        f'(not written: {extra[:4]}{"..." if len(extra) > 4 else ""}, '
                                        f'missing: {sorted(set(old) - set(seen or []))[:4]})')
                        continue
                    p = run([tool, 'after', cut, after, FOLDER], env=env)
                    if p.returncode:
                        failures.append(f'{what}: FatFs after the reboot: {p.stdout.strip().splitlines()[-1:]}'
                                        f'{p.stderr[-300:]}')
                        continue
                    ours = sorted(l[5:] for l in p.stdout.splitlines() if l.startswith('LIST '))
                    if ours != seen:
                        failures.append(f'{what}: FatFs lists differently: {sorted(set(ours) ^ set(seen))[:4]}')
                    if 'NEWFILE BAD' in p.stdout:
                        failures.append(f'{what}: the new file holds wrong data')
                    ours2 = sorted(l[6:] for l in p.stdout.splitlines() if l.startswith('LIST2 '))
                    if ours2 != listing(kind, after):
                        failures.append(f'{what}: after growing again, FatFs and the independent listing differ')
                    bad = fsck(kind, after)
                    if bad:
                        failures.append(f'{what}: fsck after growing again: {bad[:4]}')
            print(f'  {label}: {card_cuts} cuts', flush=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    for f in failures[:6 if UNFIXED else 25]:
        print('expected failure:' if UNFIXED else 'FAIL', f)
    name = f'folder-growth power cut ({CFG} switches{", FAT stretch fix OFF" if UNFIXED else ""})'
    if skipped:
        print('SKIP: ' + '; '.join(sorted(skipped)))
    if not ran:
        return 0
    print(f'{name}: {cuts} cuts, {len(failures)} failures ({torn} exFAT entry blocks left half written, '
          f'repaired by exfat_repair_dir)')
    if UNFIXED and any(k.startswith('fat') for k in skipped):
        print('SKIP: the unfixed check needs the FAT tools')
        return 0
    if UNFIXED:   # R0.15's order must show the damage on FAT, and only there
        fat_fail = [f for f in failures if f.startswith('FAT')]
        other = [f for f in failures if not f.startswith('FAT')]
        ok = bool(fat_fail) and not other
        print(f'unfixed FAT code {"fails as expected" if ok else "DID NOT fail as expected"} '
              f'({len(fat_fail)} FAT failures, {len(other)} exFAT failures)')
        return 0 if ok else 1
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
