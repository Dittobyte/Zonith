// The BC7 texture cache (bc7.h). Ported from UltraTears (kuraeow/ultratears, GPL-3.0, commit 57bf619):
// src/gfx/vkb_bc7.c (the cache files and the background encoder), src/gfx/tex/bc7key.c (the key) and
// tools/bc7build/bc7build.c (the builder, here the totk --bc7-build mode reading our RomFS image directly).
//
// Files in TOTK_BC7_DIR (default "bc7" next to totk): bc7.pack (blocks, appended) and bc7.idx (32-byte records
// {u64 key a, key b, offset; u32 length, format 0 BC7 / 1 BC1}, appended after their blocks are written, so a record
// always has its data; a record past the end of the pack is an interrupted write and dropped). The same files as
// UltraTears', so either builder fills them. The pack as it was at start is mapped read-only; blocks appended during
// the session are read from the file. The cache is the player's own derived data: never in git or a build.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>
#include "bc7.h"
#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "tex.h"
#include "platform/platform.h"

#ifndef _WIN32
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

int bc7_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_BC7"); on = !(e && *e == '0'); }
    return on;
}

// ---- the key (UltraTears bc7key.c bc7_key_of with depth 1: the same bytes give the same key as their builder's)
static uint64_t mix(uint64_t h, uint64_t v) { h ^= v * 0x9E3779B97F4A7C15ull; h = (h << 27 | h >> 37) * 0xC2B2AE3D27D4EB4Full; return h; }
static uint64_t fin(uint64_t h) { h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull; h ^= h >> 33; return h; }

Bc7Key bc7_key_of(const uint8_t *blocks, size_t n, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh) {
    uint64_t a = 0x5432102030405060ull, b = 0x0123456789ABCDEFull;
    uint64_t p0 = (uint64_t)w << 32 | h, p1 = (uint64_t)1 << 32 | bw << 16 | bh << 8;
    a = mix(a, p0); a = mix(a, p1); b = mix(b, p1); b = mix(b, p0);
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        uint64_t x, y;
        memcpy(&x, blocks + i, 8); memcpy(&y, blocks + i + 8, 8);
        a = mix(a, x); b = mix(b, y);
    }
    for (; i < n; i++) { a = mix(a, blocks[i]); b = mix(b, (uint64_t)blocks[i] << 8); }
    Bc7Key k = {fin(a ^ n), fin(b ^ (n << 1))};
    if (!k.a && !k.b) k.a = 1;   // 0,0 marks an empty slot of the index
    return k;
}

// ---- the index (open addressing) and the files
typedef struct { uint64_t a, b, off; uint32_t len, fmt; } Rec;   // also the bc7.idx record (fmt: 0 BC7, 1 BC1)
static Rec *tab;
static size_t cap, count;
static FILE *pack, *idx;
static const uint8_t *map;
static uint64_t map_size;
static PlatMutex mu = PLAT_MUTEX_INIT;
static int opened;   // 1 open, -1 unusable

static Rec *slot(uint64_t a, uint64_t b) {   // mu held, cap a power of two
    size_t h = (size_t)(a ^ (b * 0x9E3779B97F4A7C15ull)) & (cap - 1);
    while ((tab[h].a || tab[h].b) && !(tab[h].a == a && tab[h].b == b)) h = (h + 1) & (cap - 1);
    return &tab[h];
}
static void put(const Rec *r) {   // mu held
    if ((count + 1) * 2 > cap) {
        Rec *old = tab;
        size_t n = cap;
        cap = cap ? cap * 2 : 1 << 16;
        if (!(tab = calloc(cap, sizeof *tab))) rt_fatal("bc7: out of memory for the index");
        for (size_t i = 0; i < n; i++) if (old[i].a || old[i].b) *slot(old[i].a, old[i].b) = old[i];
        free(old);
    }
    Rec *s = slot(r->a, r->b);
    if (!s->a && !s->b) count++;
    *s = *r;
}
static int known(Bc7Key k) { return cap && (slot(k.a, k.b)->a || slot(k.a, k.b)->b); }   // mu held

