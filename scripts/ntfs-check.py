#!/usr/bin/env python3
"""Offline NTFS consistency check (a chkdsk-style subset): scripts/ntfs-check.py image [offset_bytes]
Checks: record fixups/headers, MFT bitmap vs in-use records, every cluster
owned once and $Bitmap matching ownership, link counts, $I30 indexes (sorted
by $UpCase collation, same leaf depth, VCNs, index bitmap, entries <-> $FILE_NAME
attributes, sequence numbers), $LogFile clean."""
import struct, sys

errs = []
def err(m):
    errs.append(m)
    if len(errs) <= 60: print('ERROR:', m)

f = open(sys.argv[1], 'rb'); base = int(sys.argv[2]) if len(sys.argv) > 2 else 0
def rd(off, n): f.seek(base + off); return f.read(n)
bs = rd(0, 512)
assert bs[3:11] == b'NTFS    '
bps, spc = struct.unpack_from('<HB', bs, 11)
cl = bps * spc
total_sectors, mft_lcn = struct.unpack_from('<QQ', bs, 0x28)
cpr = struct.unpack_from('<b', bs, 0x40)[0]
rec_size = cl * cpr if cpr > 0 else 1 << -cpr
cpi = struct.unpack_from('<b', bs, 0x44)[0]
idx_size = cl * cpi if cpi > 0 else 1 << -cpi
nclusters = total_sectors * bps // cl

def fixup(b, magic):
    b = bytearray(b)
    if b[:4] != magic: return None
    uo, uc = struct.unpack_from('<HH', b, 4)
    usn = b[uo:uo+2]
    for i in range(1, uc):
        e = i * 512 - 2
        if b[e:e+2] != usn: return None
        b[e:e+2] = b[uo+2*i:uo+2*i+2]
    return bytes(b)

def runs(a):
    off = struct.unpack_from('<H', a, 0x20)[0]
    p, vcn, lcn, out = off, struct.unpack_from('<Q', a, 0x10)[0], 0, []
    while p < len(a) and a[p]:
        h = a[p]; nl, no = h & 15, h >> 4; p += 1
        ln = int.from_bytes(a[p:p+nl], 'little'); p += nl
        if no:
            d = int.from_bytes(a[p:p+no], 'little', signed=True); p += no; lcn += d
            out.append((vcn, lcn, ln))
        else:
            out.append((vcn, None, ln))
        vcn += ln
    return out

def attrs(rec):
    o = struct.unpack_from('<H', rec, 0x14)[0]
    used = struct.unpack_from('<I', rec, 0x18)[0]
    out = []
    while o + 8 <= len(rec):
        t, ln = struct.unpack_from('<II', rec, o)
        if t == 0xFFFFFFFF:
            if o + 8 != used and o + 4 != used - 4: err(f'used size {used} but end at {o}')
            break
        if ln < 0x18 or o + ln > used: return None
        a = rec[o:o+ln]
        nl, no = a[9], struct.unpack_from('<H', a, 0xA)[0]
        name = a[no:no+2*nl].decode('utf-16le', 'replace')
        out.append((t, name, a))
        o += ln
    return out

def value(a):
    if a[8] == 0:
        vl, vo = struct.unpack_from('<IH', a, 0x10)
        return a[vo:vo+vl]
    size = struct.unpack_from('<Q', a, 0x30)[0]
    data = bytearray()
    for vcn, lcn, ln in runs(a):
        data += b'\0' * (ln * cl) if lcn is None else rd(lcn * cl, ln * cl)
    return bytes(data[:size])

rec0 = fixup(rd(mft_lcn * cl, rec_size), b'FILE')
mft_attr = [a for t, n, a in attrs(rec0) if t == 0x80 and n == ''][0]
mft = value(mft_attr)
nrec = len(mft) // rec_size
def record(i): return fixup(mft[i*rec_size:(i+1)*rec_size], b'FILE')
mftbm = value([a for t, n, a in attrs(rec0) if t == 0xB0][0])
upcase = value([a for t, n, a in attrs(record(10)) if t == 0x80][0])
up = struct.unpack(f'<{len(upcase)//2}H', upcase)
cbm = value([a for t, n, a in attrs(record(6)) if t == 0x80][0])

def bit(m, i): return (m[i >> 3] >> (i & 7)) & 1

