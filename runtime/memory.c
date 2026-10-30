// Adapted from MacNativeGames games/totk/runtime/services.c and loader.c @ b75be8c (MIT, Copyright (c) 2026
// MacNativeGames contributors), and LAS Recomp runtime/main.c and platform/windows.c @ da35b83 (MIT, Copyright (c)
// 2026 Peiikai).
// Guest address space and the memory SVCs: heap, module data permissions, stack moves,
// QueryMemory, physical memory in the alias region, GetInfo memory ids, and the kernel-region allocator.
//
// Every region is a plain reservation; pages are committed and decommitted at 4 KiB granularity. Decommitted pages
// come back zeroed, so no stale-page bookkeeping is needed. The stack and heap block and the alias region are
// reserved with MEM_WRITE_WATCH: mem_written_pages reports the pages the CPU wrote, which
// the Vulkan back end uploads again.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"

#define PAGE 0x1000ull
#define GiB (1ull << 30)
#define LOW_SEARCH_FROM 0x100000000ull
#define LOW_SIZE (6 * GiB)
#define STACK_OFF 0x10000000ull
#define STACK_SIZE 0x10000000ull
#define HEAP_OFF (STACK_OFF + STACK_SIZE + 0x10000000ull)
#define HEAP_CAP (4 * GiB)
#define ALIAS_SIZE (64 * GiB)
#define ALIAS_ALIGN (4 * GiB)
#define ALIAS_PAGES (ALIAS_SIZE / PAGE)
#define DEFAULT_POOL 0xCD500000ull       // retail application pool
#define SYSTEM_RESOURCE_BUDGET (32ull << 20)
#define MAX_STACK_MAPS 4096
#define REEXEC_LIMIT 20

// Horizon memory states, permissions, attributes and results used here.
enum { ST_FREE = 0, ST_CODE = 3, ST_CODEDATA = 4, ST_NORMAL = 5, ST_STACK = 0xB, ST_THREADLOCAL = 0xC,
       ST_INACCESSIBLE = 0x10 };
enum { PERM_NONE = 0, PERM_R = 1, PERM_RW = 3, PERM_RX = 5 };
enum { ATTR_PERMISSION_LOCKED = 16 };
#define R_OK 0u
#define R_INVALID_ADDRESS 0xcc01u   // misaligned address
#define R_INVALID_SIZE 0xca01u
#define R_INVALID_NEW_PERM 0xd801u
#define R_INVALID_STATE 0xd401u     // InvalidCurrentMemory
#define R_INVALID_REGION 0xdc01u
#define R_LIMIT_REACHED 0x10801u

static RtGuestLayout L;
static uint64_t mods_lo, mods_hi;   // fixed module window
static SRWLOCK lock = SRWLOCK_INIT;
static uint64_t heap_size, alias_bytes;
static uint8_t *alias_pages;        // 1 per mapped 4 KiB page of the alias region
typedef struct { uint64_t dst, src, size; } StackMap;
static StackMap stack_maps[MAX_STACK_MAPS];
static unsigned nstack_maps;
static uint8_t *module_perm[MOD_COUNT];   // per data page of each module: perm | ATTR_PERMISSION_LOCKED

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static void *reserve_at(uint64_t at, uint64_t size, DWORD extra) {
    void *p = VirtualAlloc((void *)(uintptr_t)at, size, MEM_RESERVE | extra, PAGE_NOACCESS);
    if (p && p != (void *)(uintptr_t)at) {
        VirtualFree(p, 0, MEM_RELEASE);
        p = NULL;
    }
    return p;
}

// ---- frozen regions
// A 2 MiB region of the watched reservations that nothing wrote for a while can be made read-only (mem_freeze), so the
// back end's write-watch queries skip it: no page of a frozen region changes before a thaw. The first write faults and
// thaw_handler makes the region writable again before the write retries. A kernel write into guest memory (a file
// read) fails on a read-only page instead of faulting, so file reads thaw their buffer first (mem_thaw), and so does
// every commit, decommit and protection change here. A region stays thawed for FRZ_HOLD_MS after a mem_thaw: a file
// read still in flight writes its buffer later. frz_lock is a leaf lock: nothing writes guest memory under it.
#define FRZ_SHIFT 21
#define FRZ_SIZE (1ull << FRZ_SHIFT)
#define FRZ_HOLD_MS 5000u
static volatile uint8_t frz[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static uint32_t thaw_ms[RT_ADDRESS_LIMIT >> FRZ_SHIFT];   // frz_lock: GetTickCount of the last thaw request, 0 never
static volatile LONG nfrz;
static int dirty_hash;   // TOTK_DIRTY: imported pages are tracked by hashes (mem_dirty_track), set by mem_init
static int no_watch;     // TOTK_WRITE_WATCH off: the reservations have no write watch; untracked pages read as unwritten
static SRWLOCK frz_lock = SRWLOCK_INIT;
static volatile LONG64 frz_count[4];   // freezes; thaws by a write fault, by mem_thaw, by a mapping change

static int watched(uint64_t addr, uint64_t size) {
    const RtGuestLayout *g = &L;
    return (addr >= g->aslr.base && size <= LOW_SIZE && addr - g->aslr.base <= LOW_SIZE - size) ||
           (addr >= g->alias.base && size <= ALIAS_SIZE && addr - g->alias.base <= ALIAS_SIZE - size);
}

// frz_lock held exclusively. why: 1 a write fault, 2 mem_thaw (holds the region thawed), 3 a mapping change here.
static void thaw_locked(uint64_t r, int why) {
    if (why == 2) thaw_ms[r] = (uint32_t)GetTickCount64() | 1;
    if (!frz[r]) return;
    DWORD old;
    uint64_t a = r << FRZ_SHIFT, end = a + FRZ_SIZE;
    if (!VirtualProtect((void *)(uintptr_t)a, FRZ_SIZE, PAGE_READWRITE, &old)) {
        // Not all committed (freeze_locked left reserved holes): the read-only runs only. Every committed page of a
        // frozen region was read-write at the freeze, and mapping changes thaw before they run, so these are all ours.
        MEMORY_BASIC_INFORMATION mbi;
        while (a < end && VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) {
            uint64_t b = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (b > end) b = end;
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READONLY &&
                !VirtualProtect((void *)(uintptr_t)a, b - a, PAGE_READWRITE, &old))
                rt_fatal("memory: thawing %#llx+%#llx failed (error %lu)", (unsigned long long)a,
                         (unsigned long long)(b - a), GetLastError());
            a = b;
        }
    }
    __atomic_store_n(&frz[r], 0, __ATOMIC_RELEASE);
    InterlockedDecrement(&nfrz);
    InterlockedIncrement64(&frz_count[why]);
}

// frz_lock held exclusively.
static void thaw_range_locked(uint64_t a, uint64_t n, int why) {
    if (!n || a >= RT_ADDRESS_LIMIT) return;
    uint64_t r0 = a >> FRZ_SHIFT, r1 = (a + n - 1 < RT_ADDRESS_LIMIT ? a + n - 1 : RT_ADDRESS_LIMIT - 1) >> FRZ_SHIFT;
    for (uint64_t r = r0; r <= r1; r++) thaw_locked(r, why);
}

