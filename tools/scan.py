"""Scope scan of the guest executable: function starts (eh_frame + BL targets), instruction mix."""
import struct, collections, json, capstone, sys, time
ELF = sys.argv[1] if len(sys.argv) > 1 else 'game/main.elf'
d = open(ELF, 'rb').read(); img = d[0x1000:]
phoff = struct.unpack_from('<Q', d, 32)[0]
TEXT_END = struct.unpack_from('<IIQQQQQQ', d, phoff)[5]
u32 = lambda o: struct.unpack_from('<I', img, o)[0]
mod0 = u32(4)
eh_s = mod0 + struct.unpack_from('<i', img, mod0 + 16)[0]
fde_count = struct.unpack_from('<i', img, eh_s + 8)[0]
funcs = sorted({eh_s + struct.unpack_from('<i', img, eh_s + 12 + i * 8)[0] for i in range(fde_count)})
md = capstone.Cs(capstone.CS_ARCH_ARM64 if hasattr(capstone, 'CS_ARCH_ARM64') else capstone.CS_ARCH_AARCH64, capstone.CS_MODE_ARM)
md.skipdata = True
mn = collections.Counter(); bl = set(); n = 0; bad = 0; t = time.time()
for addr, size, m, ops in md.disasm_lite(img[:TEXT_END], 0):
    n += 1; mn[m] += 1
    if m == 'bl': bl.add(int(ops.lstrip('#'), 16))
    elif m == '.byte': bad += 1
pick = lambda pre: {k: v for k, v in mn.items() if any(k.startswith(p) for p in pre)}
out = {'text_size': hex(TEXT_END), 'insns': n, 'undecodable_words': bad, 'fde_functions': len(funcs),
       'bl_targets': len(bl), 'bl_targets_not_in_fde': len(bl - set(funcs)), 'distinct_mnemonics': len(mn),
       'indirect': pick(['br', 'blr', 'ret']), 'system': pick(['svc', 'mrs', 'msr', 'sys', 'dmb', 'dsb', 'isb', 'hint', 'yield', 'wfe', 'sev', 'clrex', 'brk', 'hlt']),
       'atomics': pick(['ldxr', 'ldaxr', 'stxr', 'stlxr', 'ldar', 'stlr', 'cas', 'ldadd', 'swp', 'ldxp', 'stxp', 'ldaxp', 'stlxp', 'ldapr']),
       'seconds': round(time.time() - t), 'mnemonics': mn.most_common()}
json.dump(out, open('analysis/scan.json', 'w'), indent=1)
json.dump(sorted(set(funcs) | bl), open('analysis/func_starts.json', 'w'))
print({k: v for k, v in out.items() if k != 'mnemonics'})
