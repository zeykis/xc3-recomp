"""Static recompiler driver: game/main.elf -> gen/*.c

Usage: python3 recomp/recompile.py [--elf game/main.elf] [--out gen] [--per-file 1500] [--only 0xADDR,...]

Pipeline:
  1. load the ELF wrapper produced by triage.py (program headers only; image at file offset 0x1000)
  2. function starts = eh_frame FDEs + BL targets + code pointers found in RELATIVE relocations
  3. per function: disassemble, find intra-function branch targets + jump tables, translate each insn
  4. write C files, a function table (guest offset -> host fn) and a coverage report
"""
import argparse, collections, json, os, re, struct, sys, time
import capstone
sys.path.insert(0, os.path.dirname(__file__))
import a64c

ap = argparse.ArgumentParser()
ap.add_argument('--elf', default='game/main.elf')
ap.add_argument('--out', default='gen')
ap.add_argument('--per-file', type=int, default=1500)
ap.add_argument('--only', default='')
ap.add_argument('--limit', type=int, default=0, help='only the first N functions (for quick tests)')
ap.add_argument('--no-inline', action='store_true', help='do not inline small leaf functions at their call sites')
ap.add_argument('--patch', action='append', default=None,
                help='IPSwitch .pchtxt code patch (its @enabled sections) applied before translation; repeatable. '
                     'Default: every *.pchtxt in patches/ (or <game>/patches/ if it exists) whose @nsobid matches <game>/exefs/main')
args = ap.parse_args()

raw = open(args.elf, 'rb').read()
IMG = raw[0x1000:]
phoff = struct.unpack_from('<Q', raw, 32)[0]
nph = struct.unpack_from('<H', raw, 56)[0]
segs = []
dyn_off = None
for i in range(nph):
    t, fl, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', raw, phoff + i * 56)
    if t == 1:
        segs.append((va, fs, ms, fl))
    if t == 2:
        dyn_off = va
TEXT_END = segs[0][1]
u32 = lambda o: struct.unpack_from('<I', IMG, o)[0]
u64 = lambda o: struct.unpack_from('<Q', IMG, o)[0]

# ---------------------------------------------------------------- function starts
mod0 = u32(4)
eh_s = mod0 + struct.unpack_from('<i', IMG, mod0 + 16)[0]
fde_count = struct.unpack_from('<i', IMG, eh_s + 8)[0]
starts = {eh_s + struct.unpack_from('<i', IMG, eh_s + 12 + i * 8)[0] for i in range(fde_count)}
n_fde = len(starts)

# dynamic section -> RELA relocations (RELATIVE addends that point into .text are code pointers)
tags = collections.defaultdict(list)
o = dyn_off
while True:
    t, v = struct.unpack_from('<qQ', IMG, o); o += 16
    if t == 0:
        break
    tags[t].append(v)
