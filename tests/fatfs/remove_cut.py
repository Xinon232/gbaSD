#!/usr/bin/env python3
"""Power cut while a file with a long name is deleted or renamed, on FAT12,
FAT16, FAT32 (512-byte and larger clusters) and exFAT.

A file with a long name has several entries in its folder: on FAT its
long-name (LFN) entries and its 8.3 short-name entry (SFN) last, on exFAT a
file, a stream and name entries. The test puts such a file at every offset
in a sector (0-15), so its entries cross a sector boundary at every point
(a 22-character name: 3 FAT entries; a 250-character name: 21 entries, up
to three sectors), in the root folder and in /A, and then

  - f_unlink deletes it,
  - f_rename renames it in the same folder,
  - f_rename moves it to /B under a new name,

the power cut after each sector write in turn (remove_cut.c). After each cut
("reboot", as the apps start: exfat_repair_dir on the folders first; on FAT
it must do nothing):

  - an independent listing of the folder and of /B (mtools mdir -b for FAT,
    tests/exfat_kit/exfat_read.py for exFAT, no FatFs code) shows the file
    under its old name, its new name, or (rename: the cut after the new
    entry reached the card and before the old one was deleted, R0.15's
    "critical section" of f_rename, unchanged) under both, and every other
    file as it was; never under any other name: no 8.3 alias (ADIARY~1.TXT)
    of a long-named file;
  - fsck.fat -n / fsck.exfat -n: clean, or only the leftovers in ALLOWED;
  - FatFs lists the same, the file reads back under each name it has, and
    FatFs creates, renames and deletes files with long names in both
    folders; after a remount the listings still agree and fsck is as above.

R0.15 deleted a FAT entry block in order (LFN entries first): a cut between
the two sector writes of a block over a sector boundary left the SFN entry
alone, the file visible under its 8.3 alias. With GBASD_FF_FAT_REMOVE_ORDER_FIX
0 this test must fail (--unfixed checks that it does, on FAT only).

  python3 tests/fatfs/remove_cut.py [--config=off|on] [--unfixed] [--quick]

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
if QUICK:
    CARDS = [c for c in CARDS if '512 B' in c[0]]
# exFAT: /A only. R0.15 cannot grow an exFAT root by two clusters in one
# entry-set allocation (a set longer than a cluster: 512-byte clusters and a
# name of about 200 characters or more): after the first new cluster dir_next
# sets obj.stat bit 2, and get_fat then no longer reads the root's FAT chain
# (it does so only with stat 0), so the second stretch fails with FR_INT_ERR
# (nothing written wrong; the file is not created). Not a power-cut issue and
# not part of this test; the apps keep their files in sub-folders.
LOCS = ['/', '/A']
# (old name, same-folder new name, new name in /B)
LONG = 'A long name of 250 characters' + ''.join(f' part {k:02d}' for k in range(27)) + ' end.txt'
LONG = LONG[:246] + '.txt'
NAMES = [('A diary 13.07.2026.txt', 'A diary 13.07.2026 renamed.txt', 'A diary moved to B.txt'),
         (LONG, 'Renamed - ' + LONG[10:], 'Moved - ' + LONG[8:])]
assert all(len(n) == 250 for n in NAMES[1])
# exFAT with 512-byte clusters: 225 characters (17 entries) instead of 250 (19
# entries). A set of 19 entries can span three clusters there, and
# fsck.exfat (exfatprogs 1.2.2) then reports a bad checksum although the set
# is sound (FatFs and exfat_read read it); 17 entries span two at most.
NAMES_EXFAT_512 = [NAMES[0], tuple(n[:221] + '.txt' for n in NAMES[1])]
OPS = ['unlink', 'rename', 'move']

# fsck reports a cut may leave, all harmless (nothing visible, nothing
# damaged; a PC's fsck removes or frees them):
ALLOWED = [
    # FAT: long-name entries without their short-name entry (no file; every
    # reader skips them): the old block's LFN entries in the sector(s) before
    # its SFN sector when the cut came after the SFN sector was written (the
    # gbaSD order: the file is gone, as intended), or a new block (rename)
    # whose LFN entries reached the card and its SFN entry did not (R0.15).
    re.compile(r'^Orphaned long file name part '),
    re.compile(r'^A new long file name starts within an old one\.'),   # the same leftover, a block after it
    # FAT: the same for a block over three sectors (a name of about 200
    # characters or more): the cut came after its first LFN sector was
    # written too, so the LFN entries left in the middle sector have no start.
    re.compile(r'^Long filename fragment ".*" found outside a LFN sequence\.$'),
    re.compile(r'^Leaving filesystem unchanged\.'),
    # FAT, f_unlink: the cut came after the entry was deleted and before the
    # file's clusters were freed (R0.15 order).
    re.compile(r'^Reclaimed \d+ unused clusters? \(\d+ bytes\)'),
    re.compile(r'^Free cluster summary wrong'),          # FAT32 FSINFO free count (a hint, recomputed)
    re.compile(r'^FATs differ but appear to be intact\.'),   # a cut between a FAT sector and its copy
    re.compile(r'^Using first FAT\.'),
    # exFAT: a cluster marked in the bitmap that nothing uses (f_unlink cut
    # after the entry set was deleted, before the bitmap was cleared)
    re.compile(r'cluster is marked as allocated, but not used|bitmap'),
    # exFAT: the new entry set (rename) grew the folder: the cut came after
    # the cluster was taken and cleared, before the new length was written
    # (as in growth_cut.py)
    re.compile(r'^ERROR: /[AB]: more clusters are allocated'),
]
# FAT, f_rename, only while both names are listed: the cut came after the new
# entry reached the card and before the old one was deleted, so two entries
# hold the same clusters (R0.15's "critical section" of f_rename, unchanged;
# both names read the same data). fsck.fat -n prints:
#   /OLD  and
#   /NEW
#     share clusters.
#     Truncating second to 0 bytes.
#   /NEW
#     File size is N bytes, cluster chain length is 0 bytes.
#     Truncating file to 0 bytes.
SHARED = [re.compile(r'^/.* and$'), re.compile(r'^share clusters\.$'), re.compile(r'^Truncating second to 0 bytes\.$'),
          re.compile(r'^File size is \d+ bytes, cluster chain length is 0 bytes\.$'),
          re.compile(r'^Truncating file to 0 bytes\.$'),
          # exFAT, the same state: "ERROR: /NEW: cluster is already allocated for the other file. truncated to 0 bytes"
          re.compile(r'^ERROR: /.*: cluster is already allocated for the other file\.')]


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


def fsck(kind, img, shared=False):
    """Reports fsck makes that are not in ALLOWED (shared: or SHARED) ([] = clean or harmless)."""
    allowed = ALLOWED + SHARED if shared else ALLOWED
    p = run(['fsck.fat', '-n', img] if kind == 'fat' else ['fsck.exfat', '-n', img])
    if p.returncode == 0:
        return []
    lines = [l.strip() for l in (p.stdout + p.stderr).splitlines() if l.strip()]
    if kind == 'fat':
        body = [l for l in lines[1:] if not re.match(r'^\S+: \d+ files, \d+/\d+ clusters', l)]
        bad, keep = [], False
        for l in body:
            if any(a.search(l) for a in allowed):
                keep = True
                continue
            if keep and (l.startswith(('Auto', 'Not auto-correcting', '(Maybe the start bit is missing', '1)', '2)', '3)', '4)', '/'))
                         or 'vs. really' in l):
                continue
            keep = False
            bad.append(l)
        return bad
    bad = [l for l in lines if 'ERROR' in l and not any(a.search(l) for a in allowed)]
    return bad if bad or any('ERROR' in l for l in lines) else [f'fsck.exfat exit {p.returncode}: {lines[-3:]}']


def listing(kind, img, loc):
    """(names in loc, names in /B), read without FatFs (None: the card cannot be read)."""
    if kind == 'fat':
        p = run(['mdir', '-/', '-b', '-i', img, '::/'])
        if p.returncode:
            return None
        out = {}
        for l in p.stdout.splitlines():
            if l.startswith('::/'):
                folder, _, name = l[2:].rstrip('/').rpartition('/')
                out.setdefault(folder or '/', []).append(name)
        return tuple(tuple(sorted(out.get(f, []))) for f in (loc, '/B'))
    try:
        v = Volume(str(img))
        return tuple(tuple(sorted(name for name, d, size, first, cont in v.listdir(f))) for f in (loc, '/B'))
    except Exception as e:  # noqa: BLE001 - a folder the reader cannot walk is a failure
        return ((f'<exfat_read: {e}>',), ())


class Image:
    """A card image kept as its non-zero 64 KB chunks: writing a copy is quick (most of a card is zeros)."""
    def __init__(self, path):
        data = Path(path).read_bytes()
        self.size, z = len(data), bytes(1 << 16)
        self.chunks = [(o, data[o:o + (1 << 16)]) for o in range(0, len(data), 1 << 16) if data[o:o + (1 << 16)] != z]

    def write(self, path):
        with open(path, 'wb') as f:
            f.truncate(self.size)
            for o, c in self.chunks:
                f.seek(o)
                f.write(c)


def tools_missing(kind):
    need = ['mkfs.fat', 'fsck.fat', 'mdir'] if kind == 'fat' else ['mkfs.exfat', 'fsck.exfat']
    return [t for t in need if not shutil.which(t)]


def states(op, loc, old, new, before_loc, before_b):
    """{(listing of loc, listing of /B): 'old' | 'new' | 'both'}: what a cut may leave."""
    gone = tuple(sorted(set(before_loc) - {old}))
    if op == 'unlink':
        return {(before_loc, before_b): 'old', (gone, before_b): 'new'}
    if op == 'rename':
        return {(before_loc, before_b): 'old', (tuple(sorted(gone + (new,))), before_b): 'new',
                (tuple(sorted(before_loc + (new,))), before_b): 'both'}
    nb = tuple(sorted(before_b + (new,)))
    return {(before_loc, before_b): 'old', (gone, nb): 'new', (before_loc, nb): 'both'}


def main():
    shm = Path('/dev/shm')
    work = Path(tempfile.mkdtemp(prefix='remove-cut-', dir=shm if shm.is_dir() and os.access(shm, os.W_OK) else None))
    failures, cuts, ran, skipped, torn, both, crossing = [], 0, 0, set(), 0, 0, 0
    try:
        conf = ffbuild.make_conf(work / 'conf', CFG)
        tool = ffbuild.build(work / 'remove_cut', [HERE / 'remove_cut.c', ffbuild.F / 'exfat_repair.c'], conf,
                             ['GBASD_FF_FAT_REMOVE_ORDER_FIX=0'] if UNFIXED else [])
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
        for label, kind, mb, opts in CARDS:
            miss = tools_missing(kind)
            if miss:
                skipped.add(f'{kind} ({", ".join(miss)} missing)')
                continue
            ran += 1
            blank, base, cut = (work / f for f in ('blank.img', 'base.img', 'cut.img'))
            blank.unlink(missing_ok=True)
            with blank.open('wb') as f:
                f.truncate(mb << 20)
            p = run(['mkfs.fat', *opts, '-S', '512', blank] if kind == 'fat' else ['mkfs.exfat', *opts, blank])
            if p.returncode:
                sys.exit(f'{label}: mkfs failed: {p.stdout}{p.stderr}')
            card_cuts = 0
            for loc in LOCS if kind == 'fat' else ['/A']:   # exFAT: see LOCS
                for old, new_same, new_b in NAMES_EXFAT_512 if kind == 'exfat' and '512 B' in label else NAMES:
                    for off in (range(16) if len(old) > 100 else range(12, 16)):
                        shutil.copyfile(blank, base)
                        p = run([tool, 'prep', base, loc, old, off], env=env)
                        m = re.search(r'BLOCK (\d+) (\d+)', p.stdout)
                        if p.returncode or not m or fsck(kind, base):
                            sys.exit(f'{label} prep {loc} {old[:20]} @{off}: {p.stdout}{p.stderr}{fsck(kind, base)}')
                        img = Image(base)
                        crossing += int(m.group(1)) // 16 != int(m.group(2)) // 16
                        before_loc, before_b = listing(kind, base, loc)
                        assert old in before_loc, before_loc
                        for op in OPS:
                            new = new_b if op == 'move' else new_same
                            ok_states = states(op, loc, old, new, before_loc, before_b)
                            img.write(cut)
                            p = run([tool, 'cut', cut, loc, op, old, new, -1], env=env)
                            m = re.search(r'WRITES (\d+)', p.stdout)
                            full = listing(kind, cut, loc)
                            if p.returncode or not m or ok_states.get(full) != 'new' or fsck(kind, cut):
                                sys.exit(f'{label} {loc} {op} {old[:20]} @{off}: clean run: {p.stdout}{p.stderr}'
                                         f'{full}{fsck(kind, cut)}')
                            n = int(m.group(1))
                            for k in range(n + 1):
                                what = f'{label}, {loc}, {op} "{old[:22]}" ({len(old)} chars) at entry {off}, cut after {k}/{n} writes'
                                img.write(cut)
                                p = run([tool, 'cut', cut, loc, op, old, new, k], env=env)
                                if p.returncode:
                                    failures.append(f'{what}: {p.stdout}{p.stderr}')
                                    continue
                                cuts += 1
                                card_cuts += 1
                                if kind == 'exfat':   # (on FAT it does nothing: growth_cut.py checks that)
                                    p = run([tool, 'repair', cut, loc], env=env)
                                    m = re.search(r'REPAIRED (-?\d+)', p.stdout)
                                    if p.returncode or not m:
                                        failures.append(f'{what}: exfat_repair_dir: {p.stdout}{p.stderr[-300:]}')
                                        continue
                                    torn += int(m.group(1)) > 0
                                seen = listing(kind, cut, loc)
                                state = ok_states.get(seen)
                                if state is None:
                                    seen = seen or ((), ())
                                    odd = sorted((set(seen[0]) | set(seen[1]))
                                                 - set(before_loc) - set(before_b) - {new})
                                    alias = [x for x in odd if '~' in x]
                                    failures.append(f'{what}: the folders list '
                                                    + (f'an 8.3 alias {alias}' if alias else f'{odd[:3]}, missing '
                                                       f'{sorted((set(before_loc) | set(before_b)) - set(seen[0]) - set(seen[1]))[:3]}'))
                                    continue
                                both += state == 'both'
                                bad = fsck(kind, cut, state == 'both')
                                if bad:
                                    failures.append(f'{what}: fsck after the cut: {bad[:4]}')
                                p = run([tool, 'after', cut, loc, old, new], env=env)
                                if p.returncode:
                                    failures.append(f'{what}: FatFs after the reboot: '
                                                    f'{p.stdout.strip().splitlines()[-1:]}{p.stderr[-300:]}')
                                    continue
                                out = p.stdout.splitlines()

                                def fatfs_list(tag):
                                    got = {}
                                    for l in out:
                                        if l.startswith(tag + ' '):
                                            f_, _, nm = l[len(tag) + 1:].partition(' ')
                                            got.setdefault(f_, []).append(nm)
                                    return (tuple(sorted(got.get(loc, []))), tuple(sorted(got.get('/B', []))))
                                if fatfs_list('LIST') != seen:
                                    failures.append(f'{what}: FatFs lists differently')
                                reads = [l for l in out if l.startswith('READ ')]
                                # the file under each name listed (unlink, new: none), and the file FatFs created after
                                want = {'old': 1, 'new': 0 if op == 'unlink' else 1, 'both': 2}[state] + 1
                                if any(not l.endswith(' ok') for l in reads) or len(reads) != want:
                                    failures.append(f'{what}: the file does not read back: {reads}')
                                if fatfs_list('LIST2') != listing(kind, cut, loc):
                                    failures.append(f'{what}: after FatFs went on, FatFs and the independent listing differ')
                                bad = fsck(kind, cut, state == 'both')
                                if bad:
                                    failures.append(f'{what}: fsck after FatFs went on: {bad[:4]}')
            print(f'  {label}: {card_cuts} cuts', flush=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    for f in failures[:6 if UNFIXED else 40]:
        print('expected failure:' if UNFIXED else 'FAIL', f)
    name = f'delete / rename power cut ({CFG} switches{", FAT remove order fix OFF" if UNFIXED else ""})'
    if skipped:
        print('SKIP: ' + '; '.join(sorted(skipped)))
    if not ran:
        return 0
    aliases = sum('8.3 alias' in f for f in failures)
    print(f'{name}: {cuts} cuts ({crossing} entry blocks over a sector boundary), {len(failures)} failures '
          f'({aliases} 8.3 aliases); {both} renames cut with both names listed (R0.15), '
          f'{torn} exFAT entry sets repaired by exfat_repair_dir')
    if UNFIXED and any(k.startswith('fat') for k in skipped):
        print('SKIP: the unfixed check needs the FAT tools')
        return 0
    if UNFIXED:   # R0.15's order must leave aliases on FAT, and only there
        fat_fail = [f for f in failures if f.startswith('FAT')]
        other = [f for f in failures if not f.startswith('FAT')]
        ok = bool(fat_fail) and not other
        print(f'unfixed FAT code {"fails as expected" if ok else "DID NOT fail as expected"} '
              f'({len(fat_fail)} FAT failures, {len(other)} exFAT failures)')
        return 0 if ok else 1
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