void mem_thaw(uint64_t a, uint64_t n) {
    AcquireSRWLockExclusive(&frz_lock);   // also when nothing is frozen: the hold keeps a read's buffer writable
    thaw_range_locked(a, n, 2);
    ReleaseSRWLockExclusive(&frz_lock);
}

// frz_lock held exclusively. Makes the committed pages of region r read-only when every one of them is read-write;
// reserved holes (heap pages given back) stay as they are, and thaw_locked finds the read-only runs again.
static int freeze_locked(uint64_t r) {
    uint64_t a = r << FRZ_SHIFT, end = a + FRZ_SIZE, run[16][2];
    int n = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uint64_t x = a; x < end; ) {
        if (!VirtualQuery((void *)(uintptr_t)x, &mbi, sizeof mbi)) return 0;
        uint64_t y = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (y > end) y = end;
        if (mbi.State == MEM_COMMIT) {
            if (mbi.Protect != PAGE_READWRITE || n == 16) return 0;
            run[n][0] = x, run[n][1] = y, n++;
        } else if (mbi.State != MEM_RESERVE) return 0;
        x = y;
    }
    DWORD old;
    for (int i = 0; i < n; i++)
        if (!VirtualProtect((void *)(uintptr_t)run[i][0], run[i][1] - run[i][0], PAGE_READONLY, &old)) {
            while (i--) VirtualProtect((void *)(uintptr_t)run[i][0], run[i][1] - run[i][0], PAGE_READWRITE, &old);
            return 0;
        }
    return n > 0;
}

int mem_freeze(uint64_t a) {
    if ((a & (FRZ_SIZE - 1)) || !watched(a, FRZ_SIZE)) return 0;
    uint64_t r = a >> FRZ_SHIFT;
    int ok = 0;
    AcquireSRWLockExclusive(&frz_lock);
    if (frz[r]) ok = 1;
    else if (thaw_ms[r] && (int32_t)((uint32_t)GetTickCount64() - thaw_ms[r]) < (int32_t)FRZ_HOLD_MS) ok = 0;   // |1 may be ahead
    else if (freeze_locked(r)) {
        __atomic_store_n(&frz[r], 1, __ATOMIC_RELEASE);
        InterlockedIncrement(&nfrz);
        InterlockedIncrement64(&frz_count[0]);
        ok = 1;
    }
    ReleaseSRWLockExclusive(&frz_lock);
    return ok;
}

int mem_frozen(uint64_t a) { return a < RT_ADDRESS_LIMIT && __atomic_load_n(&frz[a >> FRZ_SHIFT], __ATOMIC_ACQUIRE); }

void mem_freeze_stats(uint64_t *frozen, uint64_t *freezes, uint64_t *fault_thaws, uint64_t *other_thaws) {
    *frozen = (uint64_t)__atomic_load_n(&nfrz, __ATOMIC_RELAXED);
    *freezes = (uint64_t)InterlockedExchange64(&frz_count[0], 0);
    *fault_thaws = (uint64_t)InterlockedExchange64(&frz_count[1], 0);
    *other_thaws = (uint64_t)InterlockedExchange64(&frz_count[2], 0) + (uint64_t)InterlockedExchange64(&frz_count[3], 0);
}

// The first write into a frozen region thaws it and retries. A fault in a region another thread thawed meanwhile
// retries too; any other fault goes on to the crash handler.
static LONG CALLBACK thaw_handler(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || er->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    uint64_t a = er->ExceptionInformation[1];
    if (a >= RT_ADDRESS_LIMIT || !watched(a, 1)) return EXCEPTION_CONTINUE_SEARCH;
    AcquireSRWLockExclusive(&frz_lock);
    int was = frz[a >> FRZ_SHIFT];
    thaw_locked(a >> FRZ_SHIFT, 1);
    ReleaseSRWLockExclusive(&frz_lock);
    if (was) return EXCEPTION_CONTINUE_EXECUTION;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE)
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

// Mapping changes thaw the regions they touch and run under frz_lock, so no freeze sees them half done.
static void commit(uint64_t a, uint64_t n, const char *what) {
    AcquireSRWLockExclusive(&frz_lock);
    thaw_range_locked(a, n, 3);
    int ok = !n || VirtualAlloc((void *)(uintptr_t)a, n, MEM_COMMIT, PAGE_READWRITE);
    ReleaseSRWLockExclusive(&frz_lock);
    if (!ok)
        rt_fatal("memory: commit %s %#llx+%#llx failed (error %lu)", what, (unsigned long long)a,
                 (unsigned long long)n, GetLastError());
}

static void decommit(uint64_t a, uint64_t n, const char *what) {
    AcquireSRWLockExclusive(&frz_lock);
    thaw_range_locked(a, n, 3);
    int ok = !n || VirtualFree((void *)(uintptr_t)a, n, MEM_DECOMMIT);
    ReleaseSRWLockExclusive(&frz_lock);
    if (!ok)
        rt_fatal("memory: decommit %s %#llx+%#llx failed (error %lu)", what, (unsigned long long)a,
                 (unsigned long long)n, GetLastError());
}

static void protect(uint64_t a, uint64_t n, DWORD prot, const char *what) {
    DWORD old;
    AcquireSRWLockExclusive(&frz_lock);
    thaw_range_locked(a, n, 3);
    int ok = !n || VirtualProtect((void *)(uintptr_t)a, n, prot, &old);
    ReleaseSRWLockExclusive(&frz_lock);
    if (!ok)
        rt_fatal("memory: protect %s %#llx+%#llx failed (error %lu)", what, (unsigned long long)a,
                 (unsigned long long)n, GetLastError());
}

