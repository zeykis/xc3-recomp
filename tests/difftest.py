"""Differential test: generated C vs Unicorn (reference AArch64 emulator).

For every distinct instruction *form* used by the game (up to N samples each), run the single
instruction on random register/flag/memory states in both, and compare all architectural state.

  python3 tests/difftest.py [--per-form 3] [--trials 20] [--filter fmla]
"""
import argparse, collections, ctypes, json, math, os, random, re, struct, subprocess, sys
import capstone
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL
from unicorn.arm64_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'recomp'))
import a64c

ap = argparse.ArgumentParser()
ap.add_argument('--elf', default=os.path.join(ROOT, 'game/main.elf'))
ap.add_argument('--per-form', type=int, default=3)
ap.add_argument('--trials', type=int, default=20)
ap.add_argument('--filter', default='')
ap.add_argument('--seed', type=int, default=1)
args = ap.parse_args()
random.seed(args.seed)

DATA = 0x200000000
DSIZE = 0x100000
MID = DATA + DSIZE // 2
SKIP = {'b', 'bl', 'blr', 'br', 'ret', 'udf', '.byte', 'nop', 'prfm', 'prfum', 'dmb', 'clrex',
        # exclusives need a paired load/store - tested separately
        'ldaxr', 'ldxr', 'stlxr', 'stxr', 'ldaxrb', 'stxrb', 'stlxrb', 'ldaxrh', 'stlxrh',
        # estimate instructions: ARM returns an 8-bit estimate, we return the exact value (intentional)
        'frecpe', 'frsqrte'}

raw = open(args.elf, 'rb').read()
IMG = raw[0x1000:]
TEXT_END = struct.unpack_from('<Q', raw, struct.unpack_from('<Q', raw, 32)[0] + 32)[0]
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); md.skipdata = True

forms = collections.defaultdict(list)
for a, s, m, o in md.disasm_lite(IMG[:TEXT_END], 0):
    if m in SKIP or (args.filter and not re.search(args.filter, m)):
        continue
    k = m + ' ' + re.sub(r'\b([xwsdhqbv])\d+\b', r'\1N', re.sub(r'#-?0x[0-9a-f]+|#-?\d+(\.\d+(e[+-]?\d+)?)?', '#I', o))
    if len(forms[k]) < args.per_form:
        forms[k].append((a, m, o, IMG[a:a + 4]))


class TEnv:
    test = True
    def label(self, t): return None
    def func(self, t): return None
    def indirect_jump(self, ins, r): return ['return 2;']


class I:
    def __init__(self, a, m, o): self.address, self.mnemonic, self.op_str = a, m, o


cases, src = [], ['#include "xrt.h"', '#include <sys/mman.h>', 'uint64_t G_BASE = 0;',
                  'void xrt_dispatch(Ctx *c, uint64_t t) {}', 'void xrt_trap(Ctx *c, uint64_t p, const char *w) {}',
                  'void xrt_unimpl(Ctx *c, uint64_t p, const char *t) {}']
for k, samples in forms.items():
    for (a, m, o, word) in samples:
        try:
            body = a64c.translate(I(a, m, o), TEnv())
        except Exception as e:
            continue
        n = len(cases)
        cases.append((k, a, m, o, word))
        src.append(f'static int t{n}(Ctx *c) {{ ' + ' '.join(body) + ' return 0; }')