static const char *cache_dir(void) {
    static char d[1024];
    if (*d) return d;
    const char *e = cfg_get("TOTK_BC7_DIR");
    if (e && *e) { snprintf(d, sizeof d, "%s", e); return d; }
    char exe[1024];
    char *sep = plat_exe_path(exe, sizeof exe) ? plat_last_sep(exe) : NULL;
    if (sep) *sep = 0;
    snprintf(d, sizeof d, "%s/bc7", sep ? exe : ".");
    return d;
}

static int open_files(void) {   // mu held
    if (opened) return opened > 0;
    opened = -1;
    const char *d = cache_dir();
    char path[1100];
    plat_fs_mkdir(d);
    snprintf(path, sizeof path, "%s/bc7.pack", d);
    PlatFile *pf = plat_file_open(path);   // mapped before the writer opens it (plat_file_open shares reads only)
    if (pf) {
        PlatFileId id;
        if (plat_file_id(pf, &id) && id.size && (map = plat_file_map(pf))) map_size = id.size;
        plat_file_close(pf);   // the view outlives the handle
    }
    if (!(pack = fopen(path, "r+b")) && !(pack = fopen(path, "w+b"))) { rt_log("bc7: cannot open %s", path); return 0; }
    _fseeki64(pack, 0, SEEK_END);
    int64_t pack_size = _ftelli64(pack);
    snprintf(path, sizeof path, "%s/bc7.idx", d);
    if (!(idx = fopen(path, "r+b")) && !(idx = fopen(path, "w+b"))) { rt_log("bc7: cannot open %s", path); return 0; }
    Rec r;
    int64_t good = 0;
    while (fread(&r, sizeof r, 1, idx) == 1 && (int64_t)(r.off + r.len) <= pack_size) { put(&r); good += sizeof r; }
    _fseeki64(idx, good, SEEK_SET);
    rt_log("bc7: cache %s: %zu surfaces, %.1f MiB%s", d, count, pack_size / 1048576.0, map ? " (mapped)" : "");
    opened = 1;
    return 1;
}

int bc7_lookup(Bc7Key k) {
    plat_mutex_lock(&mu);
    int fmt = BC7_MISS;
    if (open_files() && known(k)) {
        Rec *s = slot(k.a, k.b);
        if (s->len) fmt = s->fmt ? BC7_FMT_BC1 : BC7_FMT_BC7;
    }
    plat_mutex_unlock(&mu);
    return fmt;
}

int bc7_read(Bc7Key k, int fmt, uint8_t *out, size_t n) {
    plat_mutex_lock(&mu);
    Rec r = {0};
    int ok = open_files() && known(k);
    if (ok) r = *slot(k.a, k.b), ok = r.len == n && (r.fmt ? BC7_FMT_BC1 : BC7_FMT_BC7) == fmt;
    int mapped = ok && r.off + n <= map_size;
    if (ok && !mapped) ok = !_fseeki64(pack, (int64_t)r.off, SEEK_SET) && fread(out, 1, n, pack) == n;
    plat_mutex_unlock(&mu);
    if (mapped) memcpy(out, map + r.off, n);   // the mapped part is never written again
    return ok;
}

static void store(Bc7Key k, const uint8_t *data, size_t n, int fmt) {
    plat_mutex_lock(&mu);
    if (open_files() && !(known(k) && slot(k.a, k.b)->len)) {   // a claimed key (length 0) is written
        _fseeki64(pack, 0, SEEK_END);
        Rec r = {k.a, k.b, (uint64_t)_ftelli64(pack), (uint32_t)n, fmt == BC7_FMT_BC1};
        if (fwrite(data, 1, n, pack) == n && !fflush(pack)) {
            _fseeki64(idx, 0, SEEK_END);
            if (fwrite(&r, sizeof r, 1, idx) == 1) { fflush(idx); put(&r); }
        }
    }
    plat_mutex_unlock(&mu);
}

// ---- encoding in the background (a miss on an RGBA8 upload: BC7 the next time)
typedef struct Job { Bc7Key k; uint8_t *rows; uint32_t w, h, bw, bh; int srgb; size_t n; struct Job *next; } Job;
static Job *jobs, **tail = &jobs;
static size_t queued;
static PlatMutex jmu = PLAT_MUTEX_INIT;
static PlatCond jcv = PLAT_COND_INIT;
enum { QUEUE_MAX = 128u << 20, ENCODERS = 2 };   // ASTC rows (4-16x smaller than UltraTears' RGBA8 copies)

