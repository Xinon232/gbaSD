#!/usr/bin/env python3
"""Exact-ROM tests of the SuperCard read CRC16 check on mGBA.

Builds tools/crc_diag (count-only and check ROMs) and scsd_runner, makes a
FAT32 card image with files, and runs one pass of each ROM on the SuperCard
model (tests/models/scsd_model.h) with the model's fault injection:
clean reads, one flipped data bit (transient and permanent), one wrong CRC,
and the old constant CRC (safety switch). Checks the driver's counters and
that the data read matches the card image. Not hardware proof.
Needs devkitARM + libgba, libmgba-dev, mtools, dosfstools.
Usage: tests/emu/crc_diag_test.py
"""
import os, random, re, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIAG = ROOT / 'tools' / 'crc_diag'
FIRST_SECTORS = 64


def fail(name, what):
    sys.exit(f'FAIL {name}: {what}')


def symbol(elf, name):
    nm = os.environ.get('DEVKITARM', '/opt/devkitpro/devkitARM') + '/bin/arm-none-eabi-nm'
    for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
        p = line.split()
        if p[-1] == name:
            return int(p[0], 16)
    fail('setup', f'no symbol {name}')


def make_card(work):
    img = work / 'card.img'
    subprocess.run(['truncate', '-s', '96M', str(img)], check=True)
    subprocess.run(['mkfs.vfat', '-F', '32', '-s', '8', str(img)], check=True, capture_output=True)
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
    rnd = random.Random(7)
    for i in range(6):   # 6 MiB of files at the start of the data area
        f = work / f'file{i}.bin'
        f.write_bytes(bytes(rnd.getrandbits(8) for _ in range(1 << 20)))
        subprocess.run(['mcopy', '-i', str(img), str(f), f'::/file{i}.bin'], check=True, env=env)
    return img


def expected(img, vol, data, total):
    raw = img.read_bytes()
    a, b, nonzero = 1, 0, 0
    for start, count in ((vol, FIRST_SECTORS), (data, total)):
        chunk = raw[start * 512:(start + count) * 512]
        for i in range(0, len(chunk), 512):
            nonzero += any(chunk[i:i + 512])
        for x in chunk:
            a = (a + x) & 0xFFFFFFFF
            b = (b + a) & 0xFFFFFFFF
    return b ^ ((a << 16) & 0xFFFFFFFF), nonzero


def run(runner, rom, img, env_extra, d_addr, s_addr):
    env = dict(os.environ, SD_IMAGE=str(img), SDHC='1')
    env.update(env_extra)
    # until diag.pass (word 3) reaches 1
    p = subprocess.run([str(runner), str(rom), '6000', hex(d_addr + 12), '1', f'{d_addr:#x}:15', f'{s_addr:#x}:6'],
                       env=env, capture_output=True, text=True)
    if p.returncode:
        fail(rom.name, f'runner exit {p.returncode}\n{p.stderr[-1500:]}')
    words = [list(map(int, l.split()[2:])) for l in p.stdout.splitlines() if l.startswith('WORDS')]
    keys = ['magic', 'init_error', 'cart', 'pass', 'sectors', 'nonzero', 'read_errors', 'checksum1',
            'volume_start', 'data_start', 'data_sectors', 'first_bad_lba', 'same_data', 'other_data', 'kib_per_s']
    d = dict(zip(keys, words[0]))
    s = dict(zip(['crc_sectors', 'mismatches', 'retries', 'failures', 'suspects'], words[1]))
    s.update(mode=words[1][5] & 255, trusted=(words[1][5] >> 8) & 255, disabled=(words[1][5] >> 16) & 255)
    rep = dict(re.findall(r'(\w+)=(\d+)', re.search(r'SCSD (.*)', p.stdout).group(1)))
    if d['pass'] < 1:
        fail(rom.name, f'pass not finished {d} {s}')
    return d, s, rep