// The process is re-run when a region is taken: Windows places DLLs, heaps and thread stacks at random addresses,
// and a fresh process gets a fresh layout. The parent waits and exits with the child's code.
static void reexec_or_fail(const char *what) {
    const char *e = cfg_get("TOTK_REEXEC");
    int n = e ? atoi(e) : 0;
    if (n >= REEXEC_LIMIT) rt_fatal("memory: %s is not free after %d attempts", what, n);
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    SetEnvironmentVariableA("TOTK_REEXEC", buf);
    fflush(stdout);
    fflush(stderr);
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    wchar_t *cmd = _wcsdup(GetCommandLineW());   // CreateProcessW may write its command line
    if (!cmd || !CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        rt_fatal("memory: %s is not free and re-running the process failed (error %lu)", what, GetLastError());
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 70;
    GetExitCodeProcess(pi.hProcess, &code);
    ExitProcess(code);
}

void mem_init(void) {
    if (L.pool) return;
    // Fixed window: modules, kernel region, host-function addresses.
    mods_lo = MOD_MAIN_BASE;
    mods_hi = align_up(rt_layout[rt_layout_count - 1].base + rt_layout[rt_layout_count - 1].size, 0x10000);
    if (mods_hi > RT_KERNEL_BASE) rt_fatal("memory: modules end at %#llx, inside the kernel region",
                                           (unsigned long long)mods_hi);
    if (!reserve_at(mods_lo, mods_hi - mods_lo, 0)) reexec_or_fail("the module window");
    if (!reserve_at(RT_KERNEL_BASE, RT_KERNEL_SIZE, 0)) reexec_or_fail("the kernel region");
    if (!reserve_at(RT_HOSTFN_BASE, RT_HOSTFN_SIZE, 0)) reexec_or_fail("the host-function window");

    // Wine registers a MEM_WRITE_WATCH reservation for userfaultfd write protection and marks it VM_NOHUGEPAGE, so its
    // memory never gets transparent huge pages, and amdgpu revalidates every imported page of it one PTE at a time at
    // every command submission (hmm_range_fault in amdgpu_cs_ioctl: 25-30 ms a frame on a Z1). TOTK_WRITE_WATCH
    // (environment only: it is read before totk.ini) off reserves without the watch; auto does so under Wine unless
    // TOTK_DIRTY=writewatch. The hash tracker then covers every imported page, the only memory the GPU and the texture
    // paths read.
    const char *(*wine_version)(void) =
        (const char *(*)(void))(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
    char ww[16] = "", dd[16] = "";
    GetEnvironmentVariableA("TOTK_WRITE_WATCH", ww, sizeof ww);
    GetEnvironmentVariableA("TOTK_DIRTY", dd, sizeof dd);
    no_watch = !strcmp(ww, "off") || (strcmp(ww, "on") && wine_version && strcmp(dd, "writewatch"));
    DWORD wflag = no_watch ? 0 : MEM_WRITE_WATCH;
    // Stack and heap block, then the alias region above it, both searched below the module window.
    uint64_t low = 0, alias = 0;
    for (uint64_t at = LOW_SEARCH_FROM; at + LOW_SIZE <= mods_lo && !low; at += GiB)
        if (reserve_at(at, LOW_SIZE, wflag)) low = at;
    if (!low) reexec_or_fail("a 6 GiB stack and heap block below 2^39");
    for (uint64_t at = align_up(low + LOW_SIZE, ALIAS_ALIGN); at + ALIAS_SIZE <= mods_lo && !alias; at += ALIAS_ALIGN)
        if (reserve_at(at, ALIAS_SIZE, wflag)) alias = at;
    if (!alias) reexec_or_fail("a 64 GiB alias region below 2^39");

    AddVectoredExceptionHandler(1, thaw_handler);   // before the crash handler
    alias_pages = VirtualAlloc(NULL, ALIAS_PAGES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!alias_pages) rt_fatal("memory: cannot allocate the alias page map");
    // totk.ini is read here, once the address space is claimed and before the first setting: the pool size below
    // depends on TOTK_APP_POOL and on the resolution settings (resolution.c decides them once, on this first call).
    cfg_init();
    const char *pool = cfg_get("TOTK_APP_POOL");
    L = (RtGuestLayout){
        .aslr = {low, RT_ADDRESS_LIMIT - low},
        .stack = {low + STACK_OFF, STACK_SIZE},
        .heap = {low + HEAP_OFF, HEAP_CAP},
        .alias = {alias, ALIAS_SIZE},
        .kernel = {RT_KERNEL_BASE, RT_KERNEL_SIZE},
        .hostfn = {RT_HOSTFN_BASE, RT_HOSTFN_SIZE},
        .pool = pool ? strtoull(pool, NULL, 0) : DEFAULT_POOL + res_heap_extra_bytes(),   // resolution.c
    };
    if (!L.pool) rt_fatal("memory: TOTK_APP_POOL is 0");
    // Dirty tracking (see mem_dirty_track): hashes under Wine unless TOTK_DIRTY says otherwise.
    const char *d = cfg_value("TOTK_DIRTY");
    if (strcmp(d, "auto") && strcmp(d, "hash") && strcmp(d, "writewatch")) {
        rt_log("memory: TOTK_DIRTY=%s is not auto, hash or writewatch: auto", d);
        d = "auto";
    }
    char why[64];
    if (strcmp(d, "auto")) snprintf(why, sizeof why, "TOTK_DIRTY=%s", d);
    else snprintf(why, sizeof why, "auto, %s%s", wine_version ? "Wine " : "Windows", wine_version ? wine_version() : "");
    dirty_hash = !strcmp(d, "hash") || (!strcmp(d, "auto") && wine_version) || no_watch;
    rt_log("memory: dirty tracking %s (%s)", !dirty_hash ? "write watch" : no_watch ?
           "hash: imported pages by XXH3, no write watch on the other pages (TOTK_WRITE_WATCH)" :
           "hash: imported pages by XXH3, other pages by write watch", why);
}

const RtGuestLayout *mem_layout(void) { return &L; }

// Adapted from LAS Recomp runtime/platform/windows.c:40-63 @ da35b83 (MIT, Copyright (c) 2026 Peiikai): GetWriteWatch
// with the reset flag. A range inside one watched reservation (stack and heap block, or alias region) only.
static uint64_t written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max, DWORD flags) {
    if (!max || !size) return 0;
    if (!watched(addr, size)) return UINT64_MAX;
    ULONG_PTR n = (ULONG_PTR)max;
    DWORD gran;
    _Static_assert(sizeof(PVOID) == sizeof(uint64_t), "page addresses are written straight into a uint64_t array");
    if (GetWriteWatch(flags, (void *)(uintptr_t)addr, (SIZE_T)size, (PVOID *)pages, &n, &gran)) return UINT64_MAX;
    return n;
}

// ---- dirty tracking of imported pages (TOTK_DIRTY)
// Under Wine (Proton 11) GetWriteWatch is exact for memory only the CPU touches, but every page of a Vulkan host-pointer
// import comes back written after any GPU access to it, a read included: amdgpu takes the pages of the userptr with
// write access, which clears the userfaultfd write protection Wine's kernel write watches rely on. Every reset also
// write-protects the pages again, and the next GPU access then revalidates the whole import (wwdiag on RADV: a 64 MiB
// GPU read took 8.5-9 ms after a reset, 2 ms without one). Each frame then saw every pool the GPU read as rewritten.
// "hash" mode (TOTK_DIRTY=hash, and auto under Wine, which ntdll's wine_get_version export identifies) keeps a 64-bit
// XXH3 hash per page that an import covers (mem_dirty_track) and answers for those pages by hashing them again; every
// other page keeps GetWriteWatch. GetWriteWatch then never resets an imported page, so the import is never
// write-protected again. "writewatch" (the default on Windows) uses GetWriteWatch for every page.
// - A page joins with its fresh bit set and is reported once at its next query, as Windows reports every page of a new
//   import as written.
// - A page that is not committed (heap above its size, unmapped alias pages, moved stack sources) is not read; it is
//   marked fresh and reported once it is readable again.
// - mem_dirty_absorb takes the current bytes as the hash: the runtime's own writes into imported pages (GPU write-backs,
//   texture write-backs) are reported to the back end by vk_guest_written, as GetWriteWatch never sees GPU writes.
// - Concurrent queries over the same page may both report it, never neither: a hash is stored only by a query that
//   compared it with the stored one.
// Locks: `lock` shared keeps the committed state still while a query reads guest pages; dlock orders mem_dirty_track
// (exclusive) against queries and absorbs (shared), which update hashes and fresh bits with relaxed atomics.
#define XXH_INLINE_ALL
#include "third_party/xxhash/xxhash.h"

#define DCHUNK_SHIFT 21
#define DCHUNK_PAGES (1u << (DCHUNK_SHIFT - 12))
#define DFRESH 0x8000u
typedef struct { uint64_t h[DCHUNK_PAGES]; uint16_t n[DCHUNK_PAGES]; } DChunk;   // n: imports over the page | DFRESH
static DChunk *dchunk[(LOW_SIZE + ALIAS_SIZE) >> DCHUNK_SHIFT];
static SRWLOCK dlock = SRWLOCK_INIT;
static volatile LONG64 d_tracked, d_hashed, d_ticks;   // bytes tracked now; since mem_dirty_stats: bytes hashed, and
                                                       // performance counter ticks in hash-mode queries and absorbs

// The chunk slot of watched address a, or NULL.
static DChunk **dchunk_slot(uint64_t a) {
    if (a - L.aslr.base < LOW_SIZE) return &dchunk[(a - L.aslr.base) >> DCHUNK_SHIFT];
    if (a - L.alias.base < ALIAS_SIZE) return &dchunk[(LOW_SIZE + (a - L.alias.base)) >> DCHUNK_SHIFT];
    return NULL;
}

// The moved stacks whose source (inaccessible) or destination (committed) overlaps [lo, hi); `lock` held.
typedef struct { uint64_t lo, hi; int readable; } DSpan;
static int stack_spans(uint64_t lo, uint64_t hi, DSpan *out, int cap) {
    int n = 0;
    for (unsigned i = 0; i < nstack_maps && n < cap; i++) {
        const StackMap *m = &stack_maps[i];
        if (lo < m->src + m->size && m->src < hi) out[n++] = (DSpan){m->src, m->src + m->size, 0};
        if (n < cap && lo < m->dst + m->size && m->dst < hi) out[n++] = (DSpan){m->dst, m->dst + m->size, 1};
    }
    return n;
}

// Page a is committed and readable; `lock` held. sp: stack_spans of the query.
static int page_readable(uint64_t a, const DSpan *sp, int nsp) {
    for (int i = 0; i < nsp; i++)
        if (a >= sp[i].lo && a < sp[i].hi) return sp[i].readable;
    if (a - L.heap.base < heap_size) return 1;
    if (a - L.alias.base < ALIAS_SIZE) return alias_pages[(a - L.alias.base) / PAGE] != 0;
    return 0;
}

static uint64_t page_hash(uint64_t a) { return XXH3_64bits((const void *)(uintptr_t)a, PAGE); }

static LONG64 qpc_now(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Large queries hash on the thread pool: a query gathers its tracked readable pages, the pool's callbacks and the
// caller hash them in batches, and the compare pass below takes the hashes in the same order. The committed state and
// the tracked pages cannot change in between (`lock` and dlock stay held). 4 KiB pages hash at 30-36 GB/s on one
// core (wwdiag), so a query of 1 MiB or more is worth the hand-off. On an 8700F with DDR5 the title scene hashes about
// 370 MiB a frame in about 64 such queries: 8.5 ms on one thread, 7.2 ms with three helpers (the memory bus is the
// limit there); the Z1's single core reads more slowly than its bus allows.
#define PAR_MIN_PAGES 256
#define PAR_BATCH 32
typedef struct { const uint64_t *addr; uint64_t *out; LONG n; volatile LONG next; } HashJob;
static int hash_helpers = -1;   // thread-pool callbacks per large query; 0: the caller hashes alone

static void hash_batches(HashJob *j) {
    for (LONG i; (i = InterlockedExchangeAdd(&j->next, PAR_BATCH)) < j->n;)
        for (LONG k = i, e = i + PAR_BATCH < j->n ? i + PAR_BATCH : j->n; k < e; k++) j->out[k] = page_hash(j->addr[k]);
}

static VOID CALLBACK hash_cb(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_WORK w) {
    (void)inst, (void)w;
    hash_batches(ctx);
}

static void hash_parallel(const uint64_t *addr, uint64_t *out, uint64_t n) {
    if (hash_helpers < 0) {
        // TOTK_DIRTY_HELPERS: auto, or 0..8 (under Wine every hand-off wakes pool threads through the wineserver)
        const char *e = cfg_get("TOTK_DIRTY_HELPERS");
        if (e && *e && strcmp(e, "auto")) {
            int h = atoi(e);
            hash_helpers = h < 0 ? 0 : h > 8 ? 8 : h;
        } else {
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            int h = (int)si.dwNumberOfProcessors / 4;   // the game itself keeps most threads busy on few cores
            hash_helpers = h > 3 ? 3 : h;
            if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) hash_helpers = 0;   // faster on a Z1
        }
        rt_log("memory: dirty hashing on the caller plus %d thread-pool helpers per large query", hash_helpers);
    }
    HashJob j = {addr, out, (LONG)n, 0};
    PTP_WORK w = hash_helpers ? CreateThreadpoolWork(hash_cb, &j, NULL) : NULL;
    for (int i = 0; w && i < hash_helpers; i++) SubmitThreadpoolWork(w);
    hash_batches(&j);
    if (w) {
        WaitForThreadpoolWorkCallbacks(w, TRUE);   // callbacks not started yet have nothing left to take
        CloseThreadpoolWork(w);
    }
}

// A RESET query that filled pages[] to max resumes after the last page it reported when the same thread asks again for
// the same range (the callers loop until a short answer); without this, each round would hash the range from the start.
static __thread uint64_t resume_addr, resume_size, resume_at;
static __thread uint64_t *par_addr, *par_hash, par_cap;   // hash_parallel input and output of this thread's query

static uint64_t written_hashed(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max, DWORD flags) {
    if (!max || !size) return 0;
    if (!watched(addr, size)) return UINT64_MAX;
    int reset = (flags & WRITE_WATCH_FLAG_RESET) != 0;
    uint64_t lo = addr & ~(PAGE - 1), hi = align_up(addr + size, PAGE), a = lo;
    if (reset && resume_size && resume_addr == addr && resume_size == size && resume_at > lo && resume_at < hi)
        a = resume_at;
    resume_size = 0;
    DSpan sp[32];
    uint64_t n = 0, ww_lo = 0, hashed = 0;   // ww_lo: start of the pending run of untracked pages, 0 none
    int fail = 0;
    LONG64 t0 = qpc_now();
    AcquireSRWLockShared(&lock);
    AcquireSRWLockShared(&dlock);
    int nsp = stack_spans(lo, hi, sp, 32);
    // Large query: the tracked readable pages from a on, hashed ahead (the compare pass meets them in this order).
    uint64_t np = 0, pi = 0;
    if (hi - a >= PAR_MIN_PAGES * PAGE) {
        for (uint64_t x = a; x < hi;) {
            uint64_t ce = (x | ((1ull << DCHUNK_SHIFT) - 1)) + 1;
            DChunk *c = __atomic_load_n(dchunk_slot(x), __ATOMIC_ACQUIRE);
            if (ce > hi) ce = hi;
            for (; c && x < ce; x += PAGE) {
                if (!(__atomic_load_n(&c->n[(x >> 12) & (DCHUNK_PAGES - 1)], __ATOMIC_RELAXED) & ~DFRESH) ||
                    !page_readable(x, sp, nsp))
                    continue;
                if (np == par_cap) {
                    par_cap = par_cap ? par_cap * 2 : 4096;
                    par_addr = realloc(par_addr, par_cap * sizeof *par_addr);
                    par_hash = realloc(par_hash, par_cap * sizeof *par_hash);
                    if (!par_addr || !par_hash) rt_fatal("memory: out of memory for a hash query");
                }
                par_addr[np++] = x;
            }
            x = ce;
        }
        if (np >= PAR_MIN_PAGES) hash_parallel(par_addr, par_hash, np);
        else np = 0;
    }
    // Untracked pages go to GetWriteWatch in runs; the run is flushed before a tracked page so pages[] stays sorted.
#define FLUSH_WW(end)                                                                                                   \
    do {                                                                                                                \
        if (ww_lo && !fail && n < max && !no_watch) {                                                                   \
            uint64_t got = written_pages(ww_lo, (end) - ww_lo, pages + n, max - n, flags);                              \
            if (got == UINT64_MAX) fail = 1;                                                                            \
            else n += got;                                                                                              \
        }                                                                                                               \
        ww_lo = 0;                                                                                                      \
    } while (0)
    while (a < hi && n < max && !fail) {
        uint64_t ce = (a | ((1ull << DCHUNK_SHIFT) - 1)) + 1;
        if (ce > hi) ce = hi;
        DChunk *c = __atomic_load_n(dchunk_slot(a), __ATOMIC_ACQUIRE);
        if (!c) {
            if (!ww_lo) ww_lo = a;
            a = ce;
            continue;
        }
        for (; a < ce && n < max; a += PAGE) {
            unsigned i = (unsigned)((a >> 12) & (DCHUNK_PAGES - 1));
            uint16_t st = __atomic_load_n(&c->n[i], __ATOMIC_RELAXED);
            if (!(st & ~DFRESH)) {
                if (!ww_lo) ww_lo = a;
                continue;
            }
            FLUSH_WW(a);
            if (fail || n >= max) break;
            if (!page_readable(a, sp, nsp)) {
                if (!(st & DFRESH)) __atomic_fetch_or(&c->n[i], DFRESH, __ATOMIC_RELAXED);
                continue;
            }
            uint64_t h = pi < np && par_addr[pi] == a ? par_hash[pi++] : page_hash(a);
            hashed++;
            if (!(st & DFRESH) && h == __atomic_load_n(&c->h[i], __ATOMIC_RELAXED)) continue;
            pages[n++] = a;
            if (reset) {
                __atomic_store_n(&c->h[i], h, __ATOMIC_RELAXED);
                if (st & DFRESH) __atomic_fetch_and(&c->n[i], (uint16_t)~DFRESH, __ATOMIC_RELAXED);
            }
        }
    }
    if (a >= hi) FLUSH_WW(hi);
    else if (ww_lo && n < max) FLUSH_WW(a);
#undef FLUSH_WW
    ReleaseSRWLockShared(&dlock);
    ReleaseSRWLockShared(&lock);
    InterlockedAdd64(&d_hashed, (LONG64)(hashed * PAGE));
    InterlockedAdd64(&d_ticks, qpc_now() - t0);
    if (fail) return UINT64_MAX;
    if (reset && n == max) resume_addr = addr, resume_size = size, resume_at = pages[n - 1] + PAGE;
    return n;
}

int mem_dirty_hash(void) { return dirty_hash; }

void mem_dirty_track(uint64_t addr, uint64_t size, int on) {
    if (!dirty_hash || !size) return;
    uint64_t lo = addr & ~(PAGE - 1), hi = align_up(addr + size, PAGE);
    if (!watched(lo, hi - lo)) return;
    int64_t delta = 0;
    AcquireSRWLockExclusive(&dlock);
    for (uint64_t a = lo; a < hi; a += PAGE) {
        DChunk **s = dchunk_slot(a), *c = *s;
        if (!c) {
            if (!on) { a = (a | ((1ull << DCHUNK_SHIFT) - 1)) + 1 - PAGE; continue; }
            c = calloc(1, sizeof *c);
            if (!c) rt_fatal("memory: out of memory for the page hashes");
            __atomic_store_n(s, c, __ATOMIC_RELEASE);
        }
        uint16_t *st = &c->n[(a >> 12) & (DCHUNK_PAGES - 1)], k = *st & ~DFRESH;
        if (on) {
            if (k == (uint16_t)~DFRESH) rt_fatal("memory: page %#llx is in too many imports", (unsigned long long)a);
            delta += !k;
            *st = (uint16_t)((k + 1) | DFRESH);
        } else if (k) {
            delta -= k == 1;
            *st = (uint16_t)((k - 1) | (*st & DFRESH));
        }
    }
    ReleaseSRWLockExclusive(&dlock);
    InterlockedAdd64(&d_tracked, delta * (int64_t)PAGE);
}

void mem_dirty_absorb(uint64_t addr, uint64_t size) {
    if (!dirty_hash || !size) return;
    uint64_t lo = addr & ~(PAGE - 1), hi = align_up(addr + size, PAGE), hashed = 0;
    if (!watched(lo, hi - lo)) return;
    DSpan sp[32];
    LONG64 t0 = qpc_now();
    AcquireSRWLockShared(&lock);
    AcquireSRWLockShared(&dlock);
    int nsp = stack_spans(lo, hi, sp, 32);
    for (uint64_t a = lo; a < hi;) {
        uint64_t ce = (a | ((1ull << DCHUNK_SHIFT) - 1)) + 1;
        DChunk *c = __atomic_load_n(dchunk_slot(a), __ATOMIC_ACQUIRE);
        if (ce > hi) ce = hi;
        for (; c && a < ce; a += PAGE) {
            unsigned i = (unsigned)((a >> 12) & (DCHUNK_PAGES - 1));
            uint16_t st = __atomic_load_n(&c->n[i], __ATOMIC_RELAXED);
            if (!(st & ~DFRESH) || (st & DFRESH) || !page_readable(a, sp, nsp)) continue;   // a fresh page stays reported
            __atomic_store_n(&c->h[i], page_hash(a), __ATOMIC_RELAXED);
            hashed++;
        }
        a = ce;
    }
    ReleaseSRWLockShared(&dlock);
    ReleaseSRWLockShared(&lock);
    InterlockedAdd64(&d_hashed, (LONG64)(hashed * PAGE));
    InterlockedAdd64(&d_ticks, qpc_now() - t0);
}

void mem_dirty_stats(uint64_t *tracked, uint64_t *hashed, double *ms) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    *tracked = (uint64_t)__atomic_load_n(&d_tracked, __ATOMIC_RELAXED);
    *hashed = (uint64_t)InterlockedExchange64(&d_hashed, 0);
    *ms = (double)InterlockedExchange64(&d_ticks, 0) * 1000.0 / (double)f.QuadPart;
}

uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return dirty_hash ? written_hashed(addr, size, pages, max, WRITE_WATCH_FLAG_RESET)
                      : written_pages(addr, size, pages, max, WRITE_WATCH_FLAG_RESET);
}

uint64_t mem_written_peek(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return dirty_hash ? written_hashed(addr, size, pages, max, 0) : written_pages(addr, size, pages, max, 0);
}

static uint64_t image_bytes(void) {
    uint64_t n = 0;
    for (int i = 0; i < ldr_count(); i++) n += ldr_module(i)->size;
    return align_up(n, PAGE);
}

static int overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) { return a < b + m && b < a + n; }
static int inside(uint64_t a, uint64_t n, RtRange r) { return a >= r.base && n <= r.size && a - r.base <= r.size - n; }

// ---- 0x01 SetHeapSize
uint32_t mem_set_heap_size(uint64_t size, uint64_t *addr) {
    AcquireSRWLockExclusive(&lock);
    if (size > HEAP_CAP || size % 0x200000 || size + image_bytes() > L.pool) {
        ReleaseSRWLockExclusive(&lock);
        return R_INVALID_STATE;
    }
    if (size > heap_size) commit(L.heap.base + heap_size, size - heap_size, "heap");
    if (size < heap_size) decommit(L.heap.base + size, heap_size - size, "heap");
    heap_size = size;
    ReleaseSRWLockExclusive(&lock);
    if (addr) *addr = L.heap.base;
    return R_OK;
}