static int quality(void) {   // TOTK_BC7_QUALITY: bc7enc uber level of in-game encodes (default 0; the build uses 2)
    static int q = -1;
    if (q < 0) { const char *e = cfg_get("TOTK_BC7_QUALITY"); q = e && *e ? atoi(e) : 0; }
    return q;
}

static void encode_rows(Bc7Key k, const uint8_t *rows, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, int srgb, int q) {
    size_t n = bc7_size(w, h);
    uint8_t *rgba = malloc((size_t)w * h * 4), *out = malloc(n);
    if (rgba && out) {
        astc_decode_ldr(rows, w, h, bw, bh, srgb, rgba);
        bc7_encode(rgba, w, h, srgb, q, out);
        store(k, out, n, BC7_FMT_BC7);
    }
    free(rgba);
    free(out);
}

static void encoder(void *unused) {
    (void)unused;
    plat_thread_priority(PLAT_PRIO_LOWEST);   // the game's and the recording threads come first
    for (;;) {
        plat_mutex_lock(&jmu);
        while (!jobs) plat_cond_wait(&jcv, &jmu);
        Job *j = jobs;
        if (!(jobs = j->next)) tail = &jobs;
        plat_mutex_unlock(&jmu);
        if (bc7_lookup(j->k) == BC7_MISS) encode_rows(j->k, j->rows, j->w, j->h, j->bw, j->bh, j->srgb, quality());
        plat_mutex_lock(&jmu);
        queued -= j->n;
        plat_mutex_unlock(&jmu);
        free(j->rows);
        free(j);
    }
}

void bc7_queue(Bc7Key k, const uint8_t *rows, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, int srgb) {
    size_t n = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
    Job *j = malloc(sizeof *j);
    uint8_t *copy = malloc(n);
    plat_mutex_lock(&jmu);
    static int started;
    int ok = j && copy && queued + n <= QUEUE_MAX;   // a dropped one is met again later
    if (ok) {
        memcpy(copy, rows, n);
        *j = (Job){k, copy, w, h, bw, bh, srgb, n, NULL};
        queued += n;
        *tail = j, tail = &j->next;
        plat_cond_signal(&jcv);
        if (!started) {
            started = 1;
            for (int i = 0; i < ENCODERS; i++) plat_thread_spawn(encoder, NULL, "bc7-encode", 0);
        }
    }
    plat_mutex_unlock(&jmu);
    if (!ok) free(j), free(copy);
}

void bc7_encode_now(Bc7Key k, const uint8_t *rows, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, int srgb, int fmt,
                    uint8_t *out) {
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) { memset(out, 0, fmt == BC7_FMT_BC1 ? bc1_size(w, h) : bc7_size(w, h)); return; }
    astc_decode_ldr(rows, w, h, bw, bh, srgb, rgba);
    if (fmt == BC7_FMT_BC1) {
        bc1_encode(rgba, w, h, 4, out);   // not cached: whether a texture is BC1 is the build's choice
    } else {
        bc7_encode(rgba, w, h, srgb, 0, out);
        store(k, out, bc7_size(w, h), BC7_FMT_BC7);
    }
    free(rgba);
}

