/* Exclusive load/store pairs: the patterns the game uses (CAS loops, atomic add). */
#include "xrt.h"
#include <stdio.h>
#include <assert.h>
uint64_t G_BASE;
void xrt_dispatch(Ctx *c, uint64_t t) {} void xrt_trap(Ctx *c, uint64_t p, const char *w) {} void xrt_unimpl(Ctx *c, uint64_t p, const char *t) {}
int main(void) {
    Ctx ctx = {0}, *c = &ctx;
    uint64_t mem64 = 41; uint32_t mem32 = 7; uint8_t mem8 = 200;
    /* atomic increment: ldaxr / add / stlxr / cbnz */
    for (int i = 0; i < 3; i++) {
        uint64_t _a = (uint64_t)(uintptr_t)&mem64;
        c->x[10] = LDX(uint64_t, _a); c->x[10] += 1;
        c->x[11] = (uint32_t)STX(uint64_t, _a, c->x[10]);
        assert(c->x[11] == 0);
    }
    assert(mem64 == 44);
    /* store without matching load must fail */
    { uint64_t _a = (uint64_t)(uintptr_t)&mem32; c->excl_addr = 0; assert(STX(uint32_t, _a, 5) == 1 && mem32 == 7); }
    /* interference: memory changed between ldxr and stxr -> fail */
    { uint64_t _a = (uint64_t)(uintptr_t)&mem32; uint64_t v = LDX(uint32_t, _a); mem32 = 9; assert(STX(uint32_t, _a, v + 1) == 1 && mem32 == 9); }
    /* byte form */
    { uint64_t _a = (uint64_t)(uintptr_t)&mem8; uint64_t v = LDX(uint8_t, _a); assert(v == 200); assert(STX(uint8_t, _a, 201) == 0 && mem8 == 201); }
    puts("exclusive tests: OK");
    return 0;
}