owner = {}
recs = {}
for i in range(nrec):
    raw = mft[i*rec_size:(i+1)*rec_size]
    if raw[:4] != b'FILE':
        if bit(mftbm, i): err(f'record {i} in bitmap but not a FILE record')
        continue
    r = fixup(raw, b'FILE')
    if r is None: err(f'record {i}: bad fixup'); continue
    flags = struct.unpack_from('<H', r, 0x16)[0]
    inuse = flags & 1
    if inuse != bit(mftbm, i): err(f'record {i}: in use {inuse} but MFT bitmap {bit(mftbm, i)}')
    if not inuse: continue
    if struct.unpack_from('<I', r, 0x2C)[0] != i and i >= 16: err(f'record {i}: wrong record number field')
    if struct.unpack_from('<I', r, 0x1C)[0] != rec_size: err(f'record {i}: allocated size field')
    at = attrs(r)
    if at is None: err(f'record {i}: attribute list corrupt'); continue
    prev = 0
    insts = set()
    for t, n, a in at:
        if t < prev: err(f'record {i}: attributes out of order')
        prev = t
        inst = struct.unpack_from('<H', a, 0xE)[0]
        if inst in insts: err(f'record {i}: duplicate attribute instance {inst}')
        insts.add(inst)
        if inst >= struct.unpack_from('<H', r, 0x28)[0]: err(f'record {i}: instance {inst} >= next instance')
        if a[8]:
            alloc, size, init = struct.unpack_from('<QQQ', a, 0x28)
            last = struct.unpack_from('<q', a, 0x18)[0]
            rl = runs(a)
            nv = sum(ln for _, _, ln in rl)
            if nv * cl != alloc: err(f'record {i} attr {t:x}: runs cover {nv*cl}, allocated {alloc}')
            if last != nv - 1: err(f'record {i} attr {t:x}: last VCN {last} vs {nv-1}')
            if size > alloc or init > size: err(f'record {i} attr {t:x}: sizes {alloc}/{size}/{init}')
            if i == 8 and n == '$Bad': continue
            for vcn, lcn, ln in rl:
                if lcn is None: continue
                for c in range(lcn, lcn + ln):
                    if c >= nclusters: err(f'record {i}: cluster {c} beyond volume'); break
                    if c in owner: err(f'cluster {c} owned by records {owner[c]} and {i}'); break
                    owner[c] = i
    base_ref = struct.unpack_from('<Q', r, 0x20)[0]
    fns = [value(a) for t, n, a in at if t == 0x30]
    if base_ref == 0 and struct.unpack_from('<H', r, 0x12)[0] != len(fns):
        err(f'record {i}: link count {struct.unpack_from("<H", r, 0x12)[0]} vs {len(fns)} names')
    if bool(flags & 2) != any(t == 0x90 and n == '$I30' for t, n, a in at) and i != 0:
        err(f'record {i}: directory flag vs $I30')
    recs[i] = (r, at, fns)

# boot sector clusters belong to $Boot (record 7) via its run already; check bitmap
leak = miss = 0
for c in range(nclusters):
    b = bit(cbm, c)
    if b and c not in owner: leak += 1
    if not b and c in owner:
        miss += 1
        if miss < 5: err(f'cluster {c} owned by {owner[c]} but free in $Bitmap')
if miss: err(f'{miss} owned clusters free in $Bitmap')
if leak: err(f'{leak} clusters marked used but owned by no file (lost clusters)')

def key(fn):
    nl = fn[0x40]; return fn[0x42:0x42+2*nl]
def collate(a, b):
    ka, kb = key(a), key(b)
    ua = [up[x] if x < len(up) else x for x in struct.unpack(f'<{len(ka)//2}H', ka)]
    ub = [up[x] if x < len(up) else x for x in struct.unpack(f'<{len(kb)//2}H', kb)]
    if ua != ub: return -1 if ua < ub else 1
    if ka != kb:
        la = list(struct.unpack(f'<{len(ka)//2}H', ka)); lb = list(struct.unpack(f'<{len(kb)//2}H', kb))
        return -1 if la < lb else 1
    return 0