// ---- module data permissions (0x02, 0x03) and module pages for QueryMemory
static int module_index(uint64_t a) {
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        if (a - m->base < m->size) return i;
    }
    return -1;
}

static uint8_t *perm_table(int i) {   // under lock
    if (!module_perm[i]) {
        const RtLoaded *m = ldr_module(i);
        size_t n = (m->size - m->data_off) / PAGE;
        module_perm[i] = malloc(n);
        if (!module_perm[i]) rt_fatal("memory: module permission table");
        memset(module_perm[i], PERM_RW, n);
    }
    return module_perm[i];
}

static unsigned module_page_perm(int i, uint64_t off) {   // under lock
    const RtLoaded *m = ldr_module(i);
    if (off < m->data_off) return off < align_up(m->text_end, PAGE) ? PERM_RX : PERM_R;
    return module_perm[i] ? module_perm[i][(off - m->data_off) / PAGE] : PERM_RW;
}

// The module whose data segment holds [a, a+n), or -1.
static int module_data(uint64_t a, uint64_t n) {
    int i = module_index(a);
    if (i < 0) return -1;
    const RtLoaded *m = ldr_module(i);
    uint64_t off = a - m->base;
    return off >= m->data_off && n <= m->size - off ? i : -1;
}

uint32_t mem_set_permission(uint64_t a, uint64_t n, uint32_t perm) {
    if (a & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!n || (n & (PAGE - 1))) return R_INVALID_SIZE;
    if (perm != PERM_NONE && perm != PERM_R && perm != PERM_RW) return R_INVALID_NEW_PERM;
    int i = module_data(a, n);
    if (i < 0) return R_INVALID_STATE;
    const RtLoaded *m = ldr_module(i);
    AcquireSRWLockExclusive(&lock);
    uint8_t *t = perm_table(i) + (a - m->base - m->data_off) / PAGE;
    for (uint64_t p = 0; p < n / PAGE; p++)
        if (t[p] & ATTR_PERMISSION_LOCKED) {
            ReleaseSRWLockExclusive(&lock);
            return R_INVALID_STATE;
        }
    memset(t, (int)perm, n / PAGE);
    protect(a, n, perm == PERM_RW ? PAGE_READWRITE : perm == PERM_R ? PAGE_READONLY : PAGE_NOACCESS, "module data");
    ReleaseSRWLockExclusive(&lock);
    rt_log("%s data permission +%#llx size %#llx perm %u", m->name, (unsigned long long)(a - m->base),
           (unsigned long long)n, perm);
    return R_OK;
}

