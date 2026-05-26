/**
 * Copyright OpenSearch Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * BF16 + AMX accelerated distance computation for HNSW search.
 * Ported from hnswlib-amx project.
 *
 * Platform: requires Linux x86_64 with AMX/AVX512-BF16 support.
 */

#include "bf16_distance_amx.h"

#if defined(__linux__) && defined(__x86_64__)

#include <cpuid.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__AVX512BF16__)
#include <immintrin.h>
#endif

#if defined(__AMX_BF16__)
#include <immintrin.h>
#endif

#include <faiss/impl/FaissAssert.h>

namespace faiss {

// ============================================================
// CPU feature detection
// ============================================================

bool cpu_has_amx_bf16() {
    // CPUID leaf 7, sub-leaf 0, EDX bit 22 = AMX-BF16
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return (edx >> 22) & 1;
    }
    return false;
}

bool request_amx_permission() {
    static bool requested = false;
    static bool granted = false;
    if (requested) return granted;
    requested = true;

    const unsigned long ARCH_REQ_XCOMP_PERM = 0x1023;
    const unsigned long XFEATURE_XTILECFG = 17;
    const unsigned long XFEATURE_XTILEDATA = 18;

    // Must request both XTILECFG and XTILEDATA permissions
    long ret1 = syscall(SYS_arch_prctl, ARCH_REQ_XCOMP_PERM, XFEATURE_XTILECFG);
    long ret2 = syscall(SYS_arch_prctl, ARCH_REQ_XCOMP_PERM, XFEATURE_XTILEDATA);
    granted = (ret1 == 0 && ret2 == 0);
    return granted;
}

// ============================================================
// Scalar fallback
// ============================================================

float bf16_inner_product_scalar(
    const uint16_t* query_bf16,
    const uint16_t* candidate_bf16,
    size_t dim)
{
    float dot = 0.0f;
    for (size_t d = 0; d < dim; d++) {
        dot += bf16_to_float(query_bf16[d]) * bf16_to_float(candidate_bf16[d]);
    }
    return dot;
}

// ============================================================
// AVX512-BF16 implementations
// ============================================================

#if defined(__AVX512BF16__)

float bf16_inner_product_avx512(
    const uint16_t* query_bf16,
    const uint16_t* candidate_bf16,
    size_t dim)
{
    __m512 acc0 = _mm512_setzero_ps();
    __m512 acc1 = _mm512_setzero_ps();

    size_t d = 0;
    // Main loop: process 64 BF16 elements (2×32) per iteration
    size_t dim64 = dim & ~63ULL;
    for (; d < dim64; d += 64) {
        __m512bh q0 = (__m512bh)_mm512_loadu_si512(query_bf16 + d);
        __m512bh c0 = (__m512bh)_mm512_loadu_si512(candidate_bf16 + d);
        acc0 = _mm512_dpbf16_ps(acc0, q0, c0);

        __m512bh q1 = (__m512bh)_mm512_loadu_si512(query_bf16 + d + 32);
        __m512bh c1 = (__m512bh)_mm512_loadu_si512(candidate_bf16 + d + 32);
        acc1 = _mm512_dpbf16_ps(acc1, q1, c1);
    }
    // Handle remaining 32-element chunk
    if (d + 32 <= dim) {
        __m512bh q0 = (__m512bh)_mm512_loadu_si512(query_bf16 + d);
        __m512bh c0 = (__m512bh)_mm512_loadu_si512(candidate_bf16 + d);
        acc0 = _mm512_dpbf16_ps(acc0, q0, c0);
        d += 32;
    }
    acc0 = _mm512_add_ps(acc0, acc1);
    float dot = _mm512_reduce_add_ps(acc0);

    // Scalar tail for remaining elements
    for (; d < dim; d++) {
        dot += bf16_to_float(query_bf16[d]) * bf16_to_float(candidate_bf16[d]);
    }
    return dot;
}

void bf16_inner_product_batch_avx512(
    const uint16_t* query_bf16,
    const uint16_t* const* candidate_ptrs,
    float* dots_out,
    size_t count,
    size_t dim)
{
    for (size_t i = 0; i < count; i++) {
        dots_out[i] = bf16_inner_product_avx512(query_bf16, candidate_ptrs[i], dim);
    }
}

#else // no AVX512BF16

float bf16_inner_product_avx512(
    const uint16_t* query_bf16,
    const uint16_t* candidate_bf16,
    size_t dim)
{
    return bf16_inner_product_scalar(query_bf16, candidate_bf16, dim);
}

void bf16_inner_product_batch_avx512(
    const uint16_t* query_bf16,
    const uint16_t* const* candidate_ptrs,
    float* dots_out,
    size_t count,
    size_t dim)
{
    for (size_t i = 0; i < count; i++) {
        dots_out[i] = bf16_inner_product_scalar(query_bf16, candidate_ptrs[i], dim);
    }
}

#endif // __AVX512BF16__

// ============================================================
// AMX-BF16 implementations
// ============================================================

#if defined(__AMX_BF16__)

void amx_tile_config_batch16() {
    AmxTileConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.palette_id = 1;
    // Tile 0: candidates (16 rows × 64 bytes = 16 candidates × 32 BF16)
    cfg.rows[0] = 16;
    cfg.colsb[0] = 64;
    // Tile 1: query pairs (16 rows × 4 bytes)
    cfg.rows[1] = 16;
    cfg.colsb[1] = 4;
    // Tile 2: accumulators (16 rows × 4 bytes = 16 FP32)
    cfg.rows[2] = 16;
    cfg.colsb[2] = 4;
    _tile_loadconfig(&cfg);
}

void amx_tile_release() {
    _tile_release();
}

