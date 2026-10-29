"""Builds host test programs against gbaSD's FatFs (fatfs/) in the two
switch configurations the tests run:
  off: fatfs/ffconf_example.h as it is (gbaplanner / gbawriter settings,
       every GBASD_FF_* switch off),
  on:  the same with gbamp3's settings (FF_USE_LFN 1, FF_USE_FASTSEEK 1) and
       GBASD_FF_FAST_EXFAT_LOOKUP, GBASD_FF_FSIZE32, GBASD_FF_OPEN_ENTRY 1.
FF_USE_MKFS is turned on in the test builds (some tests format a RAM disk).
A path to an ffconf.h instead of off / on tests that configuration (an
app's include/ffconf.h: --config=PATH for stretch_cut.py, growth_cut.py and
remove_cut.py)."""
from pathlib import Path
import re, subprocess

R = Path(__file__).resolve().parents[2]
F = R / 'fatfs'
CONFIGS = ('off', 'on')
SAN = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']


def sub(s, name, value):
    s, n = re.subn(rf'#define {name}\s+\S+', f'#define {name}\t{value}', s)
    assert n == 1, name
    return s


def make_conf(out, cfg, mkfs=True):
    """out/ffconf.h for configuration cfg; returns out."""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    s = (F / 'ffconf_example.h' if cfg in CONFIGS else Path(cfg)).read_text()
    if mkfs:
        s = sub(s, 'FF_USE_MKFS', 1)
    if cfg == 'on':
        s = sub(s, 'FF_USE_LFN', 1)
        s = sub(s, 'FF_USE_FASTSEEK', 1)
        for k in ('GBASD_FF_FAST_EXFAT_LOOKUP', 'GBASD_FF_FSIZE32', 'GBASD_FF_OPEN_ENTRY'):
            s = sub(s, k, 1)
    (out / 'ffconf.h').write_text(s)
    return out


def build(exe, sources, conf, defines=(), cc='gcc', flags=('-std=gnu11', '-O1', '-g')):
    """Compiles sources + fatfs/ff.c + ffunicode.c with ffconf.h from conf."""
    cmd = [cc, *flags, *SAN, '-Wall', '-Wextra', '-Werror', *[f'-D{d}' for d in defines], f'-I{F}', f'-I{conf}',
           *map(str, sources), str(F / 'ff.c'), str(F / 'ffunicode.c'), '-o', str(exe)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        raise SystemExit(f'build failed: {" ".join(cmd)}\n{p.stdout}{p.stderr}')
    return exe