// The only module attribute the SDK sets is the irreversible RELRO lock.
uint32_t mem_set_attribute(uint64_t a, uint64_t n, uint32_t mask, uint32_t at) {
    if (a & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!n || (n & (PAGE - 1))) return R_INVALID_SIZE;
    if (mask != ATTR_PERMISSION_LOCKED || at != ATTR_PERMISSION_LOCKED) return R_INVALID_NEW_PERM;
    int i = module_data(a, n);
    if (i < 0) return R_INVALID_STATE;
    const RtLoaded *m = ldr_module(i);
    AcquireSRWLockExclusive(&lock);
    uint8_t *t = perm_table(i) + (a - m->base - m->data_off) / PAGE;
    for (uint64_t p = 0; p < n / PAGE; p++) t[p] |= ATTR_PERMISSION_LOCKED;
    ReleaseSRWLockExclusive(&lock);
    return R_OK;
}

// ---- 0x04/0x05 MapMemory, UnmapMemory: thread stacks move from heap or alias into the stack region by copy; the
// source is inaccessible while moved and gets the contents back on unmap.
static int alias_all_mapped(uint64_t a, uint64_t n) {   // under lock
    uint64_t first = (a - L.alias.base) / PAGE;
    for (uint64_t i = 0; i < n / PAGE; i++)
        if (!alias_pages[first + i]) return 0;
    return 1;
}

