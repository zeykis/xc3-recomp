"""AArch64 -> C instruction translator.

translate(insn, env) returns a list of C statements for one instruction.
Operands are parsed from capstone's text output (stable across capstone versions).
Anything not handled raises Unhandled, which the driver turns into an xrt_unimpl() call so the
output always compiles and the coverage report tells us what to implement next.

`env` provides:
  env.label(addr)      -> label name if addr is a branch target inside the current function, else None
  env.func(addr)       -> C function name if addr is a known function start, else None
  env.test             -> True when generating single-instruction test harnesses
"""
import re
import struct


class Unhandled(Exception):
    pass


# ---------------------------------------------------------------- operand parsing
def split_ops(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '[{':
            depth += 1
        elif ch in ']}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def imm(tok):
    t = tok.strip().lstrip('#')
    if re.fullmatch(r'-?0x[0-9a-f]+', t):
        v = int(t, 16)
    elif re.fullmatch(r'-?\d+', t):
        v = int(t)
    else:
        raise Unhandled('imm ' + tok)
    return v


def fimm(tok):
    return float(tok.strip().lstrip('#'))


GP = re.compile(r'^(x|w)(\d+)$')


class R:
    """General-purpose register reference."""
    def __init__(self, tok):
        tok = tok.strip()
        self.tok = tok
        if tok in ('sp', 'wsp'):
            self.idx, self.w = 31, 32 if tok == 'wsp' else 64
            self.zr = False
        elif tok in ('xzr', 'wzr'):
            self.idx, self.w, self.zr = None, 32 if tok == 'wzr' else 64, True
        else:
            m = GP.match(tok)
            if not m:
                raise Unhandled('reg ' + tok)
            self.idx, self.w, self.zr = int(m.group(2)), 32 if m.group(1) == 'w' else 64, False

    def r(self):
        if self.zr:
            return '0ULL'
        return f'c->x[{self.idx}]' if self.w == 64 else f'(uint64_t)(uint32_t)c->x[{self.idx}]'

    def set(self, expr):
        if self.zr:
            return [f'(void)({expr});']
        if self.w == 64:
            return [f'c->x[{self.idx}] = (uint64_t)({expr});']
        return [f'c->x[{self.idx}] = (uint32_t)({expr});']


def is_gp(tok):
    tok = tok.strip()
    return bool(GP.match(tok)) or tok in ('sp', 'wsp', 'xzr', 'wzr')


EXT = {'uxtb': ('uint8_t', False), 'uxth': ('uint16_t', False), 'uxtw': ('uint32_t', False), 'uxtx': ('uint64_t', False),
       'sxtb': ('int8_t', True), 'sxth': ('int16_t', True), 'sxtw': ('int32_t', True), 'sxtx': ('int64_t', True)}


def operand2(ops, i, width):
    """Second source operand: register (+ shift/extend) or immediate (+ lsl #12). Returns C expr (uint64)."""
    tok = ops[i]
    mod = ops[i + 1] if i + 1 < len(ops) else None
    if tok.startswith('#'):
        v = imm(tok)
        if mod:
            m = re.fullmatch(r'lsl #(\w+)', mod)
            if not m:
                raise Unhandled('imm mod ' + mod)
            v <<= imm(m.group(1))
        v &= (1 << 64) - 1
        if width == 32:
            v &= 0xffffffff
        return f'0x{v:x}ULL'
    r = R(tok)
    e = r.r()
    if mod:
        m = re.fullmatch(r'(lsl|lsr|asr|ror) #(\w+)', mod)
        if m:
            k, a = m.group(1), imm(m.group(2))
            if k == 'lsl':
                e = f'({e} << {a})'
            elif k == 'lsr':
                e = f'({e} >> {a})'
            elif k == 'asr':
                e = f'(uint64_t)((int64_t){e} >> {a})' if width == 64 else f'(uint64_t)(uint32_t)((int32_t)(uint32_t){e} >> {a})'
            else:
                e = f'ROR64({e}, {a})' if width == 64 else f'(uint64_t)ROR32((uint32_t){e}, {a})'
        else:
            m = re.fullmatch(r'(uxt[bhwx]|sxt[bhwx])(?: #(\w+))?', mod)
            if not m:
                raise Unhandled('mod ' + mod)
            ty, signed = EXT[m.group(1)]
            sh = imm(m.group(2)) if m.group(2) else 0
            base = f'(uint64_t)(int64_t)({ty}){e}' if signed else f'(uint64_t)({ty}){e}'
            e = f'({base} << {sh})' if sh else base
    if width == 32:
        e = f'(uint64_t)(uint32_t)({e})'
    return e


def mem(ops, i):
    """Memory operand at ops[i] (+ optional post-index ops[i+1]).
    Returns (addr_expr, pre_stmts, post_stmts) - address is computed into _a before any loads."""
    tok = ops[i]
    pre = tok.endswith('!')
    inner = tok.rstrip('!').strip()
    if not (inner.startswith('[') and inner.endswith(']')):
        raise Unhandled('mem ' + tok)
    parts = split_ops(inner[1:-1])
    base = R(parts[0])
    b = base.r()
    if len(parts) == 1:
        off = '0'
    elif parts[1].startswith('#'):
        off = f'(uint64_t){imm(parts[1])}LL'
    else:
        idx = R(parts[1])
        e = idx.r()
        if len(parts) > 2:
            m = re.fullmatch(r'(lsl|uxtw|sxtw|sxtx|uxtx)(?: #(\w+))?', parts[2])
            if not m:
                raise Unhandled('memmod ' + parts[2])
            sh = imm(m.group(2)) if m.group(2) else 0
            k = m.group(1)
            if k == 'sxtw':
                e = f'(uint64_t)(int64_t)(int32_t){e}'
            elif k == 'uxtw':
                e = f'(uint64_t)(uint32_t){e}'
            if sh:
                e = f'({e} << {sh})'
        off = e
    stmts = [f'uint64_t _a = {b} + {off};']
    post = []
    if pre:
        post.append(f'{b if not base.zr else "(void)0"} = _a;'.replace('(uint64_t)(uint32_t)', ''))
    if i + 1 < len(ops) and ops[i + 1].startswith('#'):
        stmts = [f'uint64_t _a = {b};']
        v = imm(ops[i + 1])
        if v >= 1 << 63:
            v -= 1 << 64
        post.append(f'{b} = _a + (uint64_t){v}LL;')
    elif i + 1 < len(ops) and is_gp(ops[i + 1]):
        stmts = [f'uint64_t _a = {b};']
        post.append(f'{b} = _a + {R(ops[i + 1]).r()};')
    return stmts, post


# ---------------------------------------------------------------- SIMD/FP register parsing
FPS = re.compile(r'^([bhsdq])(\d+)$')
VEC = re.compile(r'^v(\d+)\.(\d+)([bhsd])$')
ELEM = re.compile(r'^v(\d+)\.([bhsd])\[(\d+)\]$')
LANE = {'b': ('b', 'sb', 8, 'uint8_t'), 'h': ('h', 'sh', 16, 'uint16_t'), 's': ('s', 'ss', 32, 'uint32_t'), 'd': ('d', 'sd', 64, 'uint64_t')}


def fpreg(tok):
    m = FPS.match(tok.strip())
    if not m:
        raise Unhandled('fpreg ' + tok)
    return m.group(1), int(m.group(2))


def vec(tok):
    m = VEC.match(tok.strip())
    if not m:
        raise Unhandled('vec ' + tok)
    return int(m.group(1)), int(m.group(2)), m.group(3)


def elem(tok):
    m = ELEM.match(tok.strip())
    if not m:
        raise Unhandled('elem ' + tok)
    return int(m.group(1)), m.group(2), int(m.group(3))


def fget(tok):
    """Scalar FP value expression."""
    k, i = fpreg(tok)
    if k == 's':
        return f'c->v[{i}].f[0]', 's'
    if k == 'd':
        return f'c->v[{i}].fd[0]', 'd'
    raise Unhandled('fget ' + tok)


def fset(tok, expr):
    k, i = fpreg(tok)
    return [{'s': f'SET_S({i}, {expr});', 'd': f'SET_D({i}, {expr});'}[k]]


COND = {'eq', 'ne', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al', 'cs', 'cc'}


def cond(cc):
    cc = {'cs': 'hs', 'cc': 'lo'}.get(cc, cc)
    if cc not in COND:
        raise Unhandled('cond ' + cc)
    return f'C_{cc.upper()}'


# ---------------------------------------------------------------- control flow emission
def jump(env, target):
    lbl = env.label(target)
    if env.test:
        return ['return 1;']
    if lbl:
        return [f'goto {lbl};']
    fn = env.func(target)
    if fn:
        return [f'{fn}(c); return;']   # tail call
    return [f'xrt_dispatch(c, G_BASE + 0x{target:x}ULL); return;']


def call(env, target, ret):
    if env.test:
        return ['return 2;']
    fn = env.func(target)
    pre = [f'c->x[30] = G_BASE + 0x{ret:x}ULL;']
    if fn:
        body = env.inline_leaf(target) if hasattr(env, 'inline_leaf') else None
        if body is not None:
            return pre + ['{'] + body + ['}']
        return pre + [f'{fn}(c);']
    return pre + [f'xrt_dispatch(c, G_BASE + 0x{target:x}ULL);']


def branch_target(tok):
    return imm(tok)


# ---------------------------------------------------------------- the translator
LOADS = {'ldr': None, 'ldrb': ('uint8_t', 8, False), 'ldrh': ('uint16_t', 16, False), 'ldrsb': ('int8_t', 8, True),
         'ldrsh': ('int16_t', 16, True), 'ldrsw': ('int32_t', 32, True)}
STORES = {'str': None, 'strb': 8, 'strh': 16}


def translate(ins, env):
    m = ins.mnemonic
    ops = split_ops(ins.op_str)
    a = ins.address
    nxt = a + 4
    # normalise unscaled forms
    if m in ('ldur', 'ldurb', 'ldurh', 'ldursb', 'ldursh', 'ldursw', 'stur', 'sturb', 'sturh'):
        m = m.replace('ur', 'r', 1)

    # --- no-ops / hints
    if m in ('nop', 'prfm', 'prfum', 'hint', 'yield', 'isb'):
        return []
    if m in ('dmb', 'dsb'):
        return ['FENCE();']
    if m == 'clrex':
        return ['c->excl_addr = 0;']
    if m == 'hvc':   # (runtime call from a patch: game/patches/*.pchtxt; xrt_hvc in the runtime, execution goes on)
        return [f'xrt_hvc(c, {imm(ops[0])});']
    if m in ('udf', 'brk', 'hlt', '.byte'):
        return [f'xrt_trap(c, G_BASE + 0x{a:x}ULL, "{m}"); return;']

    # --- branches
    if m == 'b':
        return jump(env, branch_target(ops[0]))
    if m.startswith('b.'):
        t = branch_target(ops[0])
        return [f'if ({cond(m[2:])}) {{ ' + ' '.join(jump(env, t)) + ' }'] + (['return 0;'] if env.test else [])
    if m in ('cbz', 'cbnz'):
        r = R(ops[0]); t = branch_target(ops[1])
        op = '==' if m == 'cbz' else '!='
        return [f'if ({r.r()} {op} 0) {{ ' + ' '.join(jump(env, t)) + ' }'] + (['return 0;'] if env.test else [])
    if m in ('tbz', 'tbnz'):
        r = R(ops[0]); bit = imm(ops[1]); t = branch_target(ops[2])
        op = '==' if m == 'tbz' else '!='
        return [f'if ((({r.r()} >> {bit}) & 1) {op} 0) {{ ' + ' '.join(jump(env, t)) + ' }'] + (['return 0;'] if env.test else [])
    if m == 'bl':
        return call(env, branch_target(ops[0]), nxt)
    if m == 'blr':
        if env.test:
            return ['return 2;']
        r = R(ops[0])
        return [f'{{ uint64_t _t = {r.r()}; c->x[30] = G_BASE + 0x{nxt:x}ULL; xrt_dispatch(c, _t); }}']
    if m == 'br':
        if env.test:
            return ['return 2;']
        return env.indirect_jump(ins, R(ops[0]))
    if m == 'ret':
        if env.test:
            return ['return 3;']
        return ['return;']

    # --- moves
    if m == 'mov':
        d = ops[0]
        if is_gp(d) and len(ops) == 2 and is_gp(ops[1]):
            return R(d).set(R(ops[1]).r())
        if is_gp(d) and ops[1].startswith('#'):
            r = R(d)
            v = imm(ops[1]) & ((1 << r.w) - 1)
            return r.set(f'0x{v:x}ULL')
        if is_gp(d) and ELEM.match(ops[1]):           # mov w9, v0.s[1]  (umov)
            vi, k, li = elem(ops[1])
            return R(d).set(f'c->v[{vi}].{LANE[k][0]}[{li}]')
        if FPS.match(d) and ELEM.match(ops[1]):       # mov s0, v0.s[1]  (dup scalar)
            vi, k, li = elem(ops[1]); fk, fi = fpreg(d)
            return [f'{{ uint64_t _t = c->v[{vi}].{LANE[k][0]}[{li}]; c->v[{fi}].d[0] = 0; c->v[{fi}].d[1] = 0; c->v[{fi}].{LANE[k][0]}[0] = _t; }}']
        if ELEM.match(d):                             # ins
            vi, k, li = elem(d)
            if ELEM.match(ops[1]):
                si, sk, sl = elem(ops[1])
                return [f'c->v[{vi}].{LANE[k][0]}[{li}] = c->v[{si}].{LANE[sk][0]}[{sl}];']
            return [f'c->v[{vi}].{LANE[k][0]}[{li}] = ({LANE[k][3]}){R(ops[1]).r()};']
        if VEC.match(d) and VEC.match(ops[1]):        # mov v4.16b, v2.16b  (orr)
            di, n, k = vec(d); si, _, _ = vec(ops[1])
            if n * LANE[k][2] == 64:
                return [f'c->v[{di}].d[0] = c->v[{si}].d[0]; c->v[{di}].d[1] = 0;']
            return [f'c->v[{di}] = c->v[{si}];']
        raise Unhandled('mov form')
    if m in ('movz',):
        r = R(ops[0]); v = imm(ops[1])
        if len(ops) > 2:
            v <<= imm(ops[2].split('#')[1])
        return r.set(f'0x{v:x}ULL')
    if m == 'movk':
        r = R(ops[0]); v = imm(ops[1]); sh = imm(ops[2].split('#')[1]) if len(ops) > 2 else 0
        return r.set(f'({r.r()} & ~(0xffffULL << {sh})) | (0x{v:x}ULL << {sh})')
    if m == 'movn':
        raise Unhandled('movn')
    if m == 'mvn':
        if VEC.match(ops[0]):
            di, n, k = vec(ops[0]); si, _, _ = vec(ops[1])
            if n * LANE[k][2] == 64:
                return [f'c->v[{di}].d[0] = ~c->v[{si}].d[0]; c->v[{di}].d[1] = 0;']
            return [f'c->v[{di}].d[0] = ~c->v[{si}].d[0]; c->v[{di}].d[1] = ~c->v[{si}].d[1];']
        r = R(ops[0]); return r.set(f'~{operand2(ops, 1, r.w)}')
    if m == 'adrp' or m == 'adr':
        r = R(ops[0]); return r.set(f'G_BASE + 0x{imm(ops[1]):x}ULL')

    # --- arithmetic
    if m in ('add', 'sub', 'adds', 'subs', 'cmp', 'cmn') and not VEC.match(ops[0]):
        if m in ('cmp', 'cmn'):
            ops = ['xzr' if R(ops[0]).w == 64 else 'wzr'] + ops
            m = 'subs' if m == 'cmp' else 'adds'
        d = R(ops[0]); s1 = R(ops[1]); w = d.w if not d.zr else (64 if ops[0] == 'xzr' else 32)
        if s1.w == 32:
            w = 32
        b = operand2(ops, 2, w)
        op = '+' if m.startswith('add') else '-'
        out = ['{', f'uint64_t _x = {s1.r()}, _y = {b};']
        if m.endswith('s'):
            out.append(f'FL_{"ADD" if op == "+" else "SUB"}{w}(c, _x, _y);')
        out += d.set(f'_x {op} _y') if not (d.zr and m.endswith('s')) else []
        out.append('}')
        return out
    if m in ('neg', 'negs'):
        d = R(ops[0]); b = operand2(ops, 1, d.w)
        return d.set(f'0 - {b}')
    if m in ('and', 'orr', 'eor', 'bic', 'orn', 'ands', 'bics', 'tst') and not VEC.match(ops[0]):
        if m == 'tst':
            ops = ['xzr' if R(ops[0]).w == 64 else 'wzr'] + ops; m = 'ands'
        d = R(ops[0]); s1 = R(ops[1]); w = 32 if s1.w == 32 else 64
        b = operand2(ops, 2, w)
        expr = {'and': '_x & _y', 'ands': '_x & _y', 'orr': '_x | _y', 'eor': '_x ^ _y', 'bic': '_x & ~_y',
                'bics': '_x & ~_y', 'orn': '_x | ~_y'}[m]
        out = ['{', f'uint64_t _x = {s1.r()}, _y = {b}, _r = {expr};']
        if w == 32:
            out[1] = f'uint64_t _x = {s1.r()}, _y = {b}, _r = (uint32_t)({expr});'
        if m in ('ands', 'bics'):
            out.append(f'FL_LOG{w}(c, _r);')
        out += d.set('_r') + ['}']
        return out
    if m in ('lsl', 'lsr', 'asr', 'ror'):
        d = R(ops[0]); s = R(ops[1]); w = d.w
        if ops[2].startswith('#'):
            amt = str(imm(ops[2]))
        else:
            amt = f'({R(ops[2]).r()} & {w - 1})'
        x = s.r()
        if m == 'lsl':
            e = f'{x} << {amt}'
        elif m == 'lsr':
            e = f'{x} >> {amt}'
        elif m == 'asr':
            e = f'(int64_t){x} >> {amt}' if w == 64 else f'(int32_t)(uint32_t){x} >> {amt}'
        else:
            e = f'ROR64({x}, {amt})' if w == 64 else f'ROR32((uint32_t){x}, {amt})'
        return d.set(e)
    if m in ('mul', 'madd', 'msub', 'mneg'):
        d = R(ops[0]); x = R(ops[1]).r(); y = R(ops[2]).r()
        if m == 'mul':
            return d.set(f'{x} * {y}')
        if m == 'mneg':
            return d.set(f'0 - {x} * {y}')
        z = R(ops[3]).r()
        return d.set(f'{z} + {x} * {y}' if m == 'madd' else f'{z} - {x} * {y}')
    if m in ('smull', 'umull', 'smaddl', 'umaddl', 'smsubl', 'umsubl') and is_gp(ops[0]):
        d = R(ops[0]); s = m[0] == 's'
        cast = '(uint64_t)(int64_t)(int32_t)' if s else '(uint64_t)(uint32_t)'
        x = cast + R(ops[1]).r(); y = cast + R(ops[2]).r()
        if m.endswith('mull'):
            return d.set(f'{x} * {y}')
        z = R(ops[3]).r()
        return d.set(f'{z} + {x} * {y}' if 'add' in m else f'{z} - {x} * {y}')
    if m == 'umulh':
        return R(ops[0]).set(f'UMULH({R(ops[1]).r()}, {R(ops[2]).r()})')
    if m == 'smulh':
        return R(ops[0]).set(f'SMULH((int64_t){R(ops[1]).r()}, (int64_t){R(ops[2]).r()})')
    if m in ('udiv', 'sdiv'):
        d = R(ops[0]); x = R(ops[1]).r(); y = R(ops[2]).r()
        if m == 'udiv':
            return d.set(f'UDIV{d.w}({x}, {y})')
        return d.set(f'SDIV{d.w}((int{d.w}_t){x}, (int{d.w}_t){y})')
    if m in ('csel', 'csinc', 'csinv', 'csneg'):
        d = R(ops[0]); x = R(ops[1]).r(); y = R(ops[2]).r(); cc = cond(ops[3])
        alt = {'csel': y, 'csinc': f'{y} + 1', 'csinv': f'~{y}', 'csneg': f'0 - {y}'}[m]
        return d.set(f'{cc} ? {x} : ({alt})')
    if m in ('cset', 'csetm'):
        d = R(ops[0]); cc = cond(ops[1])
        return d.set(f'{cc} ? {"1ULL" if m == "cset" else "~0ULL"} : 0')
    if m in ('cinc', 'cinv', 'cneg'):
        d = R(ops[0]); x = R(ops[1]).r(); cc = cond(ops[2])
        alt = {'cinc': f'{x} + 1', 'cinv': f'~{x}', 'cneg': f'0 - {x}'}[m]
        return d.set(f'{cc} ? ({alt}) : {x}')
    if m in ('ccmp', 'ccmn'):
        x = R(ops[0]); w = x.w
        y = R(ops[1]).r() if not ops[1].startswith('#') else f'0x{imm(ops[1]):x}ULL'
        nzcv = imm(ops[2]); cc = cond(ops[3])
        f = 'SUB' if m == 'ccmp' else 'ADD'
        return [f'if ({cc}) FL_{f}{w}(c, {x.r()}, {y}); else FL_SET(c, {nzcv});']
    if m in ('sxtb', 'sxth', 'sxtw', 'uxtb', 'uxth'):
        d = R(ops[0]); ty, sgn = EXT[m]
        return d.set(f'(int64_t)({ty}){R(ops[1]).r()}' if sgn else f'({ty}){R(ops[1]).r()}')
    if m in ('ubfx', 'sbfx', 'ubfiz', 'sbfiz', 'bfi', 'bfxil'):
        d = R(ops[0]); s = R(ops[1]).r(); lsb = imm(ops[2]); wid = imm(ops[3]); w = d.w
        mask = (1 << wid) - 1
        if m == 'ubfx':
            return d.set(f'({s} >> {lsb}) & 0x{mask:x}ULL')
        if m == 'sbfx':
            return d.set(f'SEXT(({s} >> {lsb}) & 0x{mask:x}ULL, {wid})')
        if m == 'ubfiz':
            return d.set(f'({s} & 0x{mask:x}ULL) << {lsb}')
        if m == 'sbfiz':
            return d.set(f'(uint64_t)SEXT({s} & 0x{mask:x}ULL, {wid}) << {lsb}')
        if m == 'bfi':
            return d.set(f'({d.r()} & ~(0x{mask:x}ULL << {lsb})) | (({s} & 0x{mask:x}ULL) << {lsb})')
        if m == 'bfxil':
            return d.set(f'({d.r()} & ~0x{mask:x}ULL) | (({s} >> {lsb}) & 0x{mask:x}ULL)')
    if m == 'extr':
        d = R(ops[0]); hi = R(ops[1]).r(); lo = R(ops[2]).r(); lsb = imm(ops[3]); w = d.w
        if lsb == 0:
            return d.set(lo)
        if w == 64:
            return d.set(f'({lo} >> {lsb}) | ({hi} << {64 - lsb})')
        return d.set(f'(uint32_t)(((uint32_t){lo} >> {lsb}) | ((uint32_t){hi} << {32 - lsb}))')
    if m == 'clz':
        d = R(ops[0]); return d.set(f'CLZ{d.w}({R(ops[1]).r()})')
    if m == 'rbit':
        d = R(ops[0]); return d.set(f'RBIT{d.w}({R(ops[1]).r()})')
    if m == 'rev' and is_gp(ops[0]):
        d = R(ops[0]); return d.set(f'__builtin_bswap{d.w}({R(ops[1]).r()})')
    if m == 'rev16' and is_gp(ops[0]):
        d = R(ops[0]); x = R(ops[1]).r()
        mask = '0x00ff00ff00ff00ffULL' if d.w == 64 else '0x00ff00ffULL'
        return d.set(f'(({x} >> 8) & {mask}) | (({x} & {mask}) << 8)')

    # --- loads/stores (GP + FP)
    if m in LOADS or m in STORES or m in ('ldp', 'stp', 'ldpsw'):
        return ldst(m, ops)
    if m in ('ldar', 'ldarb', 'ldarh', 'ldaxr', 'ldaxrb', 'ldaxrh', 'ldxr', 'ldxrb', 'ldxrh',
             'stlr', 'stlrb', 'stlrh', 'stlxr', 'stlxrb', 'stlxrh', 'stxr', 'stxrb', 'stxrh'):
        return atomic(m, ops)

    # --- FP / SIMD
    return fpsimd(m, ops, ins)


def ty_of(reg_tok, suffix):
    if suffix == 'b':
        return 'uint8_t', 8
    if suffix == 'h':
        return 'uint16_t', 16
    return ('uint64_t', 64) if reg_tok[0] == 'x' else ('uint32_t', 32)


def ldst(m, ops):
    out = ['{']
    if m in ('ldp', 'stp', 'ldpsw'):
        pre, post = mem(ops, 2)
        out += pre
        r0, r1 = ops[0], ops[1]
        if FPS.match(r0):
            k, _ = fpreg(r0); sz = {'s': 4, 'd': 8, 'q': 16}[k]
            for j, r in enumerate((r0, r1)):
                _, i = fpreg(r)
                addr = f'_a + {j * sz}'
                if m == 'ldp':
                    out.append({4: f'SET_SU({i}, LD32({addr}));', 8: f'SET_DU({i}, LD64({addr}));', 16: f'c->v[{i}] = LD128({addr});'}[sz])
                else:
                    out.append({4: f'ST32({addr}, c->v[{i}].s[0]);', 8: f'ST64({addr}, c->v[{i}].d[0]);', 16: f'ST128({addr}, c->v[{i}]);'}[sz])
            # loads into same reg as base are fine since _a computed first
        else:
            a0, a1 = R(r0), R(r1); sz = 8 if a0.w == 64 else 4
            if m == 'ldpsw':
                out.append(f'uint64_t _v0 = (uint64_t)(int64_t)(int32_t)LD32(_a), _v1 = (uint64_t)(int64_t)(int32_t)LD32(_a + 4);')
                out += post
                out += a0.set('_v0') + a1.set('_v1')
            elif m == 'ldp':
                ld = 'LD64' if sz == 8 else 'LD32'
                out.append(f'uint64_t _v0 = {ld}(_a), _v1 = {ld}(_a + {sz});')
                out += post
                out += a0.set('_v0') + a1.set('_v1')
                out.append('}')
                return out
            else:
                st = 'ST64' if sz == 8 else 'ST32'
                out.append(f'{st}(_a, {a0.r()}); {st}(_a + {sz}, {a1.r()});')
        out += post + ['}']
        return out

    pre, post = mem(ops, 1)
    out += pre
    d = ops[0]
    if FPS.match(d):
        k, i = fpreg(d)
        if m.startswith('ld'):
            out.append({'b': f'SET_B({i}, LD8(_a));', 'h': f'SET_H({i}, LD16(_a));', 's': f'SET_SU({i}, LD32(_a));',
                        'd': f'SET_DU({i}, LD64(_a));', 'q': f'c->v[{i}] = LD128(_a);'}[k])
        else:
            out.append({'b': f'ST8(_a, c->v[{i}].b[0]);', 'h': f'ST16(_a, c->v[{i}].h[0]);', 's': f'ST32(_a, c->v[{i}].s[0]);',
                        'd': f'ST64(_a, c->v[{i}].d[0]);', 'q': f'ST128(_a, c->v[{i}]);'}[k])
        out += post + ['}']
        return out
    r = R(d)
    if m in LOADS:
        spec = LOADS[m]
        if spec is None:
            e = 'LD64(_a)' if r.w == 64 else 'LD32(_a)'
        else:
            ty, bits, sgn = spec
            ld = {8: 'LD8', 16: 'LD16', 32: 'LD32'}[bits]
            e = f'(uint64_t)(int64_t)({ty}){ld}(_a)' if sgn else f'{ld}(_a)'
        out.append(f'uint64_t _v = {e};')
        out += post
        out += r.set('_v')
    else:
        bits = STORES[m] or r.w
        st = {8: 'ST8', 16: 'ST16', 32: 'ST32', 64: 'ST64'}[bits]
        cast = {8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t', 64: 'uint64_t'}[bits]
        out.append(f'{st}(_a, ({cast}){r.r()});')
        out += post
    out.append('}')
    return out


def atomic(m, ops):
    sz = 'b' if m.endswith('b') else 'h' if m.endswith('h') else None
    if m.startswith('st') and 'x' in m[2:4] or m.startswith('stlx') or m.startswith('stx'):
        st = R(ops[0]); r = R(ops[1]); pre, _ = mem(ops, 2)
        ty, _ = ty_of(ops[1], sz)
        return ['{'] + pre + st.set(f'STX({ty}, _a, {r.r()})') + ['}']
    r = R(ops[0]); pre, _ = mem(ops, 1)
    ty, _ = ty_of(ops[0], sz)
    if m.startswith('ldx') or m.startswith('ldax'):
        return ['{'] + pre + r.set(f'LDX({ty}, _a)') + ['}']
    if m.startswith('ldar'):
        return ['{'] + pre + r.set(f'LDA({ty}, _a)') + ['}']
    if m.startswith('stlr'):
        return ['{'] + pre + [f'STL({ty}, _a, {r.r()});', '}']
    raise Unhandled('atomic ' + m)


# ---------------------------------------------------------------- FP / SIMD
def lanes(n, k):
    """(count, union field, signed field, bits, total bits)"""
    f, sf, bits, _ = LANE[k]
    return n, f, sf, bits, n * bits


def vloop(di, n, k, body, clear_hi=True):
    """Emit an element-wise loop. body uses i, and writes into _r."""
    cnt, f, sf, bits, tot = lanes(n, k)
    out = ['{', 'V128 _r = {0};', f'for (int i = 0; i < {cnt}; i++) {{ {body} }}', f'c->v[{di}] = _r;', '}']
    return out


FT = {'s': ('f', 'float'), 'd': ('fd', 'double')}


def fpsimd(m, ops, ins):
    v0 = ops[0] if ops else ''
    isvec = bool(VEC.match(v0))

    # ---------- scalar FP
    if not isvec and FPS.match(v0) and m in ('fadd', 'fsub', 'fmul', 'fdiv', 'fnmul', 'fmax', 'fmin', 'fmaxnm', 'fminnm', 'fabd') and not ELEM.match(ops[2]):
        x, k = fget(ops[1]); y, _ = fget(ops[2])
        sfx = 'F' if k == 's' else 'D'
        e = {'fadd': f'{x} + {y}', 'fsub': f'{x} - {y}', 'fmul': f'{x} * {y}', 'fdiv': f'{x} / {y}', 'fnmul': f'-({x} * {y})',
             'fmax': f'FMAX_{sfx}({x}, {y})', 'fmin': f'FMIN_{sfx}({x}, {y})', 'fmaxnm': f'FMAXNM_{sfx}({x}, {y})',
             'fminnm': f'FMINNM_{sfx}({x}, {y})', 'fabd': f'fabs{"f" if k == "s" else ""}({x} - {y})'}[m]
        return fset(v0, e)
    if not isvec and FPS.match(v0) and m in ('fmul', 'fmla', 'fmls') and ELEM.match(ops[2]):
        x, k = fget(ops[1]); vi, ek, li = elem(ops[2]); y = f'c->v[{vi}].{FT[k][0]}[{li}]'
        if m == 'fmul':
            return fset(v0, f'{x} * {y}')
        acc, _ = fget(v0)
        fn = 'fmaf' if k == 's' else 'fma'
        return fset(v0, f'{fn}({"-" if m == "fmls" else ""}{x}, {y}, {acc})')
    if m in ('fmadd', 'fmsub', 'fnmadd', 'fnmsub'):
        x, k = fget(ops[1]); y, _ = fget(ops[2]); z, _ = fget(ops[3])
        fn = 'fmaf' if k == 's' else 'fma'
        e = {'fmadd': f'{fn}({x}, {y}, {z})', 'fmsub': f'{fn}(-{x}, {y}, {z})',
             'fnmadd': f'{fn}(-{x}, {y}, -{z})', 'fnmsub': f'{fn}({x}, {y}, -{z})'}[m]
        return fset(v0, e)
    if not isvec and FPS.match(v0) and m in ('fabs', 'fneg', 'fsqrt', 'frintm', 'frintp', 'frintz', 'frinta', 'frintn', 'frintx', 'frinti') and FPS.match(ops[1]):
        x, k = fget(ops[1]); f = 'f' if k == 's' else ''
        e = {'fabs': f'fabs{f}({x})', 'fneg': f'-{x}', 'fsqrt': f'sqrt{f}({x})', 'frintm': f'floor{f}({x})',
             'frintp': f'ceil{f}({x})', 'frintz': f'trunc{f}({x})', 'frinta': f'round{f}({x})',
             'frintn': f'nearbyint{f}({x})', 'frintx': f'nearbyint{f}({x})', 'frinti': f'nearbyint{f}({x})'}[m]
        return fset(v0, e)
    if m == 'faddp' and FPS.match(v0) and VEC.match(ops[1]):
        k, i = fpreg(v0); si, n, ek = vec(ops[1]); ff = FT[k][0]
        return fset(v0, f'c->v[{si}].{ff}[0] + c->v[{si}].{ff}[1]')
    if m == 'fcvt':
        x, k = fget(ops[1])
        return fset(v0, f'({"double" if v0[0] == "d" else "float"}){x}')
    if m in ('fcmp', 'fcmpe'):
        x, k = fget(ops[0])
        y = '0.0' if ops[1].startswith('#') else fget(ops[1])[0]
        return [f'FL_FCMP(c, {x}, {y});']
    if m in ('fccmp', 'fccmpe'):
        x, _ = fget(ops[0]); y, _ = fget(ops[1]); nzcv = imm(ops[2]); cc = cond(ops[3])
        return [f'if ({cc}) FL_FCMP(c, {x}, {y}); else FL_SET(c, {nzcv});']
    if m == 'fcsel':
        x, k = fget(ops[1]); y, _ = fget(ops[2]); cc = cond(ops[3])
        return fset(v0, f'{cc} ? {x} : {y}')
    if m == 'fmov' and not isvec:
        if FPS.match(v0) and ops[1].startswith('#'):
            return fset(v0, repr(fimm(ops[1])))
        if FPS.match(v0) and is_gp(ops[1]):
            k, i = fpreg(v0); src = R(ops[1]).r()
            return [f'SET_SU({i}, (uint32_t){src});'] if k == 's' else [f'SET_DU({i}, {src});']
        if is_gp(v0) and FPS.match(ops[1]):
            k, i = fpreg(ops[1])
            return R(v0).set(f'c->v[{i}].s[0]' if k == 's' else f'c->v[{i}].d[0]')
        if FPS.match(v0) and FPS.match(ops[1]):
            k, i = fpreg(v0); _, j = fpreg(ops[1])
            return [f'SET_SU({i}, c->v[{j}].s[0]);'] if k == 's' else [f'SET_DU({i}, c->v[{j}].d[0]);']
    if m in ('scvtf', 'ucvtf') and not isvec:
        k, i = fpreg(v0); ty = FT[k][1]
        if is_gp(ops[1]):
            r = R(ops[1])
            src = (f'(int{r.w}_t){r.r()}' if m == 'scvtf' else f'(uint{r.w}_t){r.r()}')
        else:
            sk, si = fpreg(ops[1])
            src = (f'c->v[{si}].ss[0]' if sk == 's' else f'c->v[{si}].sd[0]') if m == 'scvtf' else (f'c->v[{si}].s[0]' if sk == 's' else f'c->v[{si}].d[0]')
        if len(ops) > 2:
            return fset(v0, f'({ty}){src} / (double)(1ULL << {imm(ops[2])})')
        return fset(v0, f'({ty}){src}')
    if m.startswith('fcvt') and m[4:6] in ('zs', 'zu', 'ms', 'mu', 'ps', 'pu', 'as', 'au') and is_gp(v0):
        d = R(v0); x, k = fget(ops[1]); sgn = m[5] == 's'
        rnd = {'z': '', 'm': 'M', 'p': 'P', 'a': 'A'}[m[4]]
        fn = f'FCVT{rnd}_{"S" if sgn else "U"}{d.w}'
        if len(ops) > 2:
            x = f'(double){x} * (double)(1ULL << {imm(ops[2])})'
        return d.set(f'{fn}({x})')

    # ---------- vector
    if m == 'movi':
        if FPS.match(v0):
            k, i = fpreg(v0)
            return [f'SET_DU({i}, 0x{int(ops[1].lstrip("#"), 16):x}ULL);']
        di, n, k = vec(v0)
        if ops[1].startswith('#') and re.fullmatch(r'#0+', ops[1]):
            return [f'c->v[{di}].d[0] = 0; c->v[{di}].d[1] = 0;']
        val = imm(ops[1])
        if len(ops) > 2:
            val <<= imm(ops[2].split('#')[1])
        cnt, f, sf, bits, tot = lanes(n, k)
        return vloop(di, n, k, f'_r.{f}[i] = 0x{val:x}ULL;')
    if m == 'fmov' and isvec:
        di, n, k = vec(v0); val = fimm(ops[1]); cnt, f, sf, bits, tot = lanes(n, k)
        fld = 'f' if k == 's' else 'fd'
        return vloop(di, n, k, f'_r.{fld}[i] = {val!r};')
    if m == 'dup':
        di, n, k = vec(v0); cnt, f, sf, bits, tot = lanes(n, k)
        if ELEM.match(ops[1]):
            si, ek, li = elem(ops[1])
            return ['{', f'uint64_t _e = c->v[{si}].{LANE[ek][0]}[{li}];'] + vloop(di, n, k, f'_r.{f}[i] = _e;') + ['}']
        return ['{', f'uint64_t _e = {R(ops[1]).r()};'] + vloop(di, n, k, f'_r.{f}[i] = _e;') + ['}']

    if isvec:
        di, n, k = vec(v0)
        cnt, f, sf, bits, tot = lanes(n, k)
        srcs = []
        for o in ops[1:]:
            if VEC.match(o):
                srcs.append(vec(o))
        # bitwise (byte arrangement)
        if m in ('and', 'orr', 'eor', 'bic', 'orn'):
            a_, b_ = srcs[0][0], srcs[1][0]
            e = {'and': 'x & y', 'orr': 'x | y', 'eor': 'x ^ y', 'bic': 'x & ~y', 'orn': 'x | ~y'}[m]
            body = f'uint64_t x = c->v[{a_}].d[i], y = c->v[{b_}].d[i]; _r.d[i] = {e};'
            return ['{', 'V128 _r = {0};', f'for (int i = 0; i < {tot // 64}; i++) {{ {body} }}', f'c->v[{di}] = _r;', '}']
        if m == 'bsl':
            a_, b_ = srcs[0][0], srcs[1][0]
            body = f'uint64_t s = c->v[{di}].d[i]; _r.d[i] = (c->v[{a_}].d[i] & s) | (c->v[{b_}].d[i] & ~s);'
            return ['{', 'V128 _r = {0};', f'for (int i = 0; i < {tot // 64}; i++) {{ {body} }}', f'c->v[{di}] = _r;', '}']
        if m in ('add', 'sub', 'mul', 'mla', 'cmeq') and len(srcs) == 2:
            a_, b_ = srcs[0][0], srcs[1][0]
            x, y = f'c->v[{a_}].{f}[i]', f'c->v[{b_}].{f}[i]'
            e = {'add': f'{x} + {y}', 'sub': f'{x} - {y}', 'mul': f'{x} * {y}', 'mla': f'c->v[{di}].{f}[i] + {x} * {y}',
                 'cmeq': f'({x} == {y}) ? ~0ULL : 0'}[m]
            return vloop(di, n, k, f'_r.{f}[i] = {e};')
        ff = 'f' if k == 's' else 'fd'
        if m in ('fadd', 'fsub', 'fmul', 'fdiv', 'fmax', 'fmin', 'fmaxnm', 'fminnm', 'fabd', 'fmla', 'fmls', 'frecps', 'frsqrts',
                 'fcmeq', 'fcmge', 'fcmgt', 'facge', 'facgt'):
            a_ = srcs[0][0]
            x = f'c->v[{a_}].{ff}[i]'
            if len(ops) > 2 and ELEM.match(ops[2]):
                vi, ek, li = elem(ops[2]); y = f'c->v[{vi}].{ff}[{li}]'
            elif len(ops) > 2 and ops[2].startswith('#'):
                y = '0.0'
            else:
                y = f'c->v[{srcs[1][0]}].{ff}[i]'
            sfx = 'F' if k == 's' else 'D'
            acc = f'c->v[{di}].{ff}[i]'
            ab = 'fabsf' if k == 's' else 'fabs'
            e = {'fadd': f'{x} + {y}', 'fsub': f'{x} - {y}', 'fmul': f'{x} * {y}', 'fdiv': f'{x} / {y}',
                 'fmax': f'FMAX_{sfx}({x}, {y})', 'fmin': f'FMIN_{sfx}({x}, {y})', 'fmaxnm': f'FMAXNM_{sfx}({x}, {y})',
                 'fminnm': f'FMINNM_{sfx}({x}, {y})', 'fabd': f'{ab}({x} - {y})', 'fmla': f'{"fmaf" if k == "s" else "fma"}({x}, {y}, {acc})',
                 'fmls': f'{"fmaf" if k == "s" else "fma"}(-{x}, {y}, {acc})', 'frecps': f'FRECPS_F({x}, {y})', 'frsqrts': f'FRSQRTS_F({x}, {y})'}.get(m)
            if e is not None:
                return vloop(di, n, k, f'_r.{ff}[i] = {e};')
            cmpe = {'fcmeq': f'{x} == {y}', 'fcmge': f'{x} >= {y}', 'fcmgt': f'{x} > {y}',
                    'facge': f'{ab}({x}) >= {ab}({y})', 'facgt': f'{ab}({x}) > {ab}({y})'}[m]
            return vloop(di, n, k, f'_r.{f}[i] = ({cmpe}) ? ~0ULL : 0;')
        if m in ('fcmlt', 'fcmle') and ops[2].startswith('#'):
            x = f'c->v[{srcs[0][0]}].{ff}[i]'
            return vloop(di, n, k, f'_r.{f}[i] = ({x} {"<" if m == "fcmlt" else "<="} 0.0) ? ~0ULL : 0;')
        if m in ('fabs', 'fneg', 'fsqrt', 'frecpe', 'frsqrte', 'frintm', 'frintp', 'frintz') and len(srcs) == 1:
            x = f'c->v[{srcs[0][0]}].{ff}[i]'; fsx = 'f' if k == 's' else ''
            e = {'fabs': f'fabs{fsx}({x})', 'fneg': f'-{x}', 'fsqrt': f'sqrt{fsx}({x})', 'frecpe': f'FRECPE_F({x})',
                 'frsqrte': f'FRSQRTE_F({x})', 'frintm': f'floor{fsx}({x})', 'frintp': f'ceil{fsx}({x})', 'frintz': f'trunc{fsx}({x})'}[m]
            return vloop(di, n, k, f'_r.{ff}[i] = {e};')
        if m in ('scvtf', 'ucvtf') and len(srcs) == 1:
            x = f'c->v[{srcs[0][0]}].{sf if m == "scvtf" else f}[i]'
            return vloop(di, n, k, f'_r.{ff}[i] = ({"float" if k == "s" else "double"}){x};')
        if m in ('fcvtzs', 'fcvtzu') and len(srcs) == 1:
            x = f'c->v[{srcs[0][0]}].{ff}[i]'
            if len(ops) > 2:
                x = f'(double){x} * (double)(1ULL << {imm(ops[2])})'
            fn = f'FCVT_{"S" if m == "fcvtzs" else "U"}{bits}'
            return vloop(di, n, k, f'_r.{f}[i] = {fn}({x});')
        if m == 'faddp' and len(srcs) == 2:
            a_, b_ = srcs[0][0], srcs[1][0]; half = cnt // 2
            body = f'V128 *s = i < {half} ? &c->v[{a_}] : &c->v[{b_}]; int j = (i % {half}) * 2; _r.{ff}[i] = s->{ff}[j] + s->{ff}[j + 1];'
            return ['{', 'V128 _a0 = c->v[%d], _b0 = c->v[%d];' % (a_, b_), 'V128 _r = {0};',
                    f'for (int i = 0; i < {cnt}; i++) {{ V128 *s = i < {half} ? &_a0 : &_b0; int j = (i % {half}) * 2; _r.{ff}[i] = s->{ff}[j] + s->{ff}[j + 1]; }}',
                    f'c->v[{di}] = _r;', '}']
        if m in ('zip1', 'zip2', 'uzp1', 'uzp2', 'trn1', 'trn2'):
            a_, b_ = srcs[0][0], srcs[1][0]; half = cnt // 2
            if m == 'zip1':
                body = f'_r.{f}[i] = (i & 1) ? B.{f}[i / 2] : A.{f}[i / 2];'
            elif m == 'zip2':
                body = f'_r.{f}[i] = (i & 1) ? B.{f}[{half} + i / 2] : A.{f}[{half} + i / 2];'
            elif m == 'uzp1':
                body = f'_r.{f}[i] = i < {half} ? A.{f}[2 * i] : B.{f}[2 * (i - {half})];'
            elif m == 'uzp2':
                body = f'_r.{f}[i] = i < {half} ? A.{f}[2 * i + 1] : B.{f}[2 * (i - {half}) + 1];'
            elif m == 'trn1':
                body = f'_r.{f}[i] = (i & 1) ? B.{f}[i - 1] : A.{f}[i];'
            else:
                body = f'_r.{f}[i] = (i & 1) ? B.{f}[i] : A.{f}[i + 1];'
            return ['{', f'V128 A = c->v[{a_}], B = c->v[{b_}], _r = {{0}};', f'for (int i = 0; i < {cnt}; i++) {{ {body} }}', f'c->v[{di}] = _r;', '}']
        if m == 'ext':
            a_, b_ = srcs[0][0], srcs[1][0]; sh = imm(ops[3]); nb = tot // 8
            return ['{', f'V128 A = c->v[{a_}], B = c->v[{b_}], _r = {{0}};',
                    f'for (int i = 0; i < {nb}; i++) {{ int j = i + {sh}; _r.b[i] = j < {nb} ? A.b[j] : B.b[j - {nb}]; }}', f'c->v[{di}] = _r;', '}']
        if m == 'rev64':
            a_ = srcs[0][0]; per = 64 // bits
            return vloop(di, n, k, f'_r.{f}[i] = c->v[{a_}].{f}[(i / {per}) * {per} + ({per} - 1 - i % {per})];')
        if m in ('shl', 'sshr', 'ushr') and ops[2].startswith('#'):
            a_ = srcs[0][0]; sh = imm(ops[2])
            if m == 'shl':
                return vloop(di, n, k, f'_r.{f}[i] = c->v[{a_}].{f}[i] << {sh};')
            if m == 'sshr':
                return vloop(di, n, k, f'_r.{f}[i] = c->v[{a_}].{sf}[i] >> {sh};')
            return vloop(di, n, k, f'_r.{f}[i] = c->v[{a_}].{f}[i] >> {sh};')
        if m in ('sshll', 'ushll', 'sxtl', 'uxtl'):
            si, sn, sk = srcs[0]; sh = imm(ops[2]) if len(ops) > 2 else 0
            sfld = LANE[sk][1] if m[0] == 's' else LANE[sk][0]
            return ['{', f'V128 A = c->v[{si}];'] + vloop(di, n, k, f'_r.{sf if m[0] == "s" else f}[i] = (int64_t)A.{sfld}[i] << {sh};') + ['}']
        if m == 'ssubl':
            a_, an, ak = srcs[0]; b_ = srcs[1][0]; s2 = LANE[ak][1]
            return ['{', f'V128 A = c->v[{a_}], B = c->v[{b_}];'] + vloop(di, n, k, f'_r.{sf}[i] = (int64_t)A.{s2}[i] - (int64_t)B.{s2}[i];') + ['}']
        if m in ('sqxtn', 'sqxtn2'):
            si, sn, sk = srcs[0]
            if (k, sk) != ('h', 's'):
                raise Unhandled('sqxtn form')
            if m == 'sqxtn':
                return ['{', f'V128 A = c->v[{si}], _r = {{0}};', 'for (int i = 0; i < 4; i++) _r.sh[i] = SAT_S16(A.ss[i]);', f'c->v[{di}] = _r;', '}']
            return ['{', f'V128 A = c->v[{si}];', f'for (int i = 0; i < 4; i++) c->v[{di}].sh[4 + i] = SAT_S16(A.ss[i]);', '}']
        if m == 'tbl':
            # tbl vd.Tb, {vn.16b[, ...]}, vm.Tb
            regs = re.findall(r'v(\d+)\.16b', ops[1]); idx = vec(ops[2])[0]; nt = len(regs)
            tabs = ', '.join(f'c->v[{r}]' for r in regs)
            return ['{', f'V128 T[{nt}] = {{ {tabs} }}; V128 I = c->v[{idx}], _r = {{0}};',
                    f'for (int i = 0; i < {tot // 8}; i++) {{ unsigned j = I.b[i]; _r.b[i] = j < {16 * nt} ? T[j / 16].b[j % 16] : 0; }}',
                    f'c->v[{di}] = _r;', '}']
        if m == 'mvn':
            pass

    # ---------- vector loads / stores
    if m in ('ld1', 'st1', 'ld1r', 'ld2', 'st2', 'ld4', 'st4'):
        return vldst(m, ops)

    raise Unhandled(m)


def _unh(x):
    raise Unhandled(x)


def vldst(m, ops):
    lst = ops[0]
    regs = re.findall(r'v(\d+)\.(\d*)([bhsd])', lst)
    lane = re.search(r'\}\[(\d+)\]', lst)
    pre, post = mem(ops, 1)
    out = ['{'] + pre
    nreg = len(regs)
    if m == 'ld1r':
        ri, n, k = int(regs[0][0]), int(regs[0][1]), regs[0][2]
        f, _, bits, ty = LANE[k]
        out.append(f'uint64_t _e = LD{bits}(_a);')
        out += vloop(ri, n, k, f'_r.{f}[i] = _e;')
        out += post + ['}']
        return out
    if lane:
        li = int(lane.group(1)); k = regs[0][2]; f, _, bits, ty = LANE[k]; sz = bits // 8
        for j, (ri, _, _) in enumerate(regs):
            if m.startswith('ld'):
                out.append(f'c->v[{ri}].{f}[{li}] = LD{bits}(_a + {j * sz});')
            else:
                out.append(f'ST{bits}(_a + {j * sz}, c->v[{ri}].{f}[{li}]);')
        out += post + ['}']
        return out
    n, k = int(regs[0][1]), regs[0][2]
    f, _, bits, ty = LANE[k]; sz = bits // 8; tot = n * bits // 8
    if m in ('ld1', 'st1'):
        for j, (ri, _, _) in enumerate(regs):
            if m == 'ld1':
                out.append(f'c->v[{ri}] = LD128(_a + {j * tot});' if tot == 16 else f'SET_DU({ri}, LD64(_a + {j * tot}));')
            else:
                out.append(f'ST128(_a + {j * tot}, c->v[{ri}]);' if tot == 16 else f'ST64(_a + {j * tot}, c->v[{ri}].d[0]);')
        out += post + ['}']
        return out
    # interleaved ld2/ld4/st2/st4
    ridx = [int(r[0]) for r in regs]
    if m.startswith('ld'):
        out.append(f'V128 _t[{nreg}] = {{{{0}}}};')
        out.append(f'for (int e = 0; e < {n}; e++) for (int r = 0; r < {nreg}; r++) _t[r].{f}[e] = LD{bits}(_a + (e * {nreg} + r) * {sz});')
        for j, ri in enumerate(ridx):
            out.append(f'c->v[{ri}] = _t[{j}];')
    else:
        out.append(f'V128 _t[{nreg}] = {{ {", ".join(f"c->v[{ri}]" for ri in ridx)} }};')
        out.append(f'for (int e = 0; e < {n}; e++) for (int r = 0; r < {nreg}; r++) ST{bits}(_a + (e * {nreg} + r) * {sz}, _t[r].{f}[e]);')
    out += post + ['}']
    return out
