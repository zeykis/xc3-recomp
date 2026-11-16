#!/usr/bin/env python3
"""Extract the game's RomFS (assets) straight from your XCI/NSP, decrypting on the fly.

  python tools/extract_romfs.py "Xenoblade Chronicles 3.xci" --keys ..\\keys\\prod.keys --out game\\romfs

- No hactool, no temporary NCA copy: disk needed = size of the assets (~15 GB for XC3).
- Resumable: re-running skips finished files and continues a partially written one.
- --list prints the file list; --only REGEX extracts matching paths only.
Needs: pip install cryptography. Keys must come from your own console (Lockpick_RCM).
"""
import argparse, mmap, os, re, struct, sys, time
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ap = argparse.ArgumentParser()
ap.add_argument('dump'); ap.add_argument('--keys', required=True); ap.add_argument('--out', default='game/romfs')
ap.add_argument('--list', action='store_true'); ap.add_argument('--only', default=None)
a = ap.parse_args()

keys = {}
for line in open(a.keys):
    m = re.match(r'\s*(\w+)\s*=\s*([0-9A-Fa-f]+)', line)
    if m:
        keys[m.group(1).lower()] = bytes.fromhex(m.group(2))

f = open(a.dump, 'rb')
d = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]
u64 = lambda b, o: struct.unpack_from('<Q', b, o)[0]


def cstr(b, o):
    return bytes(b[o:b.find(b'\0', o)]).decode()


def pfs(base, entsz):
    n, st = u32(d, base + 4), u32(d, base + 8); ent = base + 0x10; so = ent + entsz * n; ds = so + st
    return [(cstr(d, so + u32(d, ent + i * entsz + 16)), ds + u64(d, ent + i * entsz), u64(d, ent + i * entsz + 8)) for i in range(n)]


# ---------------------------------------------------------------- locate the Program NCA
if d[:4] == b'PFS0':
    files = pfs(0, 0x18)
else:
    head = 0x100 if d[0x100:0x104] == b'HEAD' else 0x1100
    root = u64(d, head + 0x30) + (0x1000 if head == 0x1100 else 0)
    sec = next(o for n, o, s in pfs(root, 0x40) if n == 'secure')
    files = pfs(sec, 0x40)
ncas = sorted([x for x in files if x[0].endswith('.nca') and not x[0].endswith('.cnmt.nca')], key=lambda x: -x[2])
NCA_NAME, NCA, NCA_SIZE = ncas[0]