// ---------------------------------------------------------------- totk --bc7-build
// Every ASTC surface of the RomFS's TexToGo (.txtg) and BNTX (.bntx, .bntx.zs) files, decoded and encoded once
// (UltraTears bc7build.c, its format notes):
// TexToGo: 0x00 u16 header size, 0x04 "6PK0", 0x08 u16 width, 0x0A u16 height, 0x0C u16 layers, 0x0E u8 levels,
//   0x3C u16 format (ASTC 0x1xx; sRGB 0x105, 0x109), 0x44 u32 block info (0x7F00 | (bw - 1) << 4 | bh - 1). At the header
//   size: per surface {u16 layer, u8 level, u8}, then per surface {u32 size, u32}, then each surface as a zstd frame of
//   block-linear data.
// BNTX: "NX  " at 0x20, texture count u32 0x24, BRTI pointer array u64 0x28; a BRTI: levels u16 0x16, format u32 0x1C
//   (ASTC 0x2D..0x3A << 8 | 1 UNORM / 6 sRGB), width 0x24, height 0x28, depth 0x2C, layers 0x30, log2 block height in
//   the low 3 bits of 0x34, image size u32 0x50, level offsets u64[] at 0x70 (layer 0; each layer image size / layers on).
// Textures with blocks over 100 texels (the interface's 12x12 maps) become BC1 when opaque, else BC7.
enum { BC1_AREA = 100, BC1_LEVEL = 10, MAX_AREA = 144 };
typedef struct { uint64_t off, size; const char *path; } BFile;
static BFile *bfiles;
static int nbfiles, bquality = 2;
static PlatFile *bimage;
static volatile int bnext, bdone;
static volatile uint64_t bsurfaces, bencoded, bhad, bbad, bbc1, bdone_bytes, btotal_bytes;

static int claim(Bc7Key k) {   // 1: not cached yet, now reserved for this thread (length 0 until written)
    plat_mutex_lock(&mu);
    int mine = !known(k);
    if (mine) { Rec r = {k.a, k.b, 0, 0, 0}; put(&r); }
    plat_mutex_unlock(&mu);
    return mine;
}

// Tegra X1 block-linear: a GOB is 64 bytes x 8 rows; GOBs stack bhm high.
static uint32_t block_height0(uint32_t hb) {
    uint32_t x = hb + hb / 2;
    return x >= 128 ? 16 : x >= 64 ? 8 : x >= 32 ? 4 : x >= 16 ? 2 : 1;
}
static uint32_t mip_block_height(uint32_t hb, uint32_t bh0) {   // the GPU's smaller block height for small levels
    uint32_t bhm = 1;
    while (bhm < (hb + 7) / 8) bhm <<= 1;
    if (bhm > 16) bhm = 16;
    return bhm > bh0 ? bh0 : bhm;
}
static int deswizzle(const uint8_t *src, size_t len, uint32_t wb, uint32_t hb, uint32_t bhm, uint8_t *dst) {
    uint32_t row = wb * 16, gobs = (row + 63) / 64;
    for (uint32_t y = 0; y < hb; y++)
        for (uint32_t x = 0; x < row; x += 16) {
            size_t o = (size_t)(y / (8 * bhm)) * (512u * bhm * gobs) + (size_t)(x / 64) * (512u * bhm) + ((y % (8 * bhm)) / 8) * 512u +
                       ((x % 64) / 32) * 256u + ((y % 8) / 2) * 64u + ((x % 32) / 16) * 32u + (y % 2) * 16u + (x % 16);
            if (o + 16 > len) return 0;
            memcpy(dst + (size_t)y * row + x, src + o, 16);
        }
    return 1;
}

