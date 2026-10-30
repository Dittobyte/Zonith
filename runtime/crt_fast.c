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

// memcpy likewise: Wine's goes through its memmove (an SSE2 loop with overlap checks), 14-16% of the render thread on
// the Z1, mostly the replay's state copies and the mirror and uniform snapshot copies. AVX2 up to 2 KiB, rep movsb
// (fast on Zen 3 and later) above. Like the C standard, and unlike ucrtbase's, it does not handle overlap.
__attribute__((no_builtin("memcpy"))) void *memcpy(void *restrict dst, const void *restrict src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (n >= 2048) {
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
        return dst;
    }
    if (n >= 32) {   // 32-byte steps, then the last 32 bytes (overlapping the last step)
        for (size_t i = 0; i + 32 < n; i += 32) _mm256_storeu_si256((__m256i *)(d + i), _mm256_loadu_si256((const __m256i *)(s + i)));
        _mm256_storeu_si256((__m256i *)(d + n - 32), _mm256_loadu_si256((const __m256i *)(s + n - 32)));
    } else if (n >= 16) {
        __m128i a = _mm_loadu_si128((const __m128i *)s), b = _mm_loadu_si128((const __m128i *)(s + n - 16));
        _mm_storeu_si128((__m128i *)d, a), _mm_storeu_si128((__m128i *)(d + n - 16), b);
    } else if (n >= 8) {
        uint64_t a, b;
        __builtin_memcpy(&a, s, 8), __builtin_memcpy(&b, s + n - 8, 8);
        __builtin_memcpy(d, &a, 8), __builtin_memcpy(d + n - 8, &b, 8);
    } else if (n >= 4) {
        uint32_t a, b;
        __builtin_memcpy(&a, s, 4), __builtin_memcpy(&b, s + n - 4, 4);
        __builtin_memcpy(d, &a, 4), __builtin_memcpy(d + n - 4, &b, 4);
    } else if (n) {
        unsigned char a = s[0], b = s[n / 2], c = s[n - 1];
        d[0] = a, d[n / 2] = b, d[n - 1] = c;
    }
    return dst;
}
