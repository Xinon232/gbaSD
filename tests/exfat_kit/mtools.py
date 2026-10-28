"""Stand-ins for mtools and dosfstools on exFAT test cards, so an app's
existing card tests run unchanged on exFAT. The app's test runner puts
tests/exfat_kit/bin first on PATH; each bin/<command> calls main() with its
name. On a FAT card (and for mkfs without exFAT asked for) every command
hands over to the real tool, the next one on PATH. See README.md.

From gbamp3's tests/exfat/mtools.py and the versions grown from it in
gbaplanner, gbawriter, gbavocab and gbareader, merged.

Environment:
  EXFAT_TOOL=PATH        exfat_tool built against the app's FatFs (build.py);
                         default: build/exfat_tool next to this file
  GBASD_FS=exfat         mkfs.fat / mkfs.vfat / mformat make exFAT cards
                         (mkfs.exfat). GBAPLANNER_FS, GBAWRITER_FS, GBAVOCAB_FS,
                         GBAREADER_FS, GBAMP3_FS=exfat work too (the apps' names)
  EXFAT_CLUSTER=BYTES    cluster size; else mkfs.fat -s / mformat -c; else
                         mkfs.exfat's default (4 KB on 64 MB)
  EXFAT_FRAGMENT=1       fragmented cards; EXFAT_FRAGMENT_MODE says how:
      pairs (default)    after mkfs: every other cluster of the free space taken
                         (exfat_tool fragment EXFAT_FRAGMENT_MB, 0 = all of it,
                         or EXFAT_FRAGMENT_PAIRS cluster pairs; the file kept is
                         EXFAT_FRAGMENT_FILE, default /FRAGMENT.BIN)
      files              after mkfs: EXFAT_FRAGMENT_CLUSTERS (default 2048)
                         one-cluster files at the start of the free space in
                         /.fragment, every other one deleted (holes with text)
      put                nothing after mkfs; each file mcopy puts is written in
                         a FAT chain and leaves holes behind
      none               nothing after mkfs
    and in every mode mcopy -s writes a folder's files a cluster in turn.
  EXFAT_FSCK=1           every command first runs fsck.exfat -n on the card
                         (a card the app damaged fails the test)
  EXFAT_LAYOUT_LOG=FILE  fsck appends the card's tree (contiguous / fat-chain
                         per object, read by exfat_read.py, not FatFs), mcopy a
                         line 'put PATH SIZE CLUSTER contiguous|fat-chain' per
                         file it puts

Supported (what the apps' tests use): mkfs.fat / mkfs.vfat -C -F -n -s -S
IMG [KBLOCKS], mformat -i IMG -T -c, fsck.fat / fsck.vfat [-n] IMG, mmd [-D s],
mcopy [-o -n -s -b -p -m -Q -v] (computer -> card, card -> computer, folders
with -s), mtype, mdir [-a -b], mattrib [+-][ahrs]... PATH and mattrib PATH,
mdel, mren, and a '*' or '?' in the last part of an mdir / mdel / mcopy /
mattrib card path. Output follows mtools 4: mdir -b '::/path' (folders with
a '/'), mattrib '  A  SHR     ::/path'."""
import fnmatch, os, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
NAME = None   # the command (bin/<name> calls main)
MKFS = ('mkfs.fat', 'mkfs.vfat')
FSCK = ('fsck.fat', 'fsck.vfat')
FS_VARS = ('GBASD_FS', 'GBAPLANNER_FS', 'GBAWRITER_FS', 'GBAVOCAB_FS', 'GBAREADER_FS', 'GBAMP3_FS')
VALUED = {'mkfs.fat': {'-F', '-n', '-s', '-S', '-c', '-r', '-f', '-R', '-i', '-h', '-a', '-b', '-m', '-D', '-M'},
          'mformat': {'-T', '-c', '-h', '-s', '-S', '-t', '-r', '-L', '-M', '-N', '-v'},
          'mmd': {'-D'}, 'mcopy': {'-D'}, 'mren': {'-D'}}
VALUED['mkfs.vfat'] = VALUED['mkfs.fat']
ATTR_CHANGE = lambda a: a[:1] in '+-' and len(a) > 1 and set(a[1:].lower()) <= set('ahrs')  # noqa: E731
LISTED = {}   # folder listings during one command (its own writes come after its looks)


