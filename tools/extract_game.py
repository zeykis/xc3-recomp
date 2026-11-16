#!/usr/bin/env python3
"""Extract what the recomp needs from YOUR game dump.

  python3 tools/extract_game.py "Xenoblade Chronicles 3.xci" --keys prod.keys --out game [--romfs]

- Finds the Program NCA inside the XCI secure partition (or NSP).
- Copies only the NCA header + ExeFS section into a sparse file and runs hactool on it, so getting
  the code does NOT need 15 GB of free space.
- Converts exefs/main and exefs/sdk to ELF (game/main.elf, game/sdk.elf) via REA's triage.py.
- --romfs extracts the full RomFS into game/romfs (needs ~15 GB free; required to load assets).
Needs: hactool (PATH or REA/tools), your prod.keys from your own console.
"""
import argparse, mmap, os, re, shutil, struct, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('dump'); ap.add_argument('--keys', required=True); ap.add_argument('--out', default='game')
ap.add_argument('--romfs', action='store_true'); ap.add_argument('--hactool', default=None)
a = ap.parse_args()
here = os.path.dirname(os.path.abspath(__file__))
hac = a.hactool or shutil.which('hactool') or next((os.path.join(r, f) for r in [os.path.join(here, '..', '..', 'tools')]
                                                  for _, _, fs in os.walk(r) for f in fs if f.startswith('hactool')), None)
if not hac:
    sys.exit('hactool not found (build it or run REA install-tools.ps1)')
os.makedirs(a.out, exist_ok=True)
f = open(a.dump, 'rb'); d = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
u32 = lambda o: struct.unpack_from('<I', d, o)[0]; u64 = lambda o: struct.unpack_from('<Q', d, o)[0]
cstr = lambda o: d[o:d.find(b'\0', o)].decode()


def pfs(base, entsz):
    n, st = u32(base + 4), u32(base + 8); ent = base + 0x10; so = ent + entsz * n; ds = so + st
    return [(cstr(so + u32(ent + i * entsz + 16)), ds + u64(ent + i * entsz), u64(ent + i * entsz + 8)) for i in range(n)]


if d[:4] == b'PFS0':
    files = pfs(0, 0x18)
else:
    head = 0x100 if d[0x100:0x104] == b'HEAD' else 0x1100
    root = u64(head + 0x30) + (0x1000 if head == 0x1100 else 0)
    sec = next(o for n, o, s in pfs(root, 0x40) if n == 'secure')
    files = pfs(sec, 0x40)
ncas = sorted([x for x in files if x[0].endswith('.nca') and not x[0].endswith('.cnmt.nca')], key=lambda x: -x[2])
name, off, size = ncas[0]
print(f'[*] program NCA {name} ({size / 2**30:.1f} GB)')
work = os.path.join(a.out, '_nca'); os.makedirs(work, exist_ok=True)
sparse = os.path.join(work, 'program.nca')
with open(sparse, 'wb') as o:
    o.truncate(size); o.write(d[off:off + 0x4000])
info = subprocess.run([hac, '-k', a.keys, sparse], capture_output=True, text=True).stdout
secs = re.findall(r'Section (\d+):\s+Offset:\s+0x([0-9A-Fa-f]+)\s+Size:\s+0x([0-9A-Fa-f]+)\s+Partition Type:\s+(\w+)', info)
exefs = next((int(o, 16), int(s, 16)) for _, o, s, t in secs if t == 'PFS0')
with open(sparse, 'r+b') as o:
    o.seek(exefs[0]); o.write(d[off + exefs[0]:off + exefs[0] + exefs[1]])
subprocess.run([hac, '-k', a.keys, f'--exefsdir={os.path.join(a.out, "exefs")}', sparse], capture_output=True)
print('[+] exefs:', os.listdir(os.path.join(a.out, 'exefs')))
if a.romfs:
    print('[*] extracting full NCA for RomFS (this writes ~15 GB)...')
    full = os.path.join(work, 'program_full.nca')
    with open(full, 'wb') as o:
        for i in range(off, off + size, 64 << 20):
            o.write(d[i:min(off + size, i + (64 << 20))])
    subprocess.run([hac, '-k', a.keys, f'--romfsdir={os.path.join(a.out, "romfs")}', full])
    os.remove(full)
triage = os.path.join(here, '..', '..', 'triage.py')
for mod in ('main', 'sdk'):
    src = os.path.join(a.out, 'exefs', mod)
    subprocess.run([sys.executable, triage, src, '--force'], cwd=os.path.dirname(triage), capture_output=True)
    for root, _, fs in os.walk(os.path.join(os.path.dirname(triage), 'work')):
        for fn in fs:
            if fn == mod + '.elf':
                shutil.copy(os.path.join(root, fn), os.path.join(a.out, mod + '.elf'))
print('[+] done:', sorted(os.listdir(a.out)))
