// Recall / Ultrahand target guard (recomp/patches-v143.txt, set ability; after UltraTears pull request #52 by
// mgiampapa, kuraeow/ultratears src/gfx/nvn/hle_nvn.c totk_ability_target_ok, GPL-3.0).
// The patched site loads the request's result record (x8 = [x19 + 0x6b0]); the game then reads the target actor from
// it. Here the record is handed back as 0 when the actor in it fails the game's own live-actor test (the one
// main+0x155ec relies on: its link record [actor + 0x1a8] points back to it at +0x40 and carries its id at +0x48 ==
// [actor + 0x10]), so the game's cbz x8 skips the target for that frame. A null or tagged actor goes on as before.
// Every pointer is checked to be 8-aligned and readable before it is read. Only x8 changes.
#include <stdint.h>
#include "cpu.h"
#include "dispatch.h"
#include "host.h"
#include "platform/platform.h"
#if __has_include("patches.h")
#include "patches.h"
#endif

#ifdef RT_PATCH_PAGE
#define HOST_ABILITY (RT_PATCH_PAGE + 0xf60)   // below the mouse hooks, which start at +0xf70

// Our guest heap lies below 0x800000000 (memory.c LOW_SEARCH_FROM), so UltraTears' window bounds would reject live
// actors; the pages are asked instead (a system call, on a path that runs a few times a minute).
static int guest_ptr(uint64_t p, uint64_t span) { return p >= 0x10000 && !(p & 7) && plat_mem_readable(p, span); }
static uint64_t rd64(uint64_t p) { return *(volatile uint64_t *)(uintptr_t)p; }
static uint32_t rd32(uint64_t p) { return *(volatile uint32_t *)(uintptr_t)p; }

static const char *stale(uint64_t actor) {   // NULL: a live actor
    if (!guest_ptr(actor, 0x230)) return "actor address";
    uint64_t link = rd64(actor + 0x1a8), comp, sub;
    if (!guest_ptr(link, 0x50)) return "link record address";
    if (rd64(link + 0x40) != actor) return "link record does not point back";
    if (rd32(link + 0x48) != rd32(actor + 0x10)) return "actor id";
    if ((comp = rd64(actor + 0x228)) && !guest_ptr(comp, 0xb8)) return "component address";
    if (comp && (sub = rd64(comp + 0xb0)) && !guest_ptr(sub, 0x98)) return "component target address";
    return NULL;
}

static void host_ability(Cpu *c) {
    static volatile uint64_t checked, skipped;
    uint64_t rec = rd64(c->x[19] + 0x6b0);   // the original ldr x8, [x19, #0x6b0]
    c->x[8] = rec;
    if (!rec || !guest_ptr(rec, 8)) return;   // null: the game skips it; else its own load faults as before
    uint64_t actor = rd64(rec);
    if (!actor || (actor & 1)) return;   // the game skips these itself
    uint64_t k = __atomic_add_fetch(&checked, 1, __ATOMIC_RELAXED);
    const char *why = stale(actor);
    if (!why) {
        if (k == 1) rt_log("ability target: the first one checked is live (TOTK_ABILITY_GUARD=0: no test)");
        return;
    }
    c->x[8] = 0;
    uint64_t n = __atomic_add_fetch(&skipped, 1, __ATOMIC_RELAXED);
    if (n <= 20 || !(n & (n - 1)))
        rt_log("ability target: stale one skipped (%s): owner %#llx record %#llx actor %#llx; %llu skipped of %llu", why,
               (unsigned long long)c->x[19], (unsigned long long)rec, (unsigned long long)actor,
               (unsigned long long)n, (unsigned long long)k);
}

__attribute__((constructor)) static void ability_register(void) { rt_register_host(HOST_ABILITY, host_ability); }
#endif