def tool_path():
    t = os.environ.get('EXFAT_TOOL') or str(HERE / 'build' / 'exfat_tool')
    if not Path(t).exists():
        sys.exit(f'{NAME}: exfat_tool not found ({t}); build it with tests/exfat_kit/build.py and set EXFAT_TOOL')
    return t


def real():
    """Runs the real tool of the same name (the next one on PATH)."""
    path = [p for p in os.environ.get('PATH', '').split(os.pathsep) if p and Path(p).resolve() != (HERE / 'bin').resolve()]
    path += ['/usr/sbin', '/sbin']
    exe = shutil.which(NAME, path=os.pathsep.join(path))
    if not exe:
        sys.exit(f'{NAME}: the real tool is not installed')
    os.execv(exe, [exe] + sys.argv[1:])


def options(args):
    """'-i IMG', flags, positional arguments and option values."""
    img, flags, pos, vals = None, set(), [], {}
    i = 0
    while i < len(args):
        a = args[i]
        if a == '-i' and NAME not in MKFS:
            img = args[i + 1]; i += 2; continue
        if a in VALUED.get(NAME, ()):
            vals[a] = args[i + 1]; i += 2; continue
        if NAME == 'mattrib' and ATTR_CHANGE(a):
            pos.append(a); i += 1; continue
        if a.startswith('-') and len(a) > 1:
            flags.add(a); i += 1; continue
        pos.append(a); i += 1
    return img, flags, pos, vals


def is_exfat(img):
    try:
        with open(img, 'rb') as f:
            return f.read(11)[3:11] == b'EXFAT   '
    except OSError:
        return False


def tool(img, *a, capture=False, binary=False):
    p = subprocess.run([tool_path(), img, *a], capture_output=capture, text=capture and not binary)
    if p.returncode:
        if capture:
            err = p.stderr.decode(errors='replace') if binary else p.stderr
            sys.stderr.write(err)
        sys.exit(1)
    LISTED.clear()
    return p.stdout if capture else None


def card_path(p):
    assert p.startswith('::'), p
    p = p[2:] or '/'
    return p if p.startswith('/') else '/' + p


def entries(img, path):
    """[(is_dir, size, name)] of a folder, or [(False, size, name)] for a file; None if missing."""
    if path not in LISTED:
        p = subprocess.run([tool_path(), img, 'ls', path], capture_output=True, text=True)
        out = None
        if not p.returncode:
            out = []
            for line in p.stdout.splitlines():
                k, size, name = line.split(' ', 2)
                out.append((k == 'D', int(size), name))
        LISTED[path] = out
    return LISTED[path]


def kind(img, path):
    """'D', 'F' or None."""
    if path.rstrip('/') == '':
        return 'D'
    parent, name = path.rstrip('/').rsplit('/', 1)
    for d, _, n in entries(img, parent or '/') or []:
        if n.lower() == name.lower():
            return 'D' if d else 'F'
    return None


def exact(img, path):
    """The path with the names as stored on the card (exFAT keeps case)."""
    out = ''
    for part in [p for p in path.split('/') if p]:
        e = entries(img, out or '/') or []
        out += '/' + next((n for _, _, n in e if n.lower() == part.lower()), part)
    return out or '/'


def matches(img, path):
    """Paths a (possibly wildcard) card path names: [(path, is_dir)]."""
    parent, last = path.rstrip('/').rsplit('/', 1) if path.rstrip('/') else ('', '')
    if any(c in last for c in '*?'):
        e = entries(img, parent or '/') or []
        return [(f'{exact(img, parent) if parent else ""}/{n}', d) for d, _, n in e
                if fnmatch.fnmatch(n.lower(), last.lower())]
    k = kind(img, path)
    return [(exact(img, path), k == 'D')] if k else []


