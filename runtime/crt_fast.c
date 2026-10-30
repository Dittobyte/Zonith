// memcmp for the whole exe, in place of the ucrtbase import. Wine's ucrtbase memcmp compares 8 bytes a step in scalar
// code, and the replay compares its state blocks and keys with it at every draw: 5.7% of the render thread outdoors on a
// Ryzen Z1 under Proton (docs/proton.md). 32 bytes a step with AVX2 (the build targets x86-64-v3).
#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>

__attribute__((no_builtin("memcmp"))) int memcmp(const void *pa, const void *pb, size_t n) {
    const unsigned char *a = pa, *b = pb;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        __m256i x = _mm256_loadu_si256((const __m256i *)(a + i)), y = _mm256_loadu_si256((const __m256i *)(b + i));
        uint32_t ne = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, y));
        if (ne) return (int)a[i + __builtin_ctz(ne)] - (int)b[i + __builtin_ctz(ne)];
    }
    for (; i + 8 <= n; i += 8) {
        uint64_t x, y;
        __builtin_memcpy(&x, a + i, 8), __builtin_memcpy(&y, b + i, 8);
        if (x != y) {
            size_t k = i + (size_t)__builtin_ctzll(x ^ y) / 8;   // little endian: the lowest differing byte comes first
            return (int)a[k] - (int)b[k];
        }
    }
    for (; i < n; i++)
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    return 0;
}