indexed = set()
for d, (r, at, fns) in recs.items():
    roots = [a for t, n, a in at if t == 0x90 and n == '$I30']
    if not roots: continue
    rv = value(roots[0])
    bsize = struct.unpack_from('<I', rv, 8)[0]
    vunit = cl if bsize >= cl else 512
    allocs = [a for t, n, a in at if t == 0xA0 and n == '$I30']
    bms = [a for t, n, a in at if t == 0xB0 and n == '$I30']
    alloc = value(allocs[0]) if allocs else b''
    ibm = value(bms[0]) if bms else b''
    if bool(allocs) != bool(bms): err(f'dir {d}: allocation/bitmap mismatch')
    used_blocks = set()
    entries = []
    depths = set()
    def node(hdr, depth, where):
        first, used = struct.unpack_from('<II', hdr, 0)
        o = first
        while True:
            if o + 0x10 > used: err(f'dir {d} {where}: no END entry'); return
            e = hdr[o:]
            ref, elen, klen, fl = struct.unpack_from('<QHHH', e, 0)
            if fl & 1:
                vcn = struct.unpack_from('<Q', e, elen - 8)[0]
                off = vcn * vunit
                if off % bsize or off + bsize > len(alloc): err(f'dir {d}: bad subnode VCN {vcn}');
                else:
                    if not bit(ibm, off // bsize): err(f'dir {d}: block {off//bsize} used but not in index bitmap')
                    if off // bsize in used_blocks: err(f'dir {d}: block used twice')
                    used_blocks.add(off // bsize)
                    blk = fixup(alloc[off:off+bsize], b'INDX')
                    if blk is None: err(f'dir {d}: block {off//bsize} bad fixup')
                    else:
                        if struct.unpack_from('<Q', blk, 0x10)[0] != vcn: err(f'dir {d}: block VCN field')
                        h = blk[0x18:]
                        leafflag = h[0xC] & 1
                        node(h, depth + 1, f'vcn {vcn}')
            elif hdr[0xC] & 1:
                err(f'dir {d} {where}: internal node entry without subnode')
            if fl & 2:
                if not (fl & 1): depths.add(depth)
                return
            entries.append((ref, e[0x10:0x10+klen]))
            o += elen
    node(rv[0x10:], 0, 'root')
    if len(depths) > 1: err(f'dir {d}: leaves at depths {depths}')
    for i in range(len(entries) - 1):
        if collate(entries[i][1], entries[i+1][1]) >= 0:
            err(f'dir {d}: entries out of order: {key(entries[i][1]).decode("utf-16le")} / {key(entries[i+1][1]).decode("utf-16le")}')
    for ref, fn in entries:
        m, seq = ref & ((1 << 48) - 1), ref >> 48
        name = key(fn).decode('utf-16le', 'replace')
        if m not in recs: err(f'dir {d}: entry {name} -> record {m} not in use'); continue
        rr = recs[m][0]
        if struct.unpack_from('<H', rr, 0x10)[0] != seq: err(f'dir {d}: entry {name} sequence {seq} vs record')
        if not any(x == fn[:len(x)] or (x[:0x8] == fn[:0x8] and key(x) == key(fn) and x[0x41] == fn[0x41]) for x in recs[m][2]):
            err(f'dir {d}: entry {name} has no matching $FILE_NAME in record {m}')
        if struct.unpack_from('<Q', fn, 0)[0] & ((1 << 48) - 1) != d: err(f'dir {d}: entry {name} parent field')
        fnflags = struct.unpack_from('<I', fn, 0x38)[0]
        isdir = bool(struct.unpack_from('<H', rr, 0x16)[0] & 2)
        if isdir != bool(fnflags & 0x10000000) and m >= 16: err(f'dir {d}: entry {name} directory flag')
        if not isdir and m >= 16:
            da = [a for t, n, a in recs[m][1] if t == 0x80 and n == '']
            if da:
                real = struct.unpack_from('<Q', da[0], 0x30)[0] if da[0][8] else struct.unpack_from('<I', da[0], 0x10)[0]
                if struct.unpack_from('<Q', fn, 0x30)[0] != real: err(f'dir {d}: entry {name} size {struct.unpack_from("<Q", fn, 0x30)[0]} vs data {real} (minor)')
        indexed.add((m, d, name))
for m, (r, at, fns) in recs.items():
    if struct.unpack_from('<Q', r, 0x20)[0]: continue
    for fn in fns:
        p = struct.unpack_from('<Q', fn, 0)[0] & ((1 << 48) - 1)
        name = key(fn).decode('utf-16le', 'replace')
        if (m, p, name) not in indexed and m != 5: err(f'record {m}: name {name} not in directory {p} index')

lf = [a for t, n, a in recs[2][1] if t == 0x80][0]
page = rd(runs(lf)[0][1] * cl, 512)
if page[:4] == b'RSTR':
    ra = struct.unpack_from('<H', page, 0x18)[0]
    inuse, fl = struct.unpack_from('<HH', page, ra + 0xC)
    if inuse != 0xFFFF and not fl & 2: err('$LogFile not clean')
elif page != b'\xff' * 512: err('$LogFile unrecognised')
vi = [a for t, n, a in recs[3][1] if t == 0x70]
if vi and struct.unpack_from('<H', value(vi[0]), 10)[0] & 1: err('volume marked dirty')

print(f'{nrec} records, {len(recs)} in use, {len(owner)} clusters owned of {nclusters}; {len(errs)} error(s)')
sys.exit(1 if errs else 0)