rela, relasz = tags[7][0], tags[8][0]
code_ptrs = set()
for i in range(relasz // 24):
    r_off, r_info, r_add = struct.unpack_from('<QQq', IMG, rela + i * 24)
    if (r_info & 0xffffffff) == 1027 and 0 < r_add < TEXT_END and r_add % 4 == 0:   # R_AARCH64_RELATIVE
        code_ptrs.add(r_add)

# ---------------------------------------------------------------- code patches (.pchtxt, e.g. 60 fps mods)
# Addresses in these files are offsets in main's memory image (verified on the XC3 2.2.1 60 fps patch: the old
# instruction at each address is the one the patch replaces). Patched bytes go into IMG before disassembly. A patch
# that redirects code with `b CAVE`, where the cave ends with `b SITE+4`, is inlined at the site (DETOURS below):
# the generated function runs the cave's instructions in place, so no new entry points split existing functions.
def nso_build_id(exefs_main):
    try:
        h = open(exefs_main, 'rb').read(0x60)
        return h[0x40:0x60].hex().upper() if h[:4] == b'NSO0' else None
    except OSError:
        return None


def load_pchtxt(path):
    out, bid, on = [], None, False
    for line in open(path, encoding='utf-8', errors='replace'):
        ln = line.strip()
        if ln.startswith('@nsobid-'):
            bid = ln[8:].strip().upper()
        elif ln.startswith('@enabled'):
            on = True
        elif ln.startswith('@disabled') or ln.startswith('@stop'):
            on = False
        elif on and ln and not ln.startswith(('//', '#', '@')):
            m = re.match(r'^([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]+)', ln)
            if m:
                out.append((int(m.group(1), 16), bytes.fromhex(m.group(2))))
    return bid, out


_elf_dir = os.path.dirname(os.path.abspath(args.elf))
_bid = nso_build_id(os.path.join(_elf_dir, 'exefs', 'main'))
# Patches: the repository's patches\ (60 fps / high frame rate / resolution + patches\mods); a game folder's own
# patches\ instead when it has one (local experiments).
_repo_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH_DIR = os.path.join(_elf_dir, 'patches') if os.path.isdir(os.path.join(_elf_dir, 'patches')) else os.path.join(_repo_dir, 'patches')
patch_files = args.patch
if patch_files is None:
    pdir = PATCH_DIR
    patch_files = sorted(os.path.join(pdir, n) for n in os.listdir(pdir) if n.endswith('.pchtxt')) if os.path.isdir(pdir) else []
PATCHED = {}   # address -> original 4 bytes (for the report)
if patch_files:
    IMG = bytearray(IMG)
    for pf in patch_files:
        bid, pts = load_pchtxt(pf)
        if bid and _bid and not _bid.startswith(bid[:len(_bid)]) and not bid.startswith(_bid[:len(bid)]):
            print(f'[!] {pf}: for build {bid[:16]}, not this main ({_bid[:16]}) - skipped')
            continue
        for addr, data in pts:
            if addr + len(data) > TEXT_END:
                sys.exit(f'[!] {pf}: patch at {addr:#x} is outside .text (data patches are not supported)')
            for k in range(0, len(data), 4):
                PATCHED.setdefault(addr + k, bytes(IMG[addr + k:addr + k + 4]))
            IMG[addr:addr + len(data)] = data
        print(f'[*] patch {os.path.basename(pf)}: {len(pts)} writes applied')
    IMG = bytes(IMG)

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.skipdata = True

# ---------------------------------------------------------------- switchable mods (<game>/patches/mods/*.pchtxt)
# Graphics mods made for emulators (IPSwitch patches): each file is one mod the player turns on or off at run time
# (TAB menu / xc3.ini). The code is generated both ways: at every patched instruction, `if (xrt_mod_flags[k])`
# runs the patched instruction(s), else the original. A patched `bl CAVE` into a code cave of the same mod (a few
# instructions ending in `ret`, written into unused code) runs the cave's instructions in place, so the cave
# itself is never written: mods whose caves overlap each other or the permanent patches still combine.
MODS = []      # (name, description)
TOGGLE = {}    # address -> [(mod index, [Ins tuples])]


def load_mods():
    gdir = os.path.dirname(os.path.abspath(args.elf))
    mdir = os.path.join(PATCH_DIR, 'mods')
    found = []   # (mod name, file)
    if os.path.isdir(mdir):
        found += [(os.path.splitext(fn)[0], os.path.join(mdir, fn)) for fn in sorted(os.listdir(mdir)) if fn.endswith('.pchtxt')]
    # emulator mod folders in the game folder's mods\ (mods\<name>\exefs\*.pchtxt): their code patches, named by folder
    edir = os.path.join(gdir, 'mods')
    if os.path.isdir(edir):
        for name in sorted(os.listdir(edir)):
            ex = os.path.join(edir, name, 'exefs')
            if os.path.isdir(ex):
                pf = sorted(f for f in os.listdir(ex) if f.endswith('.pchtxt'))
                pf = [f for f in pf if f.startswith(('2.2', 'main'))] or pf   # (one per game version: prefer this one's)
                found += [(name if len(pf) == 1 else name + ' ' + os.path.splitext(f)[0], os.path.join(ex, f)) for f in pf]
    for mname, path in found:
        fn = os.path.basename(path)
        bid, pts = load_pchtxt(path)
        if bid and _bid and not _bid.startswith(bid[:len(_bid)]) and not bid.startswith(_bid[:len(bid)]):
            print(f'[!] mod {fn}: for build {bid[:16]}, not this main - skipped')
            continue
        desc = ''
        for line in open(path, encoding='utf-8', errors='replace'):
            if line.strip().startswith('//') and line.strip()[2:].strip():
                desc = line.strip()[2:].strip(); break
        words = {}
        for addr, data in pts:
            for k in range(0, len(data), 4):
                words[addr + k] = data[k:k + 4]
        words = {a: w for a, w in words.items() if a + 4 <= TEXT_END and IMG[a:a + 4] != w}   # (already applied: no-op)
        if not words:
            print(f'[*] mod {fn}: nothing to do (already part of the permanent patches)')
            continue
        dis = {a: next(md.disasm_lite(w, a), None) for a, w in words.items()}
        # caves: targets of the mod's own bl, up to its ret
        caves, bad = {}, None
        for a, d in dis.items():
            if d and d[2] in ('bl', 'b') and int(d[3].split('#')[-1], 16) in words:
                t = int(d[3].split('#')[-1], 16)
                if d[2] == 'b':
                    bad = f'b {t:#x} into the mod (detour)'; break
                body, x = [], t
                while x in words and dis[x] and dis[x][2] != 'ret' and len(body) < 64:
                    body.append(dis[x]); x += 4
                if x not in words or not dis[x] or dis[x][2] != 'ret':
                    bad = f'cave at {t:#x} without ret'; break
                if any(m in ('b', 'bl', 'br', 'blr', 'ret', 'adr', 'adrp') or m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz')
                       or (m.startswith('ldr') and '[' not in o) for _, _, m, o in body):
                    bad = f'cave at {t:#x} branches or is pc-relative'; break
                caves[a] = (t, x)
        if bad:
            print(f'[!] mod {fn}: unsupported ({bad}) - skipped'); continue
        cave_words = set()
        for t, end in caves.values():
            cave_words |= set(range(t, end + 4, 4))
        k = len(MODS)
        if k >= 64:
            print('[!] more than 64 mods: the rest are skipped'); break
        sites = 0
        for a, d in sorted(dis.items()):
            if a in cave_words and a not in caves:
                continue
            if d is None:
                print(f'[!] mod {fn}: undecodable word at {a:#x} - ignored'); continue
            if a in caves:
                t, end = caves[a]
                repl = [dis[x] for x in range(t, end, 4)]
                # (cave instructions keep their own addresses in comments; they translate position-independently)
                repl = [(a, 4, m, o) for (_, _, m, o) in repl]
            else:
                repl = [d]
            TOGGLE.setdefault(a, []).append((k, repl))
            sites += 1
        MODS.append((mname, desc))
        print(f'[*] mod {k} "{mname}": {sites} switchable sites' + (f', {len(caves)} cave call(s) inlined' if caves else ''))


load_mods()
t0 = time.time()
ALL = list(md.disasm_lite(IMG[:TEXT_END], 0))
INDEX = {a: i for i, (a, s, m, o) in enumerate(ALL)}
bl_targets = {int(o.lstrip('#'), 16) for a, s, m, o in ALL if m == 'bl'}


def _btarget(op):
    return int(op.split('#')[-1], 16)


def find_detour(site):
    """Patched `b CAVE` at site whose every path ends with `b site+4`: the cave's instructions, or None."""
    a, sz, m, o = ALL[INDEX[site]]
    if m != 'b':
        return None
    cave, back = _btarget(o), site + 4
    seen, todo = set(), [cave]
    while todo:
        x = todo.pop()
        if x in seen:
            continue
        if x not in INDEX or len(seen) > 64:
            return None
        seen.add(x)
        _, _, mm, oo = ALL[INDEX[x]]
        if mm in ('ret', 'br', 'blr', 'udf', 'brk', 'svc'):
            return None
        if mm == 'b':
            t = _btarget(oo)
            if t != back:
                todo.append(t)
            continue
        if mm.startswith('b.') or mm in ('cbz', 'cbnz', 'tbz', 'tbnz'):
            todo.append(_btarget(oo))
        todo.append(x + 4)
    return sorted(seen)


DETOURS = {}
for _a in sorted(PATCHED):
    if _a in INDEX and ALL[INDEX[_a]][2] == 'b' and PATCHED.get(_a) is not None:
        d = find_detour(_a)
        if d:
            DETOURS[_a] = d
if PATCHED:
    print(f'[*] {len(DETOURS)} patch detours inlined: ' + ', '.join(f'{k:#x}->{v[0]:#x}' for k, v in DETOURS.items()))
starts |= {t for t in bl_targets if t < TEXT_END}
# code pointers become function starts only if they are not inside a known jump-table-ish place:
starts |= {p for p in code_ptrs if p in INDEX}
# PLT stubs (adrp x16 / ldr x17, [x16, #..] / add x16 / br x17): code may take their address
plt = {ALL[i][0] for i in range(len(ALL) - 3)
       if ALL[i][2] == 'adrp' and ALL[i][3].startswith('x16,') and ALL[i + 1][2] == 'ldr' and ALL[i + 1][3].startswith('x17, [x16')
       and ALL[i + 3][2] == 'br' and ALL[i + 3][3] == 'x17'}
starts |= plt
# extra entry points discovered at runtime (one hex address per line)
# Per game version: the game folder's extra_entries.txt (next to the ELF; the runtime appends to it when it hits an
# indirect jump to an address that isn't a function start) and recomp_entries.txt (entries added by hand / by patches).
extra = set()
elf_dir = os.path.dirname(os.path.abspath(args.elf))
# Plus the repository's recomp/entries/<build id prefix>.txt for this game version (the ones found so far).
_ent = [os.path.join(os.path.dirname(os.path.abspath(__file__)), 'entries', f) for f in
        (sorted(os.listdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'entries'))) if os.path.isdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'entries')) else [])
        if f.endswith('.txt') and _bid and _bid.startswith(os.path.splitext(f)[0].upper())]