static uint32_t stack_move(int unmap, uint64_t dst, uint64_t src, uint64_t n) {
    if (!n || ((dst | src | n) & (PAGE - 1))) return R_INVALID_ADDRESS;
    if (!inside(dst, n, L.stack)) return R_INVALID_REGION;
    int in_heap = inside(src, n, (RtRange){L.heap.base, heap_size});
    int in_alias = inside(src, n, L.alias);
    if (!in_heap && !in_alias) return R_INVALID_REGION;
    AcquireSRWLockExclusive(&lock);
    uint32_t r = R_OK;
    if (in_alias && !alias_all_mapped(src, n)) {
        r = R_INVALID_REGION;
        goto out;
    }
    unsigned found = nstack_maps;
    for (unsigned i = 0; i < nstack_maps; i++)
        if (stack_maps[i].dst == dst && stack_maps[i].src == src && stack_maps[i].size == n) found = i;
    if (unmap) {
        if (found == nstack_maps) {
            r = R_INVALID_STATE;
            goto out;
        }
        protect(src, n, PAGE_READWRITE, "moved stack source");
        memcpy((void *)(uintptr_t)src, (const void *)(uintptr_t)dst, n);
        decommit(dst, n, "moved stack");
        stack_maps[found] = stack_maps[--nstack_maps];
    } else {
        if (nstack_maps == MAX_STACK_MAPS) {
            r = R_INVALID_STATE;
            goto out;
        }
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(dst, n, stack_maps[i].dst, stack_maps[i].size) ||
                overlaps(src, n, stack_maps[i].src, stack_maps[i].size)) {
                r = R_INVALID_REGION;
                goto out;
            }
        commit(dst, n, "moved stack");
        memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
        protect(src, n, PAGE_NOACCESS, "moved stack source");
        stack_maps[nstack_maps++] = (StackMap){dst, src, n};
    }
out:
    ReleaseSRWLockExclusive(&lock);
    return r;
}

uint32_t mem_map(uint64_t dst, uint64_t src, uint64_t n) { return stack_move(0, dst, src, n); }
uint32_t mem_unmap(uint64_t dst, uint64_t src, uint64_t n) { return stack_move(1, dst, src, n); }

// ---- 0x2C/0x2D MapPhysicalMemory, UnmapPhysicalMemory. Horizon maps only the still-unmapped pages of the range and
// leaves mapped pages and their contents alone; TotK's heaps grow by re-mapping ranges they partly own.
static void alias_runs(uint64_t first, uint64_t count, uint8_t want, int do_commit) {   // under lock
    uint64_t i = 0;
    while (i < count) {
        if (alias_pages[first + i] != want) { i++; continue; }
        uint64_t j = i;
        while (j < count && alias_pages[first + j] == want) j++;
        uint64_t a = L.alias.base + (first + i) * PAGE, n = (j - i) * PAGE;
        if (do_commit) commit(a, n, "physical memory");
        else decommit(a, n, "physical memory");
        memset(alias_pages + first + i, !want, j - i);
        i = j;
    }
}

static uint32_t alias_map(int unmap, uint64_t a, uint64_t n) {
    if (!n || ((a | n) & (PAGE - 1))) return R_INVALID_ADDRESS;
    if (!inside(a, n, L.alias)) return R_INVALID_REGION;
    uint64_t first = (a - L.alias.base) / PAGE, count = n / PAGE;
    AcquireSRWLockExclusive(&lock);
    uint32_t r = R_OK;
    if (unmap) {
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(a, n, stack_maps[i].src, stack_maps[i].size)) {
                r = R_INVALID_REGION;
                goto out;
            }
        uint64_t released = 0;
        for (uint64_t i = 0; i < count; i++) released += alias_pages[first + i];
        alias_runs(first, count, 1, 0);
        alias_bytes -= released * PAGE;
    } else {
        uint64_t fresh = 0;
        for (uint64_t i = 0; i < count; i++) fresh += !alias_pages[first + i];
        if (fresh * PAGE + alias_bytes + heap_size + image_bytes() > L.pool) {
            r = R_LIMIT_REACHED;   // the SDK maps LimitReached to its recoverable out-of-memory result
            goto out;
        }
        alias_runs(first, count, 0, 1);
        alias_bytes += fresh * PAGE;
    }
out:
    ReleaseSRWLockExclusive(&lock);
    if (r) rt_log("%sMapPhysicalMemory %#llx+%#llx failed %#x (alias %#llx used, heap %#llx)", unmap ? "Un" : "",
                  (unsigned long long)a, (unsigned long long)n, r, (unsigned long long)alias_bytes,
                  (unsigned long long)heap_size);
    return r;
}

uint32_t mem_map_physical(uint64_t a, uint64_t n) { return alias_map(0, a, n); }
uint32_t mem_unmap_physical(uint64_t a, uint64_t n) { return alias_map(1, a, n); }

// ---- 0x06 QueryMemory
// The region around a that no known region covers: everything else below 2^39 belongs to the host and is reported
// as taken (Code, RX), so the SDK never picks it.
static void taken_gap(uint64_t a, uint64_t *base, uint64_t *size) {
    uint64_t lo = 0, hi = RT_ADDRESS_LIMIT;
    RtRange known[3 + MOD_COUNT + 2];
    int n = 0;
    known[n++] = (RtRange){L.aslr.base, LOW_SIZE};
    known[n++] = L.alias;
    known[n++] = L.kernel;
    known[n++] = L.hostfn;
    for (int i = 0; i < ldr_count(); i++) known[n++] = (RtRange){ldr_module(i)->base, ldr_module(i)->size};
    for (int i = 0; i < n; i++) {
        uint64_t e = known[i].base + known[i].size;
        if (e <= a && e > lo) lo = e;
        if (known[i].base > a && known[i].base < hi) hi = known[i].base;
    }
    *base = lo;
    *size = hi - lo;
}

static uint64_t run_end(const uint8_t *p, uint64_t i, uint64_t n, uint8_t v) {
    while (i < n && (i & 7)) { if (p[i] != v) return i; i++; }
    uint64_t w = v ? 0x0101010101010101ull : 0;
    while (i + 8 <= n && *(const uint64_t *)(p + i) == w) i += 8;
    while (i < n && p[i] == v) i++;
    return i;
}

static uint64_t run_start(const uint8_t *p, uint64_t i, uint8_t v) {   // first index of the run that ends at i
    while (i > 0 && (i & 7)) { if (p[i - 1] != v) return i; i--; }
    uint64_t w = v ? 0x0101010101010101ull : 0;
    while (i >= 8 && *(const uint64_t *)(p + i - 8) == w) i -= 8;
    while (i > 0 && p[i - 1] == v) i--;
    return i;
}