// One texture's surfaces: with large blocks kept decoded until the texture's opacity is known.
typedef struct { Bc7Key k; uint8_t *rgba; uint32_t w, h; } Dec;
typedef struct { Dec *d; int n, cap, opaque, srgb, large; } BTex;
static void encode_one(const Dec *d, int bc1, int srgb) {
    size_t n = bc1 ? bc1_size(d->w, d->h) : bc7_size(d->w, d->h);
    uint8_t *out = malloc(n);
    if (!out) return;
    if (bc1) bc1_encode(d->rgba, d->w, d->h, BC1_LEVEL, out), __atomic_fetch_add(&bbc1, 1, __ATOMIC_RELAXED);
    else bc7_encode(d->rgba, d->w, d->h, srgb, bquality, out);
    store(d->k, out, n, bc1 ? BC7_FMT_BC1 : BC7_FMT_BC7);
    __atomic_fetch_add(&bencoded, 1, __ATOMIC_RELAXED);
    free(out);
}
static void tex_begin(BTex *t, uint32_t bw, uint32_t bh, int srgb) { t->n = 0, t->opaque = 1, t->srgb = srgb, t->large = bw * bh > BC1_AREA; }
static void tex_end(BTex *t) {
    for (int i = 0; i < t->n; i++) encode_one(&t->d[i], t->opaque, t->srgb), free(t->d[i].rgba);
    free(t->d);
}
static void do_surface(BTex *t, const uint8_t *sw, size_t len, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint32_t bhm) {
    uint32_t wb = (w + bw - 1) / bw, hb = (h + bh - 1) / bh;
    uint8_t *rows = malloc((size_t)wb * hb * 16);
    if (!rows || !deswizzle(sw, len, wb, hb, bhm, rows)) { __atomic_fetch_add(&bbad, 1, __ATOMIC_RELAXED); free(rows); return; }
    __atomic_fetch_add(&bsurfaces, 1, __ATOMIC_RELAXED);
    Bc7Key k = bc7_key_of(rows, (size_t)wb * hb * 16, w, h, bw, bh);
    if (!claim(k)) { __atomic_fetch_add(&bhad, 1, __ATOMIC_RELAXED); free(rows); return; }
    Dec d = {k, malloc((size_t)w * h * 4), w, h};
    if (d.rgba) {
        astc_decode_ldr(rows, w, h, bw, bh, t->srgb, d.rgba);
        if (!t->large) {
            encode_one(&d, 0, t->srgb);
            free(d.rgba);
        } else {
            for (size_t i = 3; i < (size_t)w * h * 4 && t->opaque; i += 4) t->opaque = d.rgba[i] == 255;
            if (t->n == t->cap) t->cap = t->cap ? t->cap * 2 : 16, t->d = realloc(t->d, sizeof *t->d * (size_t)t->cap);
            t->d[t->n++] = d;
        }
    }
    free(rows);
}

static const uint8_t astc_dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
static void do_bntx(const uint8_t *d, size_t n) {
    if (n < 0x30 || memcmp(d, "BNTX", 4) || memcmp(d + 0x20, "NX  ", 4)) { __atomic_fetch_add(&bbad, 1, __ATOMIC_RELAXED); return; }
    uint32_t cnt; uint64_t arr;
    memcpy(&cnt, d + 0x24, 4); memcpy(&arr, d + 0x28, 8);
    for (uint32_t t = 0; t < cnt && arr + 8ull * (t + 1) <= n; t++) {
        uint64_t o; memcpy(&o, d + arr + 8ull * t, 8);
        if (o + 0x78 > n || memcmp(d + o, "BRTI", 4)) { __atomic_fetch_add(&bbad, 1, __ATOMIC_RELAXED); continue; }
        uint16_t mips; uint32_t fmt, layout, image; int32_t w, h, depth, layers; uint64_t ptrs;
        memcpy(&mips, d + o + 0x16, 2); memcpy(&fmt, d + o + 0x1C, 4); memcpy(&w, d + o + 0x24, 4); memcpy(&h, d + o + 0x28, 4);
        memcpy(&depth, d + o + 0x2C, 4); memcpy(&layers, d + o + 0x30, 4); memcpy(&layout, d + o + 0x34, 4);
        memcpy(&image, d + o + 0x50, 4); memcpy(&ptrs, d + o + 0x70, 8);
        uint32_t type = fmt >> 8;
        if (type < 0x2D || type > 0x3A || depth > 1 || w <= 0 || h <= 0 || ptrs + 8ull * mips > n) continue;   // not 2D ASTC
        uint32_t bw = astc_dims[type - 0x2D][0], bh = astc_dims[type - 0x2D][1];
        if (bw * bh > MAX_AREA) continue;
        if (layers < 1) layers = 1;
        BTex tx = {0};
        tex_begin(&tx, bw, bh, (fmt & 0xFF) == 6);
        uint32_t bh0 = 1u << (layout & 7);
        uint64_t stride = (uint64_t)image / (uint32_t)layers;
        for (int32_t layer = 0; layer < layers; layer++)
            for (uint32_t m = 0; m < mips; m++) {
                uint64_t mo; memcpy(&mo, d + ptrs + 8ull * m, 8);
                mo += stride * (uint64_t)layer;
                if (mo >= n) { __atomic_fetch_add(&bbad, 1, __ATOMIC_RELAXED); continue; }
                uint32_t lw = (uint32_t)w >> m ? (uint32_t)w >> m : 1, lh = (uint32_t)h >> m ? (uint32_t)h >> m : 1;
                do_surface(&tx, d + mo, n - mo, lw, lh, bw, bh, mip_block_height((lh + bh - 1) / bh, bh0));
            }
        tex_end(&tx);
    }
}