for extra_path in [os.path.join(elf_dir, 'extra_entries.txt'), os.path.join(elf_dir, 'recomp_entries.txt')] + _ent:
    if os.path.exists(extra_path):
        for line in open(extra_path):
            line = line.split('#')[0].strip()
            if line:
                extra.add(int(line, 16))
starts |= extra
# functions to instrument with an entry trace (debugging aid; one hex address per line)
trace_path = os.path.join(elf_dir, 'trace_funcs.txt')
TRACE = set()
if os.path.exists(trace_path):
    for line in open(trace_path):
        line = line.split('#')[0].strip()
        if line:
            TRACE.add(int(line, 16))
# runtime hooks (recomp/hooks.txt): "<build id prefix> <hex address> # what": the function calls xrt_hook(c, addr) on
# entry, where the runtime may adjust guest state (e.g. render sizes). Only lines for this main's build id apply.
HOOKS = set()
_hooks_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hooks.txt')
if os.path.exists(_hooks_path):
    for line in open(_hooks_path):
        f = line.split('#')[0].split()
        if len(f) == 2 and _bid and _bid.upper().startswith(f[0].upper()):
            HOOKS.add(int(f[1], 16))
print(f'[*] {len(plt)} PLT stubs, {len(extra)} extra entries, {len(TRACE)} traced functions, {len(HOOKS)} hooks')
starts = sorted(s for s in starts if s in INDEX)
print(f'[*] {len(ALL)} insns, {n_fde} FDE functions, {len(bl_targets)} BL targets, {len(code_ptrs)} code ptrs in relocs '
      f'-> {len(starts)} functions ({time.time() - t0:.0f}s)')