# ---------------------------------------------------------------- NCA header (AES-XTS, Nintendo big-endian sector tweak)
def xts_decrypt(data, key, sector0=0, ssize=0x200):
    out = bytearray()
    for i in range(0, len(data), ssize):
        tweak = (sector0 + i // ssize).to_bytes(16, 'big')
        dec = Cipher(algorithms.AES(key), modes.XTS(tweak)).decryptor()
        out += dec.update(bytes(data[i:i + ssize])) + dec.finalize()
    return bytes(out)


hdr = xts_decrypt(d[NCA:NCA + 0xC00], keys['header_key'])
if hdr[0x200:0x204] != b'NCA3':
    sys.exit('NCA header did not decrypt (wrong header_key?)')
if any(hdr[0x230:0x240]):
    sys.exit('This NCA uses title-key crypto (eShop NSP); only cartridge/XCI key-area crypto is supported for now.')
gen = max(hdr[0x206], hdr[0x220]); kidx = gen - 1 if gen > 0 else 0
kak_name = f'key_area_key_application_{kidx:02x}'
if kak_name not in keys:
    sys.exit(f'{kak_name} missing from prod.keys (dump keys again with a newer Lockpick_RCM)')
ecb = Cipher(algorithms.AES(keys[kak_name]), modes.ECB()).decryptor()
key_area = ecb.update(hdr[0x300:0x340]) + ecb.finalize()
ctr_key = key_area[0x20:0x30]

# ---------------------------------------------------------------- RomFS section
sect = None
for i in range(4):
    start, end = u32(hdr, 0x240 + i * 0x10) * 0x200, u32(hdr, 0x244 + i * 0x10) * 0x200
    fsh = hdr[0x400 + i * 0x200:0x600 + i * 0x200]
    if start and fsh[2] == 0 and fsh[4] == 3 and fsh[8:12] == b'IVFC':     # RomFS, AES-CTR
        sect = (start, end, fsh)
        break
if not sect:
    sys.exit('no RomFS section found in the Program NCA')
S_START, S_END, FSH = sect
CTR_HI = bytes(reversed(FSH[0x140:0x148]))
lvl = 0x8 + 0x10 + 5 * 0x18                       # IVFC level 6 = actual RomFS data
DATA_OFF = S_START + u64(FSH, lvl)


def read(off, size):
    """Decrypt [off, off+size) of the NCA (offsets relative to the NCA start)."""
    a0 = off & ~0xf
    ctr = CTR_HI + (a0 >> 4).to_bytes(8, 'big')
    dec = Cipher(algorithms.AES(ctr_key), modes.CTR(ctr)).decryptor()
    raw = d[NCA + a0:NCA + off + size]
    return dec.update(bytes(raw))[off - a0:]


rh = read(DATA_OFF, 0x50)
(h_size, dh_off, dh_sz, dm_off, dm_sz, fh_off, fh_sz, fm_off, fm_sz, data_off) = struct.unpack_from('<10Q', rh)
if h_size != 0x50:
    sys.exit(f'RomFS header looks wrong (size {h_size:#x}) - key or offsets mismatch')
dmeta = read(DATA_OFF + dm_off, dm_sz)
fmeta = read(DATA_OFF + fm_off, fm_sz)
NONE = 0xFFFFFFFF


def walk(doff, path):
    parent, sib, child, file_, hsh, nlen = struct.unpack_from('<6I', dmeta, doff)
    f_ = file_
    while f_ != NONE:
        fp, fsib, foff, fsz, fhsh, fnlen = struct.unpack_from('<IIQQII', fmeta, f_)
        name = fmeta[f_ + 0x20:f_ + 0x20 + fnlen].decode('utf-8', 'replace')
        yield path + name, DATA_OFF + data_off + foff, fsz
        f_ = fsib
    c = child
    while c != NONE:
        cp, csib, cch, cfile, chsh, cnlen = struct.unpack_from('<6I', dmeta, c)
        cname = dmeta[c + 0x18:c + 0x18 + cnlen].decode('utf-8', 'replace')
        yield from walk(c, path + cname + '/')
        c = csib


entries = list(walk(0, ''))
total = sum(s for _, _, s in entries)
print(f'[*] {NCA_NAME}: RomFS with {len(entries)} files, {total / 2**30:.2f} GB')
if a.list:
    for p, o, s in entries:
        print(f'{s:>14,}  {p}')
    sys.exit(0)
if a.only:
    rx = re.compile(a.only)
    entries = [e for e in entries if rx.search(e[0])]

CHUNK = 32 << 20
done = 0; t0 = time.time(); todo = sum(s for _, _, s in entries)
for p, off, size in entries:
    dst = os.path.join(a.out, *p.split('/'))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    have = os.path.getsize(dst) if os.path.exists(dst) else 0
    if have == size:
        done += size; continue
    if have > size:
        have = 0
    with open(dst, 'r+b' if have else 'wb') as o:
        o.seek(have)
        pos = have
        while pos < size:
            n = min(CHUNK, size - pos)
            o.write(read(off + pos, n))
            pos += n
            done_now = done + pos
            el = time.time() - t0
            if el > 0:
                sys.stdout.write(f'\r  {done_now / 2**30:6.2f}/{todo / 2**30:.2f} GB  {done_now / el / 2**20:6.1f} MB/s  {p[:60]:<60}')
                sys.stdout.flush()
    done += size
print(f'\n[+] extracted {len(entries)} files to {a.out}')
