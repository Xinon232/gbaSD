"""Independent exFAT reader for tests (no FatFs code; from gbamp3's
tests/exfat, as copied into gbaplanner, gbawriter, gbavocab and gbareader):
lists folders with each entry's size, first cluster and whether it is
contiguous (NoFatChain) or uses a FAT chain, and reads files. A folder ends
at its data length (as exFAT defines it), not at the end of its FAT chain.
  python3 exfat_read.py IMG [PATH]     prints the tree under PATH
  from exfat_read import Volume        Volume(img).listdir('/a'), .file('/a/b'), .find(...)"""
import struct, sys


class Volume:
    def __init__(self, path):
        self.f = open(path, 'rb')
        b = self.read(0, 512)
        assert b[3:11] == b'EXFAT   ', 'not exFAT'
        self.sector = 1 << b[108]
        self.cluster = self.sector << b[109]
        self.fat = struct.unpack_from('<I', b, 80)[0] * self.sector
        self.heap = struct.unpack_from('<I', b, 88)[0] * self.sector
        self.root = struct.unpack_from('<I', b, 96)[0]

    def read(self, at, n):
        self.f.seek(at)
        return self.f.read(n)

    def chain(self, first, size, contiguous):
        """Clusters of an object; `size` None means follow the FAT to its end."""
        if first == 0:
            return []
        if contiguous:
            return list(range(first, first + -(-size // self.cluster)))
        out, c = [], first
        while 2 <= c < 0xFFFFFFF7:
            out.append(c)
            assert len(out) < 1 << 24, 'FAT loop'
            c = struct.unpack('<I', self.read(self.fat + 4 * c, 4))[0]
        return out

    def data(self, first, size, contiguous):
        raw = b''.join(self.read(self.heap + (c - 2) * self.cluster, self.cluster) for c in self.chain(first, size, contiguous))
        return raw if size is None else raw[:size]

    def entries(self, first=None, size=None, contiguous=False):
        """[(name, is_dir, size, first_cluster, contiguous)] of a folder."""
        raw = self.data(self.root, None, False) if first is None else self.data(first, size, contiguous)
        out, i = [], 0
        while i + 32 <= len(raw):
            t = raw[i]
            if t == 0:
                break
            if t == 0x85:
                count, attr = raw[i + 1], struct.unpack_from('<H', raw, i + 4)[0]
                s = raw[i + 32:i + 64]
                assert s[0] == 0xC0, 'stream extension expected'
                flags, nlen = s[1], s[3]
                first_c, length = struct.unpack_from('<IQ', s, 20)
                name = b''.join(raw[i + 32 * k + 2:i + 32 * k + 32] for k in range(2, count + 1)).decode('utf-16-le')[:nlen]
                out.append((name, bool(attr & 0x10), length, first_c, bool(flags & 2)))
                i += 32 * (count + 1)
                continue
            i += 32
        return out

    def find(self, path):
        """Entry tuple of `path` ('/a/b'); the root is (None, True, None, None, False)."""
        e = ('', True, None, None, False)
        for part in [p for p in path.split('/') if p]:
            kids = self.entries(e[3], e[2], e[4]) if e[3] else self.entries()
            e = next((k for k in kids if k[0].lower() == part.lower()), None)
            if e is None:
                return None
        return e

    def listdir(self, path):
        e = self.find(path)
        return self.entries(e[3], e[2], e[4]) if e[3] else self.entries()

    def file(self, path):
        e = self.find(path)
        return self.data(e[3], e[2], e[4])


def walk(v, path, depth=0):
    for name, d, size, first, cont in v.listdir(path):
        print('  ' * depth + f'{"D" if d else "F"} {size} {"contiguous" if cont else "fat-chain"} {name}')
        if d:
            walk(v, path.rstrip('/') + '/' + name, depth + 1)


if __name__ == '__main__':
    walk(Volume(sys.argv[1]), sys.argv[2] if len(sys.argv) > 2 else '/')