src.append('typedef int (*TF)(Ctx *);')
src.append('static TF T[] = {' + ', '.join(f't{i}' for i in range(len(cases))) + '};')
src.append('''
int init(void) { void *p = mmap((void *)0x200000000ULL, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0); return p == (void *)0x200000000ULL; }
int run(int i, uint64_t *x, uint64_t *v, uint64_t *f) {
  Ctx c; memset(&c, 0, sizeof c);
  for (int k = 0; k < 32; k++) c.x[k] = x[k];
  for (int k = 0; k < 32; k++) { c.v[k].d[0] = v[2*k]; c.v[k].d[1] = v[2*k+1]; }
  c.n = (*f >> 31) & 1; c.z = (*f >> 30) & 1; c.c = (*f >> 29) & 1; c.vf = (*f >> 28) & 1;
  int r = T[i](&c);
  for (int k = 0; k < 32; k++) x[k] = c.x[k];
  for (int k = 0; k < 32; k++) { v[2*k] = c.v[k].d[0]; v[2*k+1] = c.v[k].d[1]; }
  *f = ((uint64_t)c.n << 31) | ((uint64_t)c.z << 30) | ((uint64_t)c.c << 29) | ((uint64_t)c.vf << 28);
  return r;
}''')
os.makedirs('/tmp/difftest', exist_ok=True)
open('/tmp/difftest/t.c', 'w').write('\n'.join(src))
so = '/tmp/difftest/t.so'
r = subprocess.run(['clang', '-O1', '-w', '-shared', '-fPIC', '-I' + os.path.join(ROOT, 'runtime/include'), '/tmp/difftest/t.c', '-o', so, '-lm'],
                   capture_output=True, text=True)
if r.returncode:
    print(r.stderr[:3000]); sys.exit(1)
lib = ctypes.CDLL(so)
assert lib.init() == 1
mem = (ctypes.c_char * DSIZE).from_address(DATA)

FLOATS = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.25, -7.5, 1e10, -1e-10, 123456.789, float('inf'), float('-inf'), 0.1, 255.0, 65535.5, -2147483648.0, 4294967296.0]


def rand_fp_lane(bits):
    if random.random() < 0.15:
        v = random.choice(FLOATS)
    else:
        v = random.uniform(-1000, 1000) if random.random() < 0.7 else random.uniform(-1e6, 1e6)
    return struct.unpack('<I', struct.pack('<f', v))[0] if bits == 32 else struct.unpack('<Q', struct.pack('<d', v))[0]


def rand_state(m, o):
    memop = '[' in o or m.startswith(('ld', 'st'))
    x = []
    for i in range(32):
        if memop:
            x.append(MID + random.randrange(-0x400, 0x400, 16))
        else:
            x.append(random.choice([random.getrandbits(64), random.getrandbits(32), random.randrange(0, 64),
                                    (1 << 64) - random.randrange(1, 100), 0, 1 << 63, 0x80000000, random.getrandbits(16)]))
    mi = re.search(r'\[\w+, ([xw])(\d+)', o)
    if mi:   # register index: keep it small so base+index stays inside the test buffer
        val = random.randrange(0, 0x40) if (mi.group(1) == 'w' and 'sxtw' not in o) else random.randrange(-0x40, 0x40)
        x[int(mi.group(2))] = val & (2 ** 64 - 1)
    v = []
    fp = m.startswith(('f', 'scvtf', 'ucvtf')) or '.4s' in o or '.2s' in o or '.2d' in o
    for i in range(32):
        if fp:
            if '.2d' in o or re.search(r'\bd\d', o):
                v += [rand_fp_lane(64), rand_fp_lane(64)]
            else:
                v += [rand_fp_lane(32) | rand_fp_lane(32) << 32, rand_fp_lane(32) | rand_fp_lane(32) << 32]
        else:
            v += [random.getrandbits(64), random.getrandbits(64)]
    f = random.getrandbits(4) << 28
    return x, v, f


def unicorn_run(a, word, x, v, f, memimg):
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    page = a & ~0xfff
    mu.mem_map(page, 0x2000, UC_PROT_ALL)
    mu.mem_write(a, bytes(word))
    mu.mem_map(DATA, DSIZE, UC_PROT_ALL)
    mu.mem_write(DATA, memimg)
    # enable FP/SIMD
    mu.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)
    for i in range(29):
        mu.reg_write(UC_ARM64_REG_X0 + i, x[i])
    mu.reg_write(UC_ARM64_REG_X29, x[29]); mu.reg_write(UC_ARM64_REG_X30, x[30]); mu.reg_write(UC_ARM64_REG_SP, x[31])
    for i in range(32):
        mu.reg_write(UC_ARM64_REG_Q0 + i, v[2 * i] | (v[2 * i + 1] << 64))
    mu.reg_write(UC_ARM64_REG_NZCV, f)
    mu.emu_start(a, a + 4, count=1)
    xo = [mu.reg_read(UC_ARM64_REG_X0 + i) for i in range(29)] + [mu.reg_read(UC_ARM64_REG_X29), mu.reg_read(UC_ARM64_REG_X30), mu.reg_read(UC_ARM64_REG_SP)]
    vo = []
    for i in range(32):
        q = mu.reg_read(UC_ARM64_REG_Q0 + i)
        vo += [q & (2 ** 64 - 1), q >> 64]
    return xo, vo, mu.reg_read(UC_ARM64_REG_NZCV) & 0xf0000000, mu.reg_read(UC_ARM64_REG_PC), bytes(mu.mem_read(DATA, DSIZE))


