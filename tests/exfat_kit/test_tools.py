#!/usr/bin/env python3
"""Self-test of the exFAT stand-ins (bin/): the same mtools steps on a FAT16
card with the real mtools and on an exFAT card with the stand-ins must give
the same exit codes and output (mdir -b, mtype, mattrib, card -> computer
copies); the exFAT card must be exFAT, fsck.exfat clean, and laid out as
asked (read with exfat_read.py, not FatFs): contiguous (NoFatChain), or with
EXFAT_FRAGMENT=1 fragmented (FAT chains) in the given EXFAT_FRAGMENT_MODE.
Adapted from gbaplanner's tests/exfat/test_tools.py.

  EXFAT_TOOL=... [EXFAT_FRAGMENT=1 EXFAT_FRAGMENT_MODE=pairs|files|put] python3 test_tools.py
(tests/exfat_kit/run.sh runs every mode). Needs mtools, dosfstools, exfatprogs."""
import filecmp, os, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from exfat_read import Volume  # noqa: E402

FRAG = os.environ.get('EXFAT_FRAGMENT') == '1'
MODE = os.environ.get('EXFAT_FRAGMENT_MODE', 'pairs') if FRAG else 'contiguous'
BIN = str(HERE / 'bin')


def steps(card, fs, work):
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', GBASD_FS=fs, PATH=BIN + os.pathsep + os.environ['PATH'],
               EXFAT_FSCK='1')
    out = []
    c = str(card)

    def run(*cmd, data=False, keep=True):
        p = subprocess.run(list(cmd), env=env, capture_output=True)
        if keep:
            text = p.stdout if data else p.stdout.decode().replace(str(work), 'WORK')
            if cmd[0] == 'mdir':   # mtools lists in entry order, the stand-in may differ: compare as sets
                text = ''.join(sorted(text.splitlines(True)))
            out.append((' '.join(cmd).replace(c, 'CARD').replace(str(work), 'WORK').replace(f'got-{fs}', 'got'),
                        p.returncode, text))
        return p

    subprocess.run(['truncate', '-s', '64M', c], check=True)
    subprocess.run(['mkfs.fat', '-F', '16', '-n', 'KIT', c], env=env, check=True, capture_output=True)
    big = bytes((i * 7 + i // 300) & 0xFF for i in range(150_000))
    small = b'2026-10-27 a line of text\n'
    tree = work / f'tree-{fs}'
    (tree / 'sub').mkdir(parents=True)
    for i in range(40):
        (tree / f'file {i:02d} with a long name.txt').write_bytes(small * (i + 1) * 50)
    (tree / 'sub' / 'inner.bin').write_bytes(big[:5000])
    for name, data in (('/app/data.txt', big), ('/app/todo.txt', small), ('/app/APP.SYS/STATE0.DAT', small * 3)):
        tmp = work / 'put.tmp'
        tmp.write_bytes(data)
        parts = name.strip('/').split('/')
        for i in range(1, len(parts)):
            run('mmd', '-D', 's', '-i', c, '::/' + '/'.join(parts[:i]))
        run('mcopy', '-o', '-i', c, str(tmp), '::' + name)
    tmp.write_bytes(small * 2)
    run('mcopy', '-o', '-i', c, str(tmp), '::/app/todo.txt')        # replaces it
    run('mmd', '-i', c, '::/app')                                    # exists: fails
    run('mmd', '-i', c, '::/nothere/deeper')                         # no parent: fails
    run('mcopy', '-s', '-i', c, str(tree), '::/app')                 # a folder tree
    for n in ('/app/data.txt', '/app/todo.txt', '/app/nothere.txt'):
        run('mtype', '-i', c, '::' + n, data=True)
    for n in ('/app', '/app/APP.SYS', '/app/tree-' + fs, '/app/tree-' + fs + '/file 1*', '/nothere'):
        run('mdir', '-a', '-b', '-i', c, '::' + n)
    sysdir = '::/app/APP.SYS'
    run('mattrib', '-i', c, sysdir)
    run('mattrib', '-i', c, '+h', '+s', sysdir)
    run('mattrib', '-i', c, sysdir)
    run('mattrib', '-i', c, '-h', '-s', sysdir)
    run('mattrib', '-i', c, '+r', '::/app/todo.txt')
    run('mattrib', '-i', c, '::/app/todo.txt')
    run('mattrib', '-i', c, '-r', '::/app/todo.txt')
    run('mren', '-i', c, '::/app/todo.txt', '::/app/done.txt')
    run('mdel', '-i', c, '::/app/tree-' + fs + '/file 0*')
    run('mdir', '-b', '-i', c, '::/app/tree-' + fs)
    run('mdel', '-i', c, '::/app/nothere.txt')
    got = work / f'got-{fs}'
    got.mkdir()
    run('mcopy', '-s', '-i', c, '::/app', str(got))
    run('mcopy', '-i', c, '::/app/done.txt', str(got / 'done-copy.txt'))
    run('fsck.vfat', '-n', c, keep=False)
    return [(a.replace('tree-' + fs, 'tree'), b, d.replace('tree-' + fs, 'tree') if isinstance(d, str) else d)
            for a, b, d in out], big, got


def main():
    with tempfile.TemporaryDirectory() as t:
        work = Path(t)
        fat, _, got_fat = steps(work / 'fat.img', 'fat', work)
        ex, big, got_ex = steps(work / 'exfat.img', 'exfat', work)
        bad = [(a, b) for a, b in zip(fat, ex) if a != b]
        for a, b in bad:
            print(f'FAT  : {a}\nexFAT: {b}')
        assert not bad and len(fat) == len(ex), 'exFAT stand-ins differ from mtools on FAT'
        (got_ex / 'app' / 'tree-exfat').rename(got_ex / 'app' / 'tree-fat')
        cmp = filecmp.dircmp(got_fat, got_ex)

        def same(d):
            return not d.left_only and not d.right_only and not d.diff_files and not d.funny_files and \
                all(same(s) for s in d.subdirs.values())
        assert same(cmp), 'mcopy -s from the card gives different trees on FAT and exFAT'
        img = str(work / 'exfat.img')
        with open(img, 'rb') as f:
            assert f.read(11)[3:11] == b'EXFAT   ', 'stand-in mkfs.fat did not make exFAT'
        p = subprocess.run(['fsck.exfat', '-n', img], capture_output=True, text=True)
        assert p.returncode == 0, p.stdout + p.stderr
        v = Volume(img)
        assert v.file('/app/data.txt') == big, 'data.txt read back by exfat_read differs'
        e = v.find('/app/data.txt')
        if FRAG and MODE != 'none':
            assert not e[4], f'data.txt should be fragmented ({MODE}): {e}'
            clusters = v.chain(e[3], e[2], e[4])
            gaps = sum(1 for a, b in zip(clusters, clusters[1:]) if b != a + 1)
            assert gaps > len(clusters) // 4, f'data.txt barely fragmented ({gaps} gaps)'
        if not FRAG:
            assert e[4], f'data.txt should be contiguous: {e}'
        tree = [x for x in v.listdir('/app/tree-exfat') if x[2] > v.cluster]
        if FRAG:
            assert any(not x[4] for x in tree), 'mcopy -s should write fragmented files'
        print(f'PASS exFAT stand-ins ({MODE}, {v.cluster}-byte clusters): {len(ex)} mtools steps as on FAT16, '
              f'mcopy -s trees equal, card fsck.exfat clean, data.txt '
              f'{"in a FAT chain" if not e[4] else "contiguous (NoFatChain)"}')


if __name__ == '__main__':
    main()
