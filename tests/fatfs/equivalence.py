#!/usr/bin/env python3
"""Compares gbaSD's FatFs (fatfs/) with the FatFs copies of the apps it
replaces, as compiled code: each app's ff.c with that app's ffconf.h against
fatfs/ff.c with the same ffconf.h, function by function (objdump -dr of every
.text.* section, and the .rodata / .data contents), with each app's compiler
flags (Butano's for gbaplanner, gbawriter, gbavocab, gbareader; build.py's for
gbamp3) and also with the host gcc.

  gbaSD switches off      (GBASD_FF_FAST_EXFAT_LOOKUP / FSIZE32 / OPEN_ENTRY 0)
                          against the four Butano apps,
  gbaSD switches on       against gbamp3 (player/vendor/fatfs),

each twice: with GBASD_FF_FAT_STRETCH_FIX 0 every function must be identical
(the copies are the same code); with the fix on (the default) the functions
that differ are listed (create_chain_ex / dir_next and what the compiler
inlines them into or lays out differently around them); the behaviour of
the fix is tested by tests/fatfs/run.sh, not here.
The text of ffunicode.c, ffsystem.c, ff.h (switches off), diskio.h and
diskio.c is compared too (line endings ignored).

Needs the app checkouts (GBASD_APPS, default the parent folder of gbaSD) and
arm-none-eabi-gcc (DEVKITARM or PATH). A local check: prints SKIP without
them.   python3 tests/fatfs/equivalence.py"""
from pathlib import Path
import os, re, shutil, subprocess, sys, tempfile

R = Path(__file__).resolve().parents[2]
F = R / 'fatfs'
APPS = Path(os.environ.get('GBASD_APPS', R.parent))
# (name, ff.c folder, ffconf.h / ff.h folder, diskio.c folder or None)
BUTANO_APPS = [('gbaplanner', 'gbaplanner/src/fatfs', 'gbaplanner/include/fatfs', 'gbaplanner/src/fatfs'),
               ('gbawriter', 'gbawriter/src', 'gbawriter/include', 'gbawriter/src'),
               ('gbavocab', 'gbavocab-dev/src', 'gbavocab-dev/include', 'gbavocab-dev/src'),
               ('gbareader', 'gbareader-dev/src', 'gbareader-dev/include', 'gbareader-dev/src')]
GBAMP3 = ('gbamp3', 'gbamp3-dev/player/vendor/fatfs', 'gbamp3-dev/player/vendor/fatfs', None)
SWITCHES_ON = {'GBASD_FF_FAST_EXFAT_LOOKUP': 1, 'GBASD_FF_FSIZE32': 1, 'GBASD_FF_OPEN_ENTRY': 1}
ARM = os.environ.get('DEVKITARM', '/opt/devkitpro/devkitARM')
ARM_GCC = next((c for c in (Path(ARM) / 'bin/arm-none-eabi-gcc', shutil.which('arm-none-eabi-gcc') or '') if c and Path(c).exists()), None)
BUTANO_FLAGS = ['-O2', '-mcpu=arm7tdmi', '-mtune=arm7tdmi', '-ffast-math', '-ffunction-sections', '-fdata-sections',
                '-mthumb', '-mthumb-interwork']
GBAMP3_FLAGS = ['-O3', '-std=c11', '-mcpu=arm7tdmi', '-marm', '-mthumb-interwork', '-mlong-calls', '-ffreestanding',
                '-fno-builtin', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-fwrapv',
                '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections']
HOST_FLAGS = ['-O2', '-ffunction-sections', '-fdata-sections']


def text(p):
    return Path(p).read_bytes().replace(b'\r\n', b'\n')