START_SET = set(starts)


def fname(addr):
    return f'f_{addr:07x}'


class Ins:
    __slots__ = ('address', 'mnemonic', 'op_str')

    def __init__(self, t):
        self.address, _, self.mnemonic, self.op_str = t


# ---------------------------------------------------------------- jump tables
JT_LD = re.compile(r'^(ldrb|ldrh|ldrsw|ldrsh|ldrsb|ldr) w(\d+), \[x(\d+), x(\d+)(?:, lsl #(\d+))?\]$')


def jump_table(fins, k, reg, lo_hi=(0, 0)):
    """Pattern: adrp xT,#P; add xT,xT,#O; adr xB,#base; ldrb wE,[xT,xI]; add xR,xB,xE,lsl #2; br xR"""
    base = table = ent = idx = None
    add_src = None
    for j in range(k - 1, max(-1, k - 14), -1):
        a, m, o = fins[j].address, fins[j].mnemonic, fins[j].op_str
        if add_src is None and m == 'add':
            mm = re.match(rf'^x{reg}, x(\d+), x(\d+)(?:, (lsl|sxtb|sxth|sxtw) #2)?$', o)
            if mm:
                add_src = (int(mm.group(1)), int(mm.group(2)))
                continue
        if add_src and m == 'adr' and o.startswith(f'x{add_src[0]},'):
            base = int(o.split('#')[1], 16)
        if add_src and ent is None:
            mm = JT_LD.match(m + ' ' + o)
            if mm and int(mm.group(2)) == add_src[1]:
                ent = mm.group(1); tbl_reg = int(mm.group(3)); idx = int(mm.group(4))
        if ent and table is None and m == 'add' and re.match(rf'^x{tbl_reg}, x{tbl_reg}, #', o):
            table_lo = int(o.split('#')[1], 16)
            for jj in range(j - 1, max(-1, j - 6), -1):
                if fins[jj].mnemonic == 'adrp' and fins[jj].op_str.startswith(f'x{tbl_reg},'):
                    table = int(fins[jj].op_str.split('#')[1], 16) + table_lo
                    break
    if ent and table is None and base is not None:
        # table base hoisted out of a loop: find the last writer of xT anywhere earlier in the function
        for j in range(k - 1, -1, -1):
            o = fins[j].op_str
            if not re.match(rf'^[wx]{tbl_reg}\b', o) or fins[j].mnemonic in ('str', 'stp', 'strb', 'strh', 'cmp', 'tst', 'cbz', 'cbnz', 'tbz', 'tbnz'):
                continue
            if fins[j].mnemonic in ('ldp', 'ldr') and '[sp' in o:
                continue               # callee-saved restore in an epilogue on another path
            if fins[j].mnemonic == 'add' and re.match(rf'^x{tbl_reg}, x{tbl_reg}, #', o):
                lo_ = int(o.split('#')[1], 16)
                for jj in range(j - 1, max(-1, j - 8), -1):
                    if fins[jj].mnemonic == 'adrp' and fins[jj].op_str.startswith(f'x{tbl_reg},'):
                        table = int(fins[jj].op_str.split('#')[1], 16) + lo_
                        break
            break
    if base is None or table is None or ent is None:
        return None
    # bound: cmp wI/xI, #N ; b.hi -> N+1 entries (follow 'mov wI, wS' copies of the index)
    count = None
    regs = {idx}
    for j in range(k - 1, max(-1, k - 40), -1):
        m, o = fins[j].mnemonic, fins[j].op_str
        mv = re.match(r'^[wx](\d+), [wx](\d+)$', o)
        if m == 'mov' and mv and int(mv.group(1)) in regs:
            regs.add(int(mv.group(2)))
            continue
        mm = re.match(r'^[wx](\d+), #(\w+)$', o)
        if m == 'cmp' and mm and int(mm.group(1)) in regs:
            count = int(mm.group(2), 0) + 1
            break
    size = {'ldrb': 1, 'ldrsb': 1, 'ldrh': 2, 'ldrsh': 2, 'ldrsw': 4, 'ldr': 4}[ent]
    fmt = {'ldrb': 'B', 'ldrsb': 'b', 'ldrh': 'H', 'ldrsh': 'h', 'ldrsw': 'i', 'ldr': 'I'}[ent]
    targets = []
    for e in range(count or 512):
        v = struct.unpack_from('<' + fmt, IMG, table + e * size)[0]
        t = base + (v << 2)
        if count is None and not (lo_hi[0] <= t < lo_hi[1]):
            break                      # unknown bound: stop at the first entry leaving the function
        targets.append(t)
    return targets