def fsck_card(img):
    p = subprocess.run(['fsck.exfat', '-n', img], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def make_exfat(img, cluster, label):
    cmd = ['mkfs.exfat', '-q']
    if cluster:
        cmd += ['-c', str(cluster)]
    if label:
        cmd += ['-L', label]
    p = subprocess.run(cmd + [img], capture_output=True, text=True)
    if p.returncode:
        sys.exit(f'{NAME} (exFAT stand-in): {" ".join(cmd)} failed:\n{p.stdout}{p.stderr}')
    if os.environ.get('EXFAT_FRAGMENT') == '1':
        mode = os.environ.get('EXFAT_FRAGMENT_MODE', 'pairs')
        if mode == 'pairs':
            amount = (os.environ['EXFAT_FRAGMENT_PAIRS'] + 'c' if os.environ.get('EXFAT_FRAGMENT_PAIRS')
                      else os.environ.get('EXFAT_FRAGMENT_MB', '0'))
            tool(img, 'fragment', amount, os.environ.get('EXFAT_FRAGMENT_FILE', '/FRAGMENT.BIN'))
        elif mode == 'files':
            tool(img, 'fragment-files', os.environ.get('EXFAT_FRAGMENT_CLUSTERS', '2048'))
        elif mode not in ('put', 'none'):
            sys.exit(f'{NAME}: unknown EXFAT_FRAGMENT_MODE {mode}')
        rc, out = fsck_card(img)
        if rc:
            sys.exit(f'{NAME}: the fragmented card is not fsck.exfat clean:\n{out}')


def mkfs(flags, pos, vals):
    """mkfs.fat [-C] [-F N] [-n LABEL] [-s SPC] [-S 512] IMG [KBLOCKS] -> exFAT."""
    img = pos[0]
    if '-C' in flags:
        with open(img, 'wb') as f:
            f.truncate(int(pos[1]) * 1024)
    elif not os.path.exists(img):
        sys.exit(f'{NAME}: {img} does not exist (make it first, e.g. truncate, or use -C)')
    if vals.get('-S', '512') != '512':
        sys.exit(f'{NAME}: only 512-byte sectors on exFAT')
    cluster = os.environ.get('EXFAT_CLUSTER') or (int(vals['-s']) * 512 if '-s' in vals else None)
    make_exfat(img, cluster, vals.get('-n'))
    if '-v' in flags:
        print(f'{NAME} (exFAT stand-in): {img} formatted exFAT')


def mformat(img, vals):
    sectors = int(vals['-T']) if '-T' in vals else os.path.getsize(img) // 512
    with open(img, 'r+b' if os.path.exists(img) else 'wb') as f:
        f.truncate(sectors * 512)
    cluster = os.environ.get('EXFAT_CLUSTER') or (int(vals['-c']) * 512 if '-c' in vals else None)
    make_exfat(img, cluster, None)


def layout_log(img):
    log = os.environ.get('EXFAT_LAYOUT_LOG')
    if not log:
        return
    p = subprocess.run([sys.executable, '-B', str(HERE / 'exfat_read.py'), img], capture_output=True, text=True)
    with open(log, 'a') as f:
        f.write(f'== {img}\n{p.stdout}{p.stderr}')


def log_put(img, path):
    log = os.environ.get('EXFAT_LAYOUT_LOG')
    if not log:
        return
    sys.path.insert(0, str(HERE))
    from exfat_read import Volume
    v = Volume(img)
    name, d, size, first, cont = v.find(path)
    with open(log, 'a') as f:
        f.write(f'put {path} {size} {v.cluster} {"contiguous" if cont else "fat-chain"}\n')


def mcopy(img, flags, pos):
    *srcs, dst = pos
    overwrite = '-o' in flags or '-n' in flags
    if dst.startswith('::'):                      # computer -> card
        d = card_path(dst)
        into = d.endswith('/') or kind(img, d) == 'D' or len(srcs) > 1
        pairs = []
        for s in srcs:
            target = (d.rstrip('/') + '/' + Path(s).name) if into else d
            if os.path.isdir(s):
                if '-s' not in flags:
                    sys.exit(f'mcopy: {s} is a directory')
                tool(img, 'puttree', s, target)
                continue
            if not overwrite and kind(img, target) == 'F':
                sys.exit(f'mcopy: {target} exists (use -o)')
            pairs.append((s, target))
        for i in range(0, len(pairs), 256):
            tool(img, 'put', *[x for pair in pairs[i:i + 256] for x in pair])
        for _, target in pairs:
            log_put(img, target)
    else:                                         # card -> computer
        into = os.path.isdir(dst) or len(srcs) > 1
        for s in srcs:
            found = matches(img, card_path(s))
            if not found:
                sys.exit(f'mcopy: File "{s}" not found')
            for p, is_dir in found:
                name = p.rstrip('/').rsplit('/', 1)[-1]
                if is_dir:
                    if '-s' not in flags:
                        sys.exit(f'mcopy: {s} is a directory')
                    # like mtools: '::/' copies the card's contents into dst, a folder copies as dst/<name>
                    tool(img, 'gettree', p, dst if not name or not into else os.path.join(dst, name))
                else:
                    tool(img, 'get', p, os.path.join(dst, name) if into else dst)


def mdir(img, flags, pos):
    for a in pos or ['::/']:
        path = card_path(a)
        found = matches(img, path)
        if not found:
            sys.exit(f'mdir: File "{a}" not found')
        lines = []
        if len(found) == 1 and found[0][1] and not any(c in path.rsplit('/', 1)[-1] for c in '*?'):
            base = found[0][0].rstrip('/')
            for d, size, n in entries(img, found[0][0]) or []:
                lines.append((f'::{base}/{n}' + ('/' if d else ''), d, size, n))
        else:
            for p, d in found:
                size = 0 if d else entries(img, p)[0][1]
                lines.append((f'::{p}' + ('/' if d else ''), d, size, p.rsplit('/', 1)[1]))
        if '-b' in flags:
            for full, *_ in lines:
                print(full)
        else:
            print(f' Directory for {a}\n')
            for _, d, size, n in lines:
                print(f'{n:<40} {"<DIR>" if d else size}')
            print(f'        {sum(1 for l in lines if not l[1])} files')
            print(f'        {int(tool(img, "free", "/", capture=True))} bytes free')


def mattrib(img, pos):
    changes = [a for a in pos if ATTR_CHANGE(a)]
    for a in (a for a in pos if a not in changes):
        if not a.startswith('::'):
            sys.exit(f'mattrib: unsupported argument {a} on exFAT cards')
        found = matches(img, card_path(a))
        if not found:
            sys.exit(f'mattrib: File "{a}" not found')
        for p, _ in found:
            if changes:
                tool(img, 'chmod', p, *changes)
                continue
            got = tool(img, 'attr', p, capture=True).strip()   # 'ADSHR' with '-' when clear
            mark = lambda c: c if c in got else ' '  # noqa: E731
            print(f'  {mark("A")}  {mark("S")}{mark("H")}{mark("R")}     ::{p}')


def main(name):
    global NAME
    NAME = name
    args = sys.argv[1:]
    img, flags, pos, vals = options(args)
    wants_exfat = any(os.environ.get(v) == 'exfat' for v in FS_VARS)
    if NAME in MKFS:
        if not wants_exfat:
            real()
        return mkfs(flags, pos, vals)
    if NAME == 'mformat':
        if not wants_exfat:
            real()
        return mformat(img, vals)
    if NAME in FSCK:
        if not pos or not is_exfat(pos[-1]):
            real()
        rc, out = fsck_card(pos[-1])
        layout_log(pos[-1])
        sys.stdout.write(out)
        sys.exit(rc)
    if not img or not is_exfat(img):
        real()
    if os.environ.get('EXFAT_FSCK') == '1':      # the card as the last run left it
        rc, out = fsck_card(img)
        if rc:
            sys.exit(f'{NAME}: fsck.exfat finds errors on {img}:\n{out}')
    if NAME == 'mcopy':
        mcopy(img, flags, pos)
    elif NAME == 'mdir':
        mdir(img, flags, pos)
    elif NAME == 'mtype':
        for a in pos:
            found = matches(img, card_path(a))
            if not found or any(d for _, d in found):
                sys.exit(f'mtype: File "{a}" not found')
            for p, _ in found:
                sys.stdout.flush()
                sys.stdout.buffer.write(tool(img, 'get', p, '-', capture=True, binary=True))
    elif NAME == 'mattrib':
        mattrib(img, pos)
    elif NAME == 'mmd':
        for a in pos:
            path = card_path(a).rstrip('/')
            parent = path.rsplit('/', 1)[0] or '/'
            if kind(img, path):
                sys.exit(f'mmd: Directory "{a}" already exists')   # mtools: skipped, exit 1
            if kind(img, parent) != 'D':
                sys.exit(f'mmd: Cannot find directory "{parent}"')
            tool(img, 'mkdir', path)
    elif NAME == 'mdel':
        gone = []
        for a in pos:
            found = matches(img, card_path(a))
            if not found:
                sys.exit(f'mdel: File "{a}" not found')
            for p, d in found:
                if d:
                    sys.exit(f'mdel: {p} is a directory')
                gone.append(p)
        for i in range(0, len(gone), 256):
            tool(img, 'rm', *gone[i:i + 256])
    elif NAME == 'mren':
        old, new = pos
        new = card_path(new)
        if '/' not in new.rstrip('/')[1:]:       # a bare new name stays in the old folder
            new = card_path(old).rsplit('/', 1)[0] + new
        tool(img, 'mv', card_path(old), new)
    else:
        sys.exit(f'{NAME}: not supported on exFAT cards')