uint32_t mem_query(uint64_t a, RtMemoryInfo *out) {
    RtMemoryInfo r = {.base = a & ~(PAGE - 1), .size = PAGE};
    uint64_t low = L.aslr.base;
    AcquireSRWLockShared(&lock);
    int mi;
    if (a >= RT_ADDRESS_LIMIT) {
        r.base = RT_ADDRESS_LIMIT;
        r.size = 0 - RT_ADDRESS_LIMIT;
        r.state = ST_INACCESSIBLE;
    } else if (a < low) {
        r.base = 0;
        r.size = low;
        r.state = ST_FREE;
    } else if (a < low + LOW_SIZE) {
        uint64_t heap = L.heap.base;
        if (a >= heap && a < heap + heap_size) {
            r = (RtMemoryInfo){.base = heap, .size = heap_size, .state = ST_NORMAL, .perm = PERM_RW};
        } else if (a < heap) {
            r = (RtMemoryInfo){.base = low, .size = heap - low, .state = ST_FREE};
        } else {
            r = (RtMemoryInfo){.base = heap + heap_size, .size = low + LOW_SIZE - heap - heap_size, .state = ST_FREE};
        }
    } else if (a - L.alias.base < L.alias.size) {
        uint64_t p = (a - L.alias.base) / PAGE;
        uint8_t v = alias_pages[p];
        uint64_t s = run_start(alias_pages, p, v), e = run_end(alias_pages, p, ALIAS_PAGES, v);
        r = (RtMemoryInfo){.base = L.alias.base + s * PAGE, .size = (e - s) * PAGE, .state = v ? ST_NORMAL : ST_FREE,
                           .perm = v ? PERM_RW : PERM_NONE};
    } else if ((mi = module_index(a)) >= 0) {
        const RtLoaded *m = ldr_module(mi);
        uint64_t off = (a - m->base) & ~(PAGE - 1), lo = off, hi = off + PAGE;
        unsigned perm = module_page_perm(mi, off);
        int data = off >= m->data_off;
        uint64_t limit = data ? m->data_off : 0, end = data ? m->size : m->data_off;
        while (lo > limit && module_page_perm(mi, lo - PAGE) == perm) lo -= PAGE;
        while (hi < end && module_page_perm(mi, hi) == perm) hi += PAGE;
        r = (RtMemoryInfo){.base = m->base + lo, .size = hi - lo, .state = data ? ST_CODEDATA : ST_CODE,
                           .perm = perm & 7, .attr = perm & ATTR_PERMISSION_LOCKED};
    } else if (a - L.kernel.base < L.kernel.size) {
        r = (RtMemoryInfo){.base = L.kernel.base, .size = L.kernel.size, .state = ST_THREADLOCAL, .perm = PERM_RW};
    } else if (a - L.hostfn.base < L.hostfn.size) {
        r = (RtMemoryInfo){.base = L.hostfn.base, .size = L.hostfn.size, .state = ST_INACCESSIBLE};
    } else {
        taken_gap(a, &r.base, &r.size);
        r.state = ST_CODE;
        r.perm = PERM_RX;
    }
    // Moved stacks: the destination is Stack, the source Normal without access; free and normal regions around them
    // are clipped (MNG services.c).
    for (unsigned i = 0; i < nstack_maps; i++) {
        const StackMap *m = &stack_maps[i];
        uint64_t end = m->dst + m->size, send = m->src + m->size;
        if (a >= m->dst && a < end) {
            r = (RtMemoryInfo){.base = m->dst, .size = m->size, .state = ST_STACK, .perm = PERM_RW};
            break;
        }
        if (a >= m->src && a < send) {
            r = (RtMemoryInfo){.base = m->src, .size = m->size, .state = ST_NORMAL, .perm = PERM_NONE, .attr = 4};
            break;
        }
        uint64_t lo = r.base, hi = r.base + r.size;
        uint64_t b = r.state == ST_FREE ? m->dst : r.state == ST_NORMAL ? m->src : 0;
        uint64_t e = b + m->size;
        if (!b) continue;
        if (e <= a && e > lo) lo = e;
        if (b > a && b < hi) hi = b;
        r.base = lo;
        r.size = hi - lo;
    }
    ReleaseSRWLockShared(&lock);
    if (out) *out = r;
    return R_OK;
}

// ---- GetInfo memory ids
int mem_info(uint32_t id, uint64_t *out) {
    uint64_t v;
    switch (id) {
    case 2: v = L.alias.base; break;
    case 3: v = L.alias.size; break;
    case 4: v = L.heap.base; break;
    case 5: v = L.heap.size; break;
    case 6: case 21: v = L.pool; break;
    case 7: case 22:
        AcquireSRWLockShared(&lock);
        v = image_bytes() + heap_size + alias_bytes;
        ReleaseSRWLockShared(&lock);
        break;
    case 12: v = L.aslr.base; break;
    case 13: v = L.aslr.size; break;
    case 14: v = L.stack.base; break;
    case 15: v = L.stack.size; break;
    case 16: v = SYSTEM_RESOURCE_BUDGET; break;
    case 17: v = sizeof stack_maps + ALIAS_PAGES; break;   // system resource in use: our bookkeeping
    case 28: v = 0; break;   // AliasRegionExtraSize: no kernel-owned extension
    default: return 0;
    }
    *out = v;
    return 1;
}

// ---- kernel region: power-of-two blocks, a free list per size, bump allocation with lazy commit.
#define KCLASSES 29          // 64 B .. 256 MiB
#define KMIN_SHIFT 6
#define KCOMMIT 0x10000ull
typedef struct KFree { struct KFree *next; } KFree;
static KFree *kfree_list[KCLASSES];
static uint64_t kbump, kcommitted;
static SRWLOCK klock = SRWLOCK_INIT;

static int kclass(uint64_t size) {
    int s = KMIN_SHIFT;
    while ((1ull << s) < size) s++;
    return s - KMIN_SHIFT;
}

void *mem_kernel_alloc(uint64_t size, uint64_t align) {
    if (!L.pool) rt_fatal("mem_kernel_alloc before mem_init");
    if (align > KCOMMIT || (align & (align - 1))) rt_fatal("mem_kernel_alloc: alignment %#llx", (unsigned long long)align);
    uint64_t want = size > align ? size : align;
    int k = kclass(want ? want : 1);
    if (k >= KCLASSES) rt_fatal("mem_kernel_alloc: %#llx bytes", (unsigned long long)size);
    uint64_t bytes = 1ull << (k + KMIN_SHIFT), al = bytes < KCOMMIT ? bytes : KCOMMIT;
    AcquireSRWLockExclusive(&klock);
    KFree *f = kfree_list[k];
    void *p;
    if (f) {
        kfree_list[k] = f->next;
        p = f;
    } else {
        uint64_t a = align_up(kbump ? kbump : RT_KERNEL_BASE, al);
        if (a + bytes > RT_KERNEL_BASE + RT_KERNEL_SIZE) rt_fatal("kernel region exhausted (%#llx bytes)",
                                                                  (unsigned long long)bytes);
        uint64_t need = align_up(a + bytes, KCOMMIT);
        if (!kcommitted) kcommitted = RT_KERNEL_BASE;
        if (need > kcommitted) {
            commit(kcommitted, need - kcommitted, "kernel region");
            kcommitted = need;
        }
        kbump = a + bytes;
        p = (void *)(uintptr_t)a;
    }
    ReleaseSRWLockExclusive(&klock);
    memset(p, 0, size ? size : 1);
    return p;
}

void mem_kernel_free(void *p, uint64_t size) {
    if (!p) return;
    uint64_t a = (uintptr_t)p;
    if (a - RT_KERNEL_BASE >= RT_KERNEL_SIZE) rt_fatal("mem_kernel_free(%p): not in the kernel region", p);
    int k = kclass(size ? size : 1);
    // A block allocated with a larger alignment sits in a larger class; freeing it into the smaller class only
    // wastes the rest.
    AcquireSRWLockExclusive(&klock);
    KFree *f = p;
    f->next = kfree_list[k];
    kfree_list[k] = f;
    ReleaseSRWLockExclusive(&klock);
}