# ---------------------------------------------------------------- per-function translation
stats = collections.Counter()
unhandled = collections.Counter()
unhandled_ex = {}
jt_stats = collections.Counter()


class Env:
    test = False

    def __init__(self, lo, hi, labels):
        self.lo, self.hi, self.labels = lo, hi, labels
        self.jts = {}

    def label(self, t):
        return f'L_{t:07x}' if t in self.labels else None

    def func(self, t):
        return fname(t) if t in START_SET else None

    def inline_leaf(self, t):
        """Body of a small straight-line leaf function at t (C lines), or None: such calls are inlined (M5)."""
        return inline_leaf(t)

    def indirect_jump(self, ins, r):
        tg = self.jts.get(ins.address)
        x = r.r()
        if not tg:
            if getattr(self, 'br_any', None) and x not in ('c->x[16]', 'c->x[17]'):
                cases = ' '.join(f'case 0x{t:x}: goto L_{t:07x};' for t in self.br_any)
                return [f'switch ({x} - G_BASE) {{ {cases} default: xrt_dispatch(c, {x}); return; }}']
            return [f'xrt_dispatch(c, {x}); return;']
        cases = ' '.join(f'case 0x{t:x}: goto L_{t:07x};' for t in sorted(set(tg)))
        return [f'switch ({x} - G_BASE) {{ {cases} default: xrt_dispatch(c, {x}); return; }}']