def is_nan_lane(val, bits):
    if bits == 32:
        return (val & 0x7f800000) == 0x7f800000 and (val & 0x7fffff)
    return (val & 0x7ff0000000000000) == 0x7ff0000000000000 and (val & 0xfffffffffffff)


def v_equal(a, b):
    if a == b:
        return True
    for bits in (32, 64):   # treat any NaN == any NaN (payload differences are irrelevant)
        ok = True
        for sh in range(0, 64, bits):
            la, lb = (a >> sh) & ((1 << bits) - 1), (b >> sh) & ((1 << bits) - 1)
            if la != lb and not (is_nan_lane(la, bits) and is_nan_lane(lb, bits)):
                ok = False; break
        if ok:
            return True
    return False


fails = collections.defaultdict(list)
uerr = {}
results = collections.Counter()
for n, (k, a, m, o, word) in enumerate(cases):
    for t in range(args.trials):
        x, v, f = rand_state(m, o)
        memimg = bytes(random.getrandbits(8) for _ in range(64)) * (DSIZE // 64)
        ctypes.memmove(DATA, memimg, DSIZE)
        try:
            ux, uv, uf, upc, umem = unicorn_run(a, word, x, v, f, memimg)
        except Exception as e:
            results['unicorn_error'] += 1
            uerr[k] = f'{m} {o}: {e}'
            break
        cx = (ctypes.c_uint64 * 32)(*x); cv = (ctypes.c_uint64 * 64)(*v); cf = ctypes.c_uint64(f)
        ret = lib.run(n, cx, cv, ctypes.byref(cf))
        diffs = []
        if m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz'):
            taken = upc != a + 4
            if taken != (ret == 1):
                diffs.append(f'branch taken={taken} c={ret}')
        else:
            for i in range(32):
                if cx[i] != ux[i]:
                    diffs.append(f'x{i}: c={cx[i]:#x} uc={ux[i]:#x}')
            for i in range(32):
                if not v_equal(cv[2 * i] | cv[2 * i + 1] << 64, uv[2 * i] | uv[2 * i + 1] << 64):
                    diffs.append(f'v{i}: c={cv[2*i+1]:016x}{cv[2*i]:016x} uc={uv[2*i+1]:016x}{uv[2*i]:016x}')
            if cf.value != uf:
                diffs.append(f'nzcv: c={cf.value >> 28:04b} uc={uf >> 28:04b}')
            cmem = mem.raw
            if cmem != umem:
                pos = next(i for i in range(DSIZE) if cmem[i] != umem[i])
                diffs.append(f'mem@{DATA + pos:#x}: c={cmem[pos]:02x} uc={umem[pos]:02x}')
        if diffs:
            results['fail'] += 1
            if len(fails[k]) < 2:
                fails[k].append(f'{m} {o}  -> ' + '; '.join(diffs[:4]))
        else:
            results['pass'] += 1

print(f'forms={len(forms)} cases={len(cases)} trials={args.trials} -> {dict(results)}')
print(f'failing forms: {len(fails)}')
for k, v in sorted(fails.items()):
    for line in v:
        print('  FAIL', line)
print('unicorn errors:', len(uerr)); [print('  UERR', v) for v in list(uerr.values())[:30]]
json.dump({'results': dict(results), 'failing_forms': {k: v for k, v in fails.items()}},
          open(os.path.join(ROOT, 'analysis/difftest.json'), 'w'), indent=1)