def compile_(cc, flags, src, incs, out, defines=()):
    cmd = [str(cc), *flags, *[f'-D{k}={v}' for k, v in defines], *[f'-I{i}' for i in incs], '-c', str(src), '-o', str(out)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        sys.exit(f'compile failed: {" ".join(cmd)}\n{p.stderr}')


def functions(objdump, obj):
    """{section: disassembly with relocations} plus the data sections' bytes."""
    out = subprocess.run([objdump, '-dr', '-z', str(obj)], capture_output=True, text=True, check=True).stdout
    secs, cur = {}, None
    for line in out.splitlines()[3:]:
        m = re.match(r'Disassembly of section (\S+):', line)
        if m:
            cur = m.group(1); secs[cur] = []; continue
        if cur:
            secs[cur].append(line)
    data = subprocess.run([objdump, '-s', str(obj)], capture_output=True, text=True, check=True).stdout
    for block in re.split(r'\nContents of section ', data)[1:]:
        name, _, body = block.partition(':\n')
        if not name.startswith(('.text', '.rel', '.note', '.comment', '.ARM.attributes')):
            secs['data ' + name] = body.splitlines()
    return {k: '\n'.join(v) for k, v in secs.items()}


def fname(sec):
    return sec.split('.text.', 1)[1] if '.text.' in sec else sec


def compare(label, a, b, fix):
    fa, fb = functions(OBJDUMP[a[1]], a[0]), functions(OBJDUMP[a[1]], b)
    diff = sorted(fname(k) for k in set(fa) | set(fb) if fa.get(k) != fb.get(k))
    ok = 'create_chain_ex' in diff if fix else not diff
    print(f'  {"OK  " if ok else "FAIL"} {label}: {len(fa)} sections, '
          + ('identical' if not diff else f'differ: {", ".join(diff)}'))
    return ok


def main():
    if not ARM_GCC or not (APPS / 'gbaplanner').is_dir():
        print('SKIP: FatFs equivalence check needs the app checkouts (GBASD_APPS) and arm-none-eabi-gcc')
        return 0
    global OBJDUMP
    OBJDUMP = {'arm': str(Path(ARM_GCC).with_name('arm-none-eabi-objdump')), 'host': 'objdump'}
    work = Path(tempfile.mkdtemp(prefix='gbasd-ffeq-'))
    good = True
    try:
        print('Text of the unchanged files:')
        for name, src, inc, dio in BUTANO_APPS + [GBAMP3]:
            for f, where in (('ffunicode.c', src), ('ffsystem.c', src), ('diskio.h', inc), ('diskio.c', dio),
                             ('ff.h', inc)):
                p = APPS / where / f if where else None
                if not p or not p.exists() or (name == 'gbamp3' and f == 'ff.h'):
                    continue
                ref = text(F / f)
                if f == 'ff.h':   # gbaSD's ff.h with the switch blocks taken out
                    s = ref.decode()
                    s = re.sub(r'\n/\* gbaSD: switches.*?(?=\n\n/\* Integer types)', '', s, flags=re.S)
                    s = re.sub(r'#if GBASD_FF_FSIZE32\n.*?#else\n(.*?)#endif\n', r'\1', s, flags=re.S)
                    s = re.sub(r'#if GBASD_FF_OPEN_ENTRY\n.*?#endif\n', '', s, flags=re.S)
                    ref = s.encode()
                same = text(p) == ref
                good &= same
                print(f'  {"OK  " if same else "FAIL"} {name} {f}')
        for comp, flags_b, flags_m in (('arm', BUTANO_FLAGS, GBAMP3_FLAGS), ('host', HOST_FLAGS, HOST_FLAGS)):
            cc = ARM_GCC if comp == 'arm' else 'gcc'
            print(f'Compiled ff.c ({comp} compiler, ' + ('Butano / gbamp3 flags' if comp == 'arm' else '-O2') + '):')
            for (name, src, inc, _), flags, switches in [(a, flags_b, {}) for a in BUTANO_APPS] + [(GBAMP3, flags_m, SWITCHES_ON)]:
                inc = APPS / inc
                conf = work / name
                conf.mkdir(exist_ok=True)
                c = text(inc / 'ffconf.h').decode()
                if switches:  # as an app sets them: in its ffconf.h
                    c += '\n' + ''.join(f'#define {k}\t{v}\n' for k, v in switches.items())
                (conf / 'ffconf.h').write_text(c)
                ref = work / f'{name}-{comp}-app.o'
                compile_(cc, flags, APPS / src / 'ff.c', [inc], ref)
                for fix in (0, 1):
                    o = work / f'{name}-{comp}-gbasd-{fix}.o'
                    compile_(cc, flags, F / 'ff.c', [F, conf], o, [('GBASD_FF_FAT_STRETCH_FIX', fix)])
                    what = ('switches on' if switches else 'switches off') + (', FAT stretch fix ' + ('on' if fix else 'off'))
                    good &= compare(f'{name} vs gbaSD ({what})', (ref, comp), o, fix)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('FatFs equivalence ' + ('PASS' if good else 'FAIL'))
    return 0 if good else 1


if __name__ == '__main__':
    sys.exit(main())