void bf16_inner_product_batch_amx(
    const uint16_t* query_bf16,
    const uint16_t* const* candidate_ptrs,
    float* dots_out,
    size_t count,
    size_t dim)
{
    // Zero accumulator tile
    _tile_zero(2);

    // Contiguous tile A buffer: 16 × 64 bytes = 1 KB, fits in L1
    alignas(64) uint8_t tile_a[16 * 64];

    // Zero-fill unused rows once
    if (count < 16) {
        memset(tile_a + count * 64, 0, (16 - count) * 64);
    }

    // Process dim in chunks of 32 BF16 elements
    size_t dim32 = dim & ~31ULL;
    for (size_t d = 0; d < dim32; d += 32) {
        // Pack tile A: copy each candidate's 64-byte chunk
        for (size_t n = 0; n < count; n++) {
            memcpy(tile_a + n * 64, candidate_ptrs[n] + d, 64);
        }
        _tile_loadd(0, tile_a, 64);

        // Tile B: query's 32 BF16 as 16 rows of 4 bytes (stride=4)
        _tile_loadd(1, query_bf16 + d, 4);

        // C += A × B
        _tile_dpbf16ps(2, 0, 1);
    }

    // Handle tail (dim % 32) with scalar
    float tail_dots[16] = {0};
    if (dim32 < dim) {
        for (size_t n = 0; n < count; n++) {
            for (size_t d = dim32; d < dim; d++) {
                tail_dots[n] += bf16_to_float(query_bf16[d]) *
                                bf16_to_float(candidate_ptrs[n][d]);
            }
        }
    }

    // Extract results from tile C
    alignas(64) float tile_c[16];
    _tile_stored(2, tile_c, 4);

    for (size_t n = 0; n < count; n++) {
        dots_out[n] = tile_c[n] + tail_dots[n];
    }
}

#else // no AMX_BF16

void amx_tile_config_batch16() {}
void amx_tile_release() {}

void bf16_inner_product_batch_amx(
    const uint16_t* query_bf16,
    const uint16_t* const* candidate_ptrs,
    float* dots_out,
    size_t count,
    size_t dim)
{
    // Fallback to AVX512 or scalar
    bf16_inner_product_batch_avx512(query_bf16, candidate_ptrs, dots_out, count, dim);
}

#endif // __AMX_BF16__

// ============================================================
// High-level batch distance (ID-based)
// ============================================================

void bf16_batch_distances(
    const uint16_t* query_bf16,
    const uint16_t* bf16_data,
    const idx_t* candidate_ids,
    float* distances_out,
    size_t count,
    size_t dim,
    bool use_amx,
    bool is_ip,
    const float* bf16_norms,
    float query_norm_sq,
    bool tiles_configured)
{
    // L2 distance requires precomputed norms
    FAISS_THROW_IF_NOT_MSG(
        is_ip || bf16_norms != nullptr,
        "bf16_batch_distances: bf16_norms must be provided for L2 distance");

    // Build pointer array
    const uint16_t* ptrs[16];

    auto compute_batch = [&](size_t start, size_t batch_count) {
        for (size_t j = 0; j < batch_count; j++) {
            ptrs[j] = bf16_data + candidate_ids[start + j] * dim;
        }

        float dots[16];
        if (use_amx && batch_count >= 4) {
            bf16_inner_product_batch_amx(query_bf16, ptrs, dots, batch_count, dim);
        } else {
            bf16_inner_product_batch_avx512(query_bf16, ptrs, dots, batch_count, dim);
        }

        // Convert raw dot product to distance
        if (is_ip) {
            // Inner product: faiss convention uses negated distance (smaller=better)
            for (size_t j = 0; j < batch_count; j++) {
                distances_out[start + j] = -dots[j];
            }
        } else {
            // L2: d = ||q||² + ||x||² - 2*dot(q,x)
            for (size_t j = 0; j < batch_count; j++) {
                float norm_x = bf16_norms ? bf16_norms[candidate_ids[start + j]] : 0.0f;
                distances_out[start + j] = query_norm_sq + norm_x - 2.0f * dots[j];
            }
        }
    };

#if defined(__AMX_BF16__)
    if (use_amx) {
        if (!tiles_configured) {
            amx_tile_config_batch16();
        }

        // Process in batches of 16
        size_t i = 0;
        for (; i + 16 <= count; i += 16) {
            compute_batch(i, 16);
        }
        if (i < count) {
            compute_batch(i, count - i);
        }

        if (!tiles_configured) {
            amx_tile_release();
        }
        return;
    }
#endif

    // Non-AMX path: process in batches of 16 using AVX512
    size_t i = 0;
    for (; i + 16 <= count; i += 16) {
        compute_batch(i, 16);
    }
    if (i < count) {
        compute_batch(i, count - i);
    }
}

} // namespace faiss

#else // !(__linux__ && __x86_64__)

// Stub implementations for non-Linux/non-x86_64 platforms
namespace faiss {

bool cpu_has_amx_bf16() { return false; }
bool request_amx_permission() { return false; }

void bf16_inner_product_batch_amx(
    const uint16_t*, const uint16_t* const*, float*, size_t, size_t) {}
void bf16_inner_product_batch_avx512(
    const uint16_t*, const uint16_t* const*, float*, size_t count, size_t dim) {
    // Scalar fallback for non-x86_64
    // In practice this path should not be called; BF16 should be disabled.
}
void amx_tile_config_batch16() {}
void amx_tile_release() {}
void bf16_batch_distances(
    const uint16_t*, const uint16_t*, const idx_t*, float*,
    size_t, size_t, bool, bool, const float*, float, bool) {}

} // namespace faiss

#endif // __linux__ && __x86_64__
