#!/usr/bin/env python3
"""Builds exfat_tool (the kit's card tool) against a FatFs and an ffconf.h.

  python3 gbasd/tests/exfat_kit/build.py OUT [--ffconf=include/ffconf.h]
          [--fatfs=DIR] [--extra=FILE.c ...] [--no-sanitize]

  OUT        the program to write (then EXFAT_TOOL=OUT for the stand-ins)
  --ffconf   the app's ffconf.h (default: gbaSD's fatfs/ffconf_example.h);
             it must have FF_FS_EXFAT 1. FF_USE_CHMOD is turned on in the
             tool's copy (mattrib needs f_chmod); nothing else is changed.
  --fatfs    the folder with ff.c, ffunicode.c, ff.h, diskio.h (default:
             gbaSD's fatfs/, the FatFs the apps use)
  --extra    more C files to link (e.g. stubs an app's FatFs copy calls)
Built with gcc, AddressSanitizer + UBSan (unless --no-sanitize)."""
from pathlib import Path
import re, subprocess, sys, tempfile

KIT = Path(__file__).resolve().parent
GBASD = KIT.parents[1]


def build(out, ffconf=None, fatfs=None, extra=(), sanitize=True):
    fatfs = Path(fatfs or GBASD / 'fatfs')
    conf = Path(ffconf or GBASD / 'fatfs' / 'ffconf_example.h').read_text()
    if not re.search(r'#define\s+FF_FS_EXFAT\s+1', conf):
        sys.exit(f'exfat_kit: FF_FS_EXFAT is not 1 in {ffconf}')
    conf = re.sub(r'#define(\s+)FF_USE_CHMOD(\s+)0', r'#define\1FF_USE_CHMOD\g<2>1', conf)
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as t:
        (Path(t) / 'ffconf.h').write_text(conf)
        san = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitize else []
        cmd = ['gcc', '-std=gnu11', '-O1', '-g', *san, '-Wall', '-Wextra', '-Werror', f'-I{fatfs}', f'-I{t}',
               str(KIT / 'exfat_tool.c'), str(fatfs / 'ff.c'), str(fatfs / 'ffunicode.c'), *map(str, extra),
               '-o', str(out)]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode:
            sys.exit(f'exfat_kit: build failed: {" ".join(cmd)}\n{p.stdout}{p.stderr}')
    return out


if __name__ == '__main__':
    a = sys.argv[1:]
    pos = [x for x in a if not x.startswith('--')]
    if len(pos) != 1:
        sys.exit(__doc__)
    get = lambda k: next((x.split('=', 1)[1] for x in a if x.startswith(f'--{k}=')), None)  # noqa: E731
    print(build(pos[0], get('ffconf'), get('fatfs'), [x.split('=', 1)[1] for x in a if x.startswith('--extra=')],
                '--no-sanitize' not in a))