def main():
    subprocess.run(['make', '-C', str(DIAG), '-s'], check=True)
    count_rom, check_rom = DIAG / 'crc_diag.gba', DIAG / 'crc_diag_check.gba'
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        runner = work / 'scsd_runner'
        subprocess.run(['cc', '-O2', str(ROOT / 'tests' / 'emu' / 'scsd_runner.c'), '-lmgba', '-o', str(runner)], check=True)
        img = make_card(work)
        ref = None
        results = []
        for rom, cases in [
            (count_rom, [('clean', {}), ('flip-once', {'SD_CRC_FLIP': '300'}), ('bad-crc', {'SD_CRC_BAD': '300'}),
                         ('const-crc', {'SD_CRC_CONST': '1'})]),
            (check_rom, [('clean', {}), ('flip-once', {'SD_CRC_FLIP': '300'}), ('flip-unaligned', {'SD_CRC_FLIP': '200'}),
                         ('bad-crc', {'SD_CRC_BAD': '300'}),
                         ('flip-permanent', {'SD_CRC_FLIP': '300', 'SD_CRC_STICKY': '1'}),
                         ('flip-early', {'SD_CRC_FLIP': '2', 'SD_CRC_STICKY': '1'}),
                         ('const-crc', {'SD_CRC_CONST': '1'})])]:
            elf = rom.with_suffix('.elf')
            d_addr, s_addr = symbol(elf, 'diag'), symbol(elf, 'gbasd_sc_crc_stats')
            for name, env in cases:
                tag = f'{rom.stem} {name}'
                d, s, rep = run(runner, rom, img, env, d_addr, s_addr)
                if d['magic'] != 0x44435243 or d['init_error'] or d['cart'] != 1:
                    fail(tag, f'start {d}')
                if ref is None:
                    ref = expected(img, d['volume_start'], d['data_start'], d['data_sectors'])
                good = d['checksum1'] == ref[0]
                faults = int(rep['crc_faults'])
                ok = {
                    ('crc_diag', 'clean'): good and s['mismatches'] == 0 and d['nonzero'] == ref[1] + 1,   # + sector 0 read at start
                    ('crc_diag', 'flip-once'): s['mismatches'] == 1 and d['other_data'] == 1 and not s['retries'],
                    ('crc_diag', 'bad-crc'): s['mismatches'] == 1 and d['same_data'] == 1,
                    # (the run stops at a frame end, maybe inside a read: the asm has counted
                    # its mismatches, the C code not yet its sectors)
                    ('crc_diag', 'const-crc'): s['crc_sectors'] <= s['mismatches'] <= s['crc_sectors'] + 32
                    and d['same_data'] > 0 and not d['other_data'] and not d['read_errors'],
                    ('crc_diag_check', 'clean'): good and s['mismatches'] == 0 and not s['retries'] and s['trusted'],
                    ('crc_diag_check', 'flip-once'): good and s['retries'] == 1 and s['mismatches'] == 1 and not d['read_errors'],
                    ('crc_diag_check', 'flip-unaligned'): good and s['retries'] == 1 and not d['read_errors'],
                    ('crc_diag_check', 'bad-crc'): good and s['retries'] == 1 and not d['read_errors'],
                    ('crc_diag_check', 'flip-permanent'): d['read_errors'] >= 1 and s['failures'] >= 1
                    and s['retries'] == 2 * s['failures'] and not s['disabled'] and not good,
                    # sector 0 stays bad: kept in pass 1 (not trusted yet); the run may
                    # stop just after pass 2 began with it, trusted by then: a read error
                    ('crc_diag_check', 'flip-early'): s['suspects'] == 1 and not s['disabled']
                    and d['read_errors'] == s['failures'] <= 1,
                    ('crc_diag_check', 'const-crc'): good and s['disabled'] and s['suspects'] == 3
                    and not d['read_errors'] and not s['trusted'],
                }[(rom.stem, name)]
                if not ok:
                    fail(tag, f'diag {d} crc {s} model {rep} expected checksum {ref[0]}')
                results.append(f'PASS {tag}: {d["sectors"]} sectors ({d["nonzero"]} non-zero), '
                               f'{s["mismatches"]} mismatches, {s["retries"]} retries, {s["failures"]} failed, '
                               f'{s["suspects"]} suspect, disabled {s["disabled"]}, {d["read_errors"]} read errors, '
                               f'{faults} faults injected, data {"= image" if good else "!= image"}')
                print(results[-1], flush=True)
    print('CRC DIAG MODEL TESTS PASS (modeled cart, not hardware proof)')


if __name__ == '__main__':
    main()
