// runtime/simd_fp.h vslow: the vector NaN fix (ADD, SUB, MUL, NMUL, DIV, ABD, SQRT under round-to-nearest) against
// the lane-by-lane path it replaces (op_s / op_d per lane, which tests/diff/simd.py checks against Unicorn). Operands
// mix NaNs (quiet, signalling, both signs, payloads), infinities, zeros, denormals, the smallest normal and random
// values, with FPCR.DN and FPCR.FZ each on and off. Built and run by tests/nanvec_check.py; exits 1 on a difference.
#include <stdio.h>

#include "simd.h"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) {
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return rng;
}

static const uint32_t ES[] = {0x7fc00000, 0xffc00001, 0x7f800001, 0xff812345, 0x7fbfffff, 0x7f800000, 0xff800000, 0,
                              0x80000000, 0x00000001, 0x807fffff, 0x00800000, 0x80800000, 0x3f800000, 0xbf800000,
                              0x7f7fffff, 0x00400000};
static const uint64_t ED[] = {0x7ff8000000000000, 0xfff8000000000001, 0x7ff0000000000001, 0xfff0123456789abc,
                              0x7ff7ffffffffffff, 0x7ff0000000000000, 0xfff0000000000000, 0, 0x8000000000000000,
                              1, 0x800fffffffffffff, 0x0010000000000000, 0x8010000000000000, 0x3ff0000000000000,
                              0xbff0000000000000, 0x7fefffffffffffff, 0x0008000000000000};
#define NE (int)(sizeof ES / sizeof ES[0])
static const int OPS[] = {FOP_ADD, FOP_SUB, FOP_MUL, FOP_NMUL, FOP_DIV, FOP_ABD, FOP_SQRT};

static uint32_t pick_s(void) { return next() & 1 ? ES[next() % NE] : (uint32_t)next(); }
static uint64_t pick_d(void) { return next() & 1 ? ED[next() % NE] : next(); }

static long checks, fails;
static void bad(const char *p, int op, uint64_t fpcr, int lane, uint64_t a, uint64_t b, uint64_t got, uint64_t want) {
    if (fails++ < 20)
        printf("%s op %d fpcr %#llx lane %d a %llx b %llx: got %llx want %llx\n", p, op, (unsigned long long)fpcr, lane,
               (unsigned long long)a, (unsigned long long)b, (unsigned long long)got, (unsigned long long)want);
}

static void one_s(Cpu *c, int op, vu32 a, vu32 b) {
    vf32 got = vslow_s(c, op, (vf32)a, (vf32)b, (vf32){0});
    for (int i = 0; i < 4; i++) {
        float w = op_s(c, op, ((vf32)a)[i], ((vf32)b)[i], 0);
        uint32_t gu = ((vu32)got)[i], wu;
        memcpy(&wu, &w, 4);
        checks++;
        if (gu != wu) bad("s", op, c->fpcr, i, a[i], b[i], gu, wu);
    }
}
static void one_d(Cpu *c, int op, vu64 a, vu64 b) {
    vf64 got = vslow_d(c, op, (vf64)a, (vf64)b, (vf64){0});
    for (int i = 0; i < 2; i++) {
        double w = op_d(c, op, ((vf64)a)[i], ((vf64)b)[i], 0);
        uint64_t gu = ((vu64)got)[i], wu;
        memcpy(&wu, &w, 8);
        checks++;
        if (gu != wu) bad("d", op, c->fpcr, i, a[i], b[i], gu, wu);
    }
}

int main(void) {
    for (int mode = 0; mode < 4; mode++) {
        Cpu c = {.fpcr = (mode & 1 ? FPCR_DN : 0) | (mode & 2 ? FPCR_FZ : 0)};
        rt_fpcr_sync(&c);
        for (int k = 0; k < (int)(sizeof OPS / sizeof *OPS); k++) {
            int op = OPS[k];
            for (int i = 0; i < NE; i++)   // every pair of special values, in both lanes of a mixed vector
                for (int j = 0; j < NE; j++) {
                    one_s(&c, op, (vu32){ES[i], 0x3f800000, ES[j], (uint32_t)next()}, (vu32){ES[j], ES[i], 0x40000000, ES[i]});
                    one_d(&c, op, (vu64){ED[i], ED[j]}, (vu64){ED[j], 0x4000000000000000});
                }
            for (int n = 0; n < 200000; n++) {
                one_s(&c, op, (vu32){pick_s(), pick_s(), pick_s(), pick_s()}, (vu32){pick_s(), pick_s(), pick_s(), pick_s()});
                one_d(&c, op, (vu64){pick_d(), pick_d()}, (vu64){pick_d(), pick_d()});
            }
        }
    }
    printf("nanvec_check: %ld checks, %ld differences\n", checks, fails);
    return fails != 0;
}