static int ends(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcmp(s + a - b, suf); }

static int read_at(uint8_t *buf, uint64_t off, uint64_t size) {
    for (uint64_t got = 0; got < size;) {
        uint32_t want = size - got > (1u << 30) ? 1u << 30 : (uint32_t)(size - got);
        int64_t r = plat_file_pread(bimage, buf + got, want, off + got);
        if (r <= 0) return 0;
        got += (uint64_t)r;
    }
    return 1;
}

// A zstd frame decompressed into a new buffer; NULL when it is not one.
static uint8_t *unzstd(const uint8_t *z, size_t zn, size_t *out_n) {
    unsigned long long raw = ZSTD_getFrameContentSize(z, zn);
    if (raw == ZSTD_CONTENTSIZE_ERROR || raw == ZSTD_CONTENTSIZE_UNKNOWN || raw > (1ull << 32)) return NULL;
    uint8_t *d = malloc(raw ? raw : 1);
    if (!d || ZSTD_isError(ZSTD_decompress(d, raw, z, zn))) { free(d); return NULL; }
    *out_n = raw;
    return d;
}

static void do_file(const BFile *f, uint8_t **buf, size_t *bcap) {
    if (f->size < 0x50) return;
    if (*bcap < f->size) {
        free(*buf);
        if (!(*buf = malloc(*bcap = f->size))) rt_fatal("bc7: out of memory for a %llu byte file", (unsigned long long)f->size);
    }
    uint8_t *b = *buf;
    if (!read_at(b, f->off, f->size)) { __atomic_fetch_add(&bbad, 1, __ATOMIC_RELAXED); return; }
    if (ends(f->path, ".bntx")) { do_bntx(b, f->size); return; }
    if (ends(f->path, ".bntx.zs")) {
        size_t n;
        uint8_t *d = unzstd(b, f->size, &n);
        if (d) do_bntx(d, n); else __atomic_fetch_add(&bbad, 1, __ATOMIC_RELAXED);
        free(d);
        return;
    }
    if (memcmp(b + 4, "6PK0", 4)) { __atomic_fetch_add(&bbad, 1, __ATOMIC_RELAXED); return; }
    uint16_t hs, w, h, layers, fmt; uint32_t info;
    memcpy(&hs, b, 2); memcpy(&w, b + 8, 2); memcpy(&h, b + 0xA, 2); memcpy(&layers, b + 0xC, 2);
    memcpy(&fmt, b + 0x3C, 2); memcpy(&info, b + 0x44, 4);
    uint8_t mips = b[0xE];
    if ((fmt >> 8) != 1 || (info & 0xFFFFFF00u) != 0x7F00) return;   // not ASTC: sampled as it is
    uint32_t bw = ((info >> 4) & 15) + 1, bh = (info & 15) + 1;
    if (bw * bh > MAX_AREA) return;
    uint32_t ns = (uint32_t)mips * (layers ? layers : 1);
    if (!ns || hs + (size_t)ns * 12 > f->size) { __atomic_fetch_add(&bbad, 1, __ATOMIC_RELAXED); return; }
    BTex tx = {0};
    tex_begin(&tx, bw, bh, fmt == 0x105 || fmt == 0x109);
    const uint8_t *ent = b + hs, *sizes = ent + ns * 4, *data = sizes + ns * 8;
    uint32_t bh0 = block_height0((h + bh - 1) / bh);
    for (uint32_t i = 0; i < ns; i++) {
        uint8_t level = ent[i * 4 + 2];
        uint32_t zs; memcpy(&zs, sizes + i * 8, 4);
        if (data + zs > b + f->size) { __atomic_fetch_add(&bbad, 1, __ATOMIC_RELAXED); break; }
        const uint8_t *z = data;
        data += zs;
        uint32_t lw = w >> level ? w >> level : 1, lh = h >> level ? h >> level : 1;
        size_t n;
        uint8_t *sw = unzstd(z, zs, &n);
        if (!sw) { __atomic_fetch_add(&bbad, 1, __ATOMIC_RELAXED); continue; }
        do_surface(&tx, sw, n, lw, lh, bw, bh, mip_block_height((lh + bh - 1) / bh, bh0));
        free(sw);
    }
    tex_end(&tx);
}

