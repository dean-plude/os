#!/usr/bin/env python3
"""Write userland/programs/cabtest_data.h: the cabinets cabtest.exe
extracts through cabinet.dll's FDI.

Two cabinets hold the same two files (a short text and an 80 KB file that
spans three 32 KB data blocks, with x86 CALL bytes in it): one compressed
with MSZIP (zlib's raw deflate, each block given the previous 32 KB as its
dictionary, so back references cross blocks) and one with LZX (window 2^16,
one verbatim block per frame, the E8 call translation on).  The LZX
encoder below is just enough of one for test data: greedy matches, the
repeated-offset slot, the delta-coded code lengths with zero runs.  Check
the output with an independent extractor (`cabextract -t FILE.cab`, or
`7z t`) after changing it:

    python3 tools/make_cabtest_data.py
    python3 tools/make_cabtest_data.py --write-cabs DIR
"""
import heapq, os, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), 'userland', 'programs', 'cabtest_data.h')
FRAME = 32768
INTEL_FILESIZE = 12000000


def test_files():
    words = [b'cabinet', b'folder', b'block', b'frame', b'window', b'match', b'literal',
             b'NovaOS', b'installer', b'Burn', b'MSZIP', b'LZX', b'tree', b'offset']
    seed = 12345
    def rnd():
        nonlocal seed
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        return seed >> 8
    small = b'Hello from a cabinet.\r\nThis file is small.\r\n'
    big = bytearray()
    while len(big) < 80000:
        r = rnd()
        if r % 61 == 0:
            # an x86 CALL: E8 and a 32-bit relative target, near or far
            target = (rnd() % 4000) - 2000 if r & 1 else rnd()
            big += b'\xe8' + struct.pack('<i', target)
        else:
            big += words[r % len(words)] + (b'\r\n' if r % 9 == 0 else b' ')
    return [('hello.txt', small), ('data\\big.bin', bytes(big[:80000]))]


def cabinet(entries, method, blocks, prev=None, nxt=None, icabinet=0):
    """a single-folder cabinet: method 1 MSZIP or 3|(16<<8) LZX; @entries =
    [(name, size, folder offset, iFolder)], @blocks = [(compressed,
    uncompressed size)]; @prev and @nxt: (cabinet, disk) of a set"""
    names = [n.encode() + b'\0' for n, _, _, _ in entries]
    flags = (1 if prev else 0) | (2 if nxt else 0)
    links = b''
    for link in (prev, nxt):
        if link:
            links += link[0].encode() + b'\0' + link[1].encode() + b'\0'
    coff_files = 36 + len(links) + 8
    coff_data = coff_files + sum(16 + len(n) for n in names)
    total = coff_data + sum(8 + len(c) for c, _ in blocks)
    out = bytearray(b'MSCF' + struct.pack('<IIIIIBBHHHHH', 0, total, 0, coff_files, 0, 3, 1, 1, len(entries),
                                          flags, 0x4E56, icabinet))
    out += links + struct.pack('<IHH', coff_data, len(blocks), method)
    for (n, size, off, ifold), raw in zip(entries, names):
        # 2025-06-11 05:18:52, archive bit
        out += struct.pack('<IIHHHH', size, off, ifold, (45 << 9) | (6 << 5) | 11, (5 << 11) | (18 << 5) | 26, 0x20) + raw
    for c, u in blocks:
        out += struct.pack('<IHH', 0, len(c), u) + c
    assert len(out) == total
    return bytes(out)


def entries(files):
    out, off = [], 0
    for n, d in files:
        out.append((n, len(d), off, 0))
        off += len(d)
    return out


def mszip(stream):
    blocks = []
    for i in range(0, len(stream), FRAME):
        z = zlib.compressobj(9, zlib.DEFLATED, -15, zdict=stream[max(0, i - FRAME):i]) if i else zlib.compressobj(9, zlib.DEFLATED, -15)
        frame = stream[i:i + FRAME]
        blocks.append((b'CK' + z.compress(frame) + z.flush(), len(frame)))
    return blocks


# ---------------------------------------------------------------------------
# LZX
# ---------------------------------------------------------------------------
WINDOW_BITS = 16
SLOTS = 32
MAIN = 256 + SLOTS * 8
POSITION_BASE = [0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024,
                 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152, 65536]
EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14]


class Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, value, count):
        for i in range(count - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> i) & 1)
            self.n += 1
            if self.n == 16:
                self.out += struct.pack('<H', self.acc)
                self.acc = 0
                self.n = 0

    def align(self):
        if self.n:
            self.put(0, 16 - self.n)


def huff_lengths(freq, limit):
    freq = list(freq)
    used = [i for i, f in enumerate(freq) if f]
    if len(used) < 2:                         # a decodable tree needs two codes
        for i in range(len(freq)):
            if i not in used:
                used.append(i)
                freq[i] = 1
            if len(used) == 2:
                break
    while True:
        heap = [(freq[i], i, (i,)) for i in used]
        heapq.heapify(heap)
        lens = [0] * len(freq)
        tick = len(freq)
        while len(heap) > 1:
            f1, _, a = heapq.heappop(heap)
            f2, _, b = heapq.heappop(heap)
            for s in a + b:
                lens[s] += 1
            heapq.heappush(heap, (f1 + f2, tick, a + b))
            tick += 1
        if max(lens) <= limit:
            return lens
        freq = [(f + 1) // 2 if f else 0 for f in freq]


def huff_codes(lens):
    codes, code = [0] * len(lens), 0
    for length in range(1, 17):
        for s, l in enumerate(lens):
            if l == length:
                codes[s] = code
                code += 1
        code <<= 1
    return codes


def write_lengths(bits, prev, new):
    """one pretree and the delta-coded lengths of a run of tree elements"""
    syms = []
    i = 0
    while i < len(new):
        if new[i] == 0:
            run = 0
            while i + run < len(new) and new[i + run] == 0 and run < 51:
                run += 1
            if run >= 20:
                syms.append((18, run - 20, 5)); i += run; continue
            if run >= 4:
                syms.append((17, run - 4, 4)); i += run; continue
        syms.append(((prev[i] - new[i]) % 17, 0, 0))
        i += 1
    freq = [0] * 20
    for s, _, _ in syms:
        freq[s] += 1
    plens = huff_lengths(freq, 15)
    pcodes = huff_codes(plens)
    for l in plens:
        bits.put(l, 4)
    for s, extra, n in syms:
        bits.put(pcodes[s], plens[s])
        if n:
            bits.put(extra, n)


def e8_translate(frame, frame_pos):
    """the encoder's side of the call translation (the decoder undoes it)"""
    out = bytearray(frame)
    if len(out) <= 10:
        return bytes(out)
    i = 0
    while i < len(out) - 10:
        if out[i] == 0xE8:
            cur = frame_pos + i
            rel = struct.unpack_from('<i', out, i + 1)[0]
            if -cur <= rel < INTEL_FILESIZE - cur:
                struct.pack_into('<i', out, i + 1, rel + cur)
            elif INTEL_FILESIZE - cur <= rel < INTEL_FILESIZE:
                struct.pack_into('<i', out, i + 1, rel - INTEL_FILESIZE)
            i += 5
        else:
            i += 1
    return bytes(out)


def lzx(stream):
    window = 1 << WINDOW_BITS
    data = b''.join(e8_translate(stream[i:i + FRAME], i) for i in range(0, len(stream), FRAME))
    heads = {}
    R = [1, 1, 1]
    main_prev, len_prev = [0] * MAIN, [0] * 249
    blocks = []
    bits = Bits()
    bits.put(1, 1)                            # E8 translation on, then the file size
    bits.put(INTEL_FILESIZE >> 16, 16)
    bits.put(INTEL_FILESIZE & 0xFFFF, 16)
    for start in range(0, len(data), FRAME):
        end = min(start + FRAME, len(data))
        tokens = []
        i = start
        while i < end:
            best_len, best_off = 0, 0
            if i + 3 <= end:
                for j in reversed(heads.get(data[i:i + 3], [])[-16:]):
                    off = i - j
                    if off >= window - 3:
                        continue
                    l = 0
                    while l < 257 and i + l < end and data[j + l] == data[i + l]:
                        l += 1
                    if l > best_len:
                        best_len, best_off = l, off
            if best_len >= 3:
                tokens.append(('m', best_len, best_off))
                step = best_len
            else:
                tokens.append(('l', data[i]))
                step = 1
            for k in range(i, i + step):
                if k + 3 <= len(data):
                    heads.setdefault(data[k:k + 3], []).append(k)
            i += step
        # symbols, with the repeated offsets
        syms = []
        for t in tokens:
            if t[0] == 'l':
                syms.append((t[1], None, None))
                continue
            _, length, off = t
            if off == R[0]:
                slot, extra = 0, None
            elif off == R[1]:
                slot, extra = 1, None
                R[0], R[1] = R[1], R[0]
            elif off == R[2]:
                slot, extra = 2, None
                R[0], R[2] = R[2], R[0]
            else:
                v = off + 2
                slot = max(s for s in range(3, SLOTS) if POSITION_BASE[s] <= v)
                extra = (v - POSITION_BASE[slot], EXTRA[slot])
                R[2], R[1], R[0] = R[1], R[0], off
            header = min(length - 2, 7)
            footer = length - 2 - 7 if header == 7 else None
            syms.append((256 + slot * 8 + header, footer, extra))
        mfreq, lfreq = [0] * MAIN, [0] * 249
        for s, footer, _ in syms:
            mfreq[s] += 1
            if footer is not None:
                lfreq[footer] += 1
        mlens = huff_lengths(mfreq, 16)
        llens = huff_lengths(lfreq, 16) if any(lfreq) else [0] * 249
        mcodes, lcodes = huff_codes(mlens), huff_codes(llens)
        bits.put(1, 3)                        # verbatim block of one frame
        bits.put((end - start) >> 8, 16)
        bits.put((end - start) & 0xFF, 8)
        write_lengths(bits, main_prev[:256], mlens[:256])
        write_lengths(bits, main_prev[256:], mlens[256:])
        write_lengths(bits, len_prev, llens)
        main_prev, len_prev = mlens, llens
        for s, footer, extra in syms:
            bits.put(mcodes[s], mlens[s])
            if footer is not None:
                bits.put(lcodes[footer], llens[footer])
            if extra and extra[1]:
                bits.put(extra[0], extra[1])
        bits.align()
        blocks.append((bytes(bits.out), end - start))
        bits.out = bytearray()
    return blocks


def main():
    files = test_files()
    stream = b''.join(d for _, d in files)
    cabs = [('mszip', cabinet(entries(files), 1, mszip(stream))),
            ('lzx', cabinet(entries(files), 3 | (WINDOW_BITS << 8), lzx(stream)))]
    # a set of two: big.bin's folder goes on into the second cabinet, which
    # also holds a file after it in that folder
    tail = ('tail.txt', b'The last file of the set.\r\n')
    span_files = files + [tail]
    e = entries(span_files)
    blocks = mszip(b''.join(d for _, d in span_files))
    # as makecab does, the data block at the boundary is split: its first
    # bytes end the first cabinet (uncompressed size 0), the rest begin the
    # second
    cut = len(blocks[2][0]) // 2
    cabs.append(('span1', cabinet([e[0], e[1][:3] + (0xFFFE,)], 1, blocks[:2] + [(blocks[2][0][:cut], 0)],
                                  nxt=('span2.cab', 'Disk 2'))))
    cabs.append(('span2', cabinet([e[1][:3] + (0xFFFD,), e[2]], 1, [(blocks[2][0][cut:], blocks[2][1])] + blocks[3:],
                                  prev=('span1.cab', 'Disk 1'), icabinet=1)))
    files = span_files
    if len(sys.argv) > 2 and sys.argv[1] == '--write-cabs':
        os.makedirs(sys.argv[2], exist_ok=True)
        for name, cab in cabs:
            open(os.path.join(sys.argv[2], name + '.cab'), 'wb').write(cab)
            for n, d in files:
                p = os.path.join(sys.argv[2], 'expected', n.replace('\\', '/'))
                os.makedirs(os.path.dirname(p), exist_ok=True)
                open(p, 'wb').write(d)
        return
    text = '/* generated by tools/make_cabtest_data.py: do not edit */\n'
    text += 'static const struct { const char *name; unsigned size; unsigned crc; } cab_expect[] = {\n'
    for n, d in files:
        text += '    { "%s", %d, 0x%08X },\n' % (n.replace('\\', '\\\\'), len(d), zlib.crc32(d))
    text += '};\n'
    for name, cab in cabs:
        text += 'static const unsigned char cab_%s[%d] = {\n' % (name, len(cab))
        for i in range(0, len(cab), 20):
            text += '    ' + ''.join('%d,' % b for b in cab[i:i + 20]) + '\n'
        text += '};\n'
    open(OUT, 'w').write(text)


if __name__ == '__main__':
    main()