# Small leaf functions (accessors and the like: at most INLINE_MAX instructions, no branches, no calls, no use of the
# link register) are inlined at their direct call sites: no call, and the guest registers around it stay in host
# registers. The function itself is still generated (indirect calls, function table). --no-inline: off.
INLINE_MAX = 12
NO_INLINE_OPS = {'b', 'bl', 'blr', 'br', 'braa', 'blraa', 'cbz', 'cbnz', 'tbz', 'tbnz', 'svc', 'hvc', 'brk', 'udf', 'hlt', 'eret', 'ret'}
_inline_cache = {}
inline_stats = collections.Counter()


def inline_leaf(t):
    if args.no_inline or os.environ.get('XRT_NO_INLINE'):   # (XRT_NO_INLINE=1 from a runner job: A/B builds)
        return None
    if t in _inline_cache:
        return _inline_cache[t]
    body = None
    if t in START_SET and t not in HOOKS and t not in TRACE and t in INDEX:
        end = bounds.get(t, t)
        i = INDEX[t]; ins_list = []
        ok = False
        while i < len(ALL) and ALL[i][0] < end and len(ins_list) <= INLINE_MAX:
            a, sz, m, o = ALL[i]
            if m == 'ret' and o.strip() in ('', 'x30'):
                ok = True; break
            if m in NO_INLINE_OPS or m.startswith('b.') or 'x30' in o or 'lr' in o.split() or a in DETOURS or a in TOGGLE:
                break
            ins_list.append(Ins(ALL[i])); i += 1
        if ok and len(ins_list) <= INLINE_MAX:
            body = []
            env = Env(t, end, set())
            try:
                for ins in ins_list:
                    lines = a64c.translate(ins, env)
                    if any('return' in l or 'goto' in l for l in lines):
                        body = None; break
                    body.append(f'/* {ins.address:07x}: {ins.mnemonic} {ins.op_str} (inlined) */')
                    body += lines
            except Exception:
                body = None
    _inline_cache[t] = body
    return body