static PlatMutex bmu = PLAT_MUTEX_INIT;
static PlatCond bcv = PLAT_COND_INIT;
static int brunning;

static void bworker(void *unused) {
    (void)unused;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);   // the rest of the machine stays usable
    uint8_t *buf = NULL;
    size_t bcap = 0;
    for (int i; (i = __atomic_fetch_add(&bnext, 1, __ATOMIC_RELAXED)) < nbfiles;) {
        do_file(&bfiles[i], &buf, &bcap);
        __atomic_fetch_add(&bdone_bytes, bfiles[i].size, __ATOMIC_RELAXED);
        __atomic_fetch_add(&bdone, 1, __ATOMIC_RELAXED);
    }
    free(buf);
    plat_mutex_lock(&bmu);
    brunning--;
    plat_cond_broadcast(&bcv);
    plat_mutex_unlock(&bmu);
}

static int by_size_desc(const void *a, const void *b) {   // large files first: the threads finish together
    uint64_t x = ((const BFile *)a)->size, y = ((const BFile *)b)->size;
    return x < y ? 1 : x > y ? -1 : 0;
}

int bc7_build(void) {
    const char *e = cfg_get("TOTK_BC7_QUALITY");
    bquality = e && *e ? atoi(e) : 2;
    e = cfg_get("TOTK_BC7_THREADS");
    int threads = e && *e ? atoi(e) : (int)plat_cpu_count() - 1;
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    if (!(bimage = plat_file_open(romfs_image_path()))) { fprintf(stderr, "bc7: cannot open %s\n", romfs_image_path()); return 1; }
    uint64_t n = romfs_file_count();
    bfiles = calloc(n ? n : 1, sizeof *bfiles);
    for (uint64_t i = 0; i < n; i++) {
        BFile f;
        if (!(f.path = romfs_entry(i, &f.off, &f.size))) continue;
        if ((strstr(f.path, "TexToGo/") && ends(f.path, ".txtg")) || ends(f.path, ".bntx") || ends(f.path, ".bntx.zs"))
            bfiles[nbfiles++] = f, btotal_bytes += f.size;
    }
    qsort(bfiles, (size_t)nbfiles, sizeof *bfiles, by_size_desc);
    plat_mutex_lock(&mu);
    int ok = open_files();
    plat_mutex_unlock(&mu);
    if (!ok) return 1;
    printf("bc7: %d texture files (%.0f MiB), %d threads, quality %d -> %s\n", nbfiles, btotal_bytes / 1048576.0, threads,
           bquality, cache_dir());
    fflush(stdout);
    uint64_t t0 = plat_now_ms();
    brunning = threads;
    for (int i = 0; i < threads; i++)
        if (!plat_thread_spawn(bworker, NULL, "bc7-build", 0)) rt_fatal("bc7: cannot start a build thread");
    plat_mutex_lock(&bmu);
    while (brunning) {   // a progress line every 2 s (the installer shows the last one)
        plat_cond_wait_ms(&bcv, &bmu, 2000);
        printf("bc7: %d / %d files, %llu / %llu bytes, %llu surfaces encoded\n", __atomic_load_n(&bdone, __ATOMIC_RELAXED),
               nbfiles, (unsigned long long)__atomic_load_n(&bdone_bytes, __ATOMIC_RELAXED),
               (unsigned long long)btotal_bytes, (unsigned long long)__atomic_load_n(&bencoded, __ATOMIC_RELAXED));
        fflush(stdout);
    }
    plat_mutex_unlock(&bmu);
    printf("bc7: done in %llu s: %llu surfaces (%llu encoded, %llu of them BC1; %llu already there), %llu unreadable; "
           "cache %zu surfaces\n", (unsigned long long)((plat_now_ms() - t0) / 1000), (unsigned long long)bsurfaces,
           (unsigned long long)bencoded, (unsigned long long)bbc1, (unsigned long long)bhad, (unsigned long long)bbad, count);
    fclose(pack), fclose(idx);
    return 0;
}
