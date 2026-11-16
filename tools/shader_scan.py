#!/usr/bin/env python3
"""Survey the shaders captured with XRT_DUMP_SHADERS=1 (game/shaders/*.code + *.ctrl).

    python3 tools/shader_scan.py game/shaders

Reports stage counts (from the Maxwell shader program header), program sizes and a histogram of
instruction classes (top 13 bits of each 64-bit instruction), which sizes the M3.5 translator work.
Code layout: 0x30-byte NVN header (magic 0x12345678) + 0x50-byte SPH (graphics stages only) + code,
code = groups of 4 x u64 (scheduling word, then 3 instructions).
"""
import collections, glob, os, struct, sys

STAGES = {1: 'vertex', 2: 'tess-ctrl', 3: 'tess-eval', 4: 'geometry', 5: 'pixel'}
END = 0xE2400FFFFF87000F          # BRA to self, placed after the final EXIT


def program(code):
    """-> (stage, list of instruction words) for one captured .code blob"""
    if len(code) < 0x80 or struct.unpack_from('<I', code, 0)[0] != 0x12345678:
        return None, []
    sph0 = struct.unpack_from('<I', code, 0x30)[0]
    stage = STAGES.get((sph0 >> 10) & 0xF, 'compute?') if (sph0 & 0x1F) in (1, 2) else 'compute'
    start = 0x80 if stage != 'compute' else 0x30
    words = []
    for off in range(start, len(code) - 7, 8):
        w = struct.unpack_from('<Q', code, off)[0]
        if (off - start) % 32 == 0:
            continue                # scheduling word
        words.append(w)
        if w == END:
            break
    return stage, words


def main(d):
    stages = collections.Counter(); sizes = []; hist = collections.Counter(); truncated = 0
    files = sorted(glob.glob(os.path.join(d, '*.code')))
    for f in files:
        stage, words = program(open(f, 'rb').read())
        if stage is None:
            stages['(no NVN header)'] += 1; continue
        stages[stage] += 1
        if not words or words[-1] != END:
            truncated += 1
        sizes.append(len(words))
        for w in words:
            hist[w >> 51] += 1        # top 13 bits ~ Maxwell opcode class
    print(f'{len(files)} shaders: ' + ', '.join(f'{k} {v}' for k, v in stages.most_common()))
    if sizes:
        sizes.sort()
        print(f'instructions per shader: median {sizes[len(sizes)//2]}, max {sizes[-1]}, total {sum(sizes)}'
              f' ({truncated} without an end marker)')
    print(f'{len(hist)} distinct opcode classes (top 13 bits); most common:')
    for k, v in hist.most_common(40):
        print(f'  {k << 3:04x}..  {v}')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'game/shaders')