def translate_function(lo, hi):
    i0 = INDEX[lo]
    fins = []
    i = i0
    while i < len(ALL) and ALL[i][0] < hi:
        fins.append(Ins(ALL[i])); i += 1
    # patch detours: the cave's instructions replace the site's `b CAVE`; its branches stay local (labels)
    spliced = []
    for ins in fins:
        cave = DETOURS.get(ins.address)
        if cave:
            spliced += [Ins(ALL[INDEX[x]]) for x in cave]
        else:
            spliced.append(ins)
    detour_lo_hi = [(min(v), max(v) + 4, k + 4) for k, v in DETOURS.items() if lo <= k < hi]
    fins = spliced
    labels = set()
    jts = {}
    unresolved_br = False
    in_range = lambda t: lo <= t < hi or any(a <= t < b for a, b, _ in detour_lo_hi)
    for k, ins in enumerate(fins):
        m = ins.mnemonic
        if m == 'b' or m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz'):
            t = int(ins.op_str.split('#')[-1], 16)
            if in_range(t):
                labels.add(t)
        elif m == 'br':
            tg = jump_table(fins, k, int(ins.op_str[1:]) if ins.op_str[1:].isdigit() else -1, (lo, hi))
            if tg and all(lo <= t < hi for t in tg):
                jts[ins.address] = tg
                labels |= set(tg)
                jt_stats['resolved'] += 1
            else:
                jt_stats['tail/indirect'] += 1
                if ins.op_str not in ('x16', 'x17'):   # (x16 / x17: PLT veneers and tail calls through them)
                    unresolved_br = True
    # A `br` whose jump table was not found may jump inside this function (switch cases the game reached later and
    # that ended it as "not a function start"): it may go to any label or to code right after an unconditional jump /
    # return, all of which get a label and a case; other targets go through the dispatcher as before.
    br_any = []
    if unresolved_br:
        for k, ins in enumerate(fins[:-1]):
            if ins.mnemonic in ('b', 'br', 'ret', 'braa', 'retaa') and lo <= fins[k + 1].address < hi:
                labels.add(fins[k + 1].address)
        br_any = sorted(t for t in labels if lo <= t < hi)
        jt_stats['br any-label'] += 1
    for ins in fins:   # switchable mods: their branches need labels too
        for _k, repl in TOGGLE.get(ins.address, ()):
            for (_a, _s, m, o) in repl:
                if m == 'b' or m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                    t = int(o.split('#')[-1], 16)
                    if in_range(t):
                        labels.add(t)
    env = Env(lo, hi, labels)
    env.jts = jts
    env.br_any = br_any
    for a_, b_, back in detour_lo_hi:
        labels.add(back)
    out = [f'void {fname(lo)}(Ctx *restrict c) {{']
    if lo in TRACE:
        out.append(f'  xrt_trace(c, 0x{lo:x});')
    if lo in HOOKS:
        out.append(f'  if (xrt_hook_ret(c, 0x{lo:x})) return;')
    last_terminal = False
    for ins in fins:
        if ins.address in labels:
            out.append(f'L_{ins.address:07x}:;')
        stats[ins.mnemonic] += 1
        try:
            lines = a64c.translate(ins, env)
        except a64c.Unhandled as e:
            unhandled[ins.mnemonic] += 1
            unhandled_ex.setdefault(ins.mnemonic + ' ' + str(e), f'{ins.mnemonic} {ins.op_str}')
            txt = f'{ins.mnemonic} {ins.op_str}'.replace('"', "'")
            lines = [f'xrt_unimpl(c, G_BASE + 0x{ins.address:x}ULL, "{txt}"); return;']
        except Exception as e:  # translator bug - keep going, report it
            unhandled['!' + ins.mnemonic] += 1
            unhandled_ex.setdefault('!' + ins.mnemonic + ' ' + repr(e), f'{ins.mnemonic} {ins.op_str}')
            txt = f'{ins.mnemonic} {ins.op_str}'.replace('"', "'")
            lines = [f'xrt_unimpl(c, G_BASE + 0x{ins.address:x}ULL, "{txt}"); return;']
        out.append(f'  /* {ins.address:07x}: {ins.mnemonic} {ins.op_str} */')
        if ins.address in TOGGLE:   # switchable mod(s): patched instruction(s) when on, the original otherwise
            first = True
            for k, repl in TOGGLE[ins.address]:
                alt = []
                try:
                    for r in repl:
                        alt.append(f'/* mod {k}: {r[2]} {r[3]} */')
                        alt += a64c.translate(Ins(r), env)
                except Exception as e:
                    print(f'[!] mod {k} at {ins.address:#x}: {r[2]} {r[3]} not translatable ({e}) - site left original')
                    continue
                out.append(f'  {"if" if first else "else if"} (xrt_mod_flags[{k}]) {{')
                out += ['    ' + l for l in alt]
                out.append('  }')
                first = False
            if not first:
                out.append('  else {')
                out += ['    ' + l for l in lines]
                out.append('  }')
                jt_stats['mod sites'] += 1
            else:
                out += ['  ' + l for l in lines]
        else:
            out += ['  ' + l for l in lines]
        last_terminal = ins.mnemonic in ('b', 'br', 'ret', 'udf') or (lines and lines[-1].endswith('return;') and ins.mnemonic in ('brk',))
    if not last_terminal:
        if hi in START_SET:
            out.append(f'  {fname(hi)}(c); return;   /* falls through into next function */')
        else:
            out.append(f'  xrt_dispatch(c, G_BASE + 0x{hi:x}ULL); return;')
    out.append('}')
    return out


os.makedirs(args.out, exist_ok=True)


def write_if_changed(path, text):
    """Keep mtimes stable so incremental builds only recompile what changed."""
    if os.path.exists(path) and open(path).read() == text:
        return
    open(path, 'w').write(text)
todo = starts
if args.only:
    want = {int(x, 16) for x in args.only.split(',')}
    todo = [s for s in starts if s in want]
if args.limit:
    todo = todo[:args.limit]
bounds = {s: (starts[i + 1] if i + 1 < len(starts) else TEXT_END) for i, s in enumerate(starts)}

hdr = ['/* generated - do not edit */', '#include "xrt.h"']
hdr += [f'void {fname(s)}(Ctx *restrict c);' for s in starts]
write_if_changed(os.path.join(args.out, 'funcs.h'), '\n'.join(hdr) + '\n')

t0 = time.time()
files = []
for fi in range(0, len(todo), args.per_file):
    chunk = todo[fi:fi + args.per_file]
    name = f'main_{fi // args.per_file:03d}.c'
    lines = ['/* generated from main.elf - do not edit */', '#include "funcs.h"', '#pragma GCC diagnostic ignored "-Wunused-label"', '']
    for s in chunk:
        lines += translate_function(s, bounds[s]) + ['']
    write_if_changed(os.path.join(args.out, name), '\n'.join(lines))
    files.append(name)
    if fi // args.per_file % 5 == 0:
        print(f'  {name}: {fi + len(chunk)}/{len(todo)} functions ({time.time() - t0:.0f}s)', flush=True)

# function table (offset -> fn); runtime builds a direct-mapped lookup from this
ft = ['/* generated */', '#include "funcs.h"', 'typedef struct { uint32_t off; GuestFn fn; } FnEntry;',
      f'const unsigned xrt_fn_count = {len(todo)};', 'const FnEntry xrt_fn_table[] = {']
ft += [f'  {{0x{s:x}, {fname(s)}}},' for s in todo]
ft += ['};']
# switchable mods (TAB menu): names and descriptions; the flags live in the runtime (xrt_mod_flags)
cstr = lambda t: '"' + t.replace('\\', '\\\\').replace('"', '\\"') + '"'
ft += [f'const unsigned xrt_mod_count = {len(MODS)};',
       'const char *const xrt_mod_names[] = {' + ', '.join(cstr(n) for n, _ in MODS) + (', ' if MODS else '') + '0};',
       'const char *const xrt_mod_descs[] = {' + ', '.join(cstr(d) for _, d in MODS) + (', ' if MODS else '') + '0};']
write_if_changed(os.path.join(args.out, 'functable.c'), '\n'.join(ft) + '\n')
# the game version this build is for (installer: tools/setup checks the player's files against it)
write_if_changed(os.path.join(args.out, 'build_info.h'),
                 '/* generated */\n#define XC3_MAIN_BUILD_ID "%s"\n' % ((_bid or '').upper()))

total = sum(stats.values()); bad = sum(unhandled.values())
cov = {'functions': len(todo), 'instructions': total, 'unhandled': bad,
       'coverage_pct': round(100 * (total - bad) / max(total, 1), 3), 'jump_tables': dict(jt_stats),
       'unhandled_by_mnemonic': unhandled.most_common(), 'unhandled_examples': unhandled_ex, 'files': files}
os.makedirs('analysis', exist_ok=True)
json.dump(cov, open('analysis/coverage.json', 'w'), indent=1)
print(f'[+] {len(todo)} functions, {total} insns, coverage {cov["coverage_pct"]}%, jump tables {dict(jt_stats)} ({time.time() - t0:.0f}s)')
print('    unhandled:', unhandled.most_common(25))
