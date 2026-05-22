/**
 * Copyright OpenSearch Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * BF16 + AMX accelerated distance computation for HNSW search.
 * Ported from hnswlib-amx project.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace faiss {

// ============================================================
// Utility functions
// ============================================================

/// Check if CPU supports AMX-BF16 instructions
bool cpu_has_amx_bf16();

/// Request AMX tile permission from Linux kernel (must call once per process)
bool request_amx_permission();

/// Convert FP32 array to BF16 (truncation, no rounding)
inline void float_to_bf16(const float* src, uint16_t* dst, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t bits;
        memcpy(&bits, &src[i], sizeof(bits));
        dst[i] = static_cast<uint16_t>(bits >> 16);
    }
}

/// Convert single BF16 to FP32
inline float bf16_to_float(uint16_t value) {
    uint32_t bits = static_cast<uint32_t>(value) << 16;
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

// ============================================================
// Distance computation kernels
// ============================================================

/// AVX512-BF16 single-pair inner product (1 query × 1 candidate)
/// Returns raw dot product (not negated, not 1-dot)
float bf16_inner_product_avx512(
    const uint16_t* query_bf16,
    const uint16_t* candidate_bf16,
    size_t dim);

/// Scalar fallback for BF16 inner product
float bf16_inner_product_scalar(
    const uint16_t* query_bf16,
    const uint16_t* candidate_bf16,
    size_t dim);

/// AMX BF16 batch inner product: 1 query × up to 16 candidates
///
/// Uses swapped-tile layout to avoid vnni repacking:
///   Tile A (src1): count rows × 64 bytes = candidates' 32 BF16 elements
///   Tile B (src2): 16 rows × 4 bytes = query's 32 BF16 as pairs
///   Tile C (dst):  count rows × 4 bytes = FP32 dot-product accumulators
///
/// @param query_bf16       [dim] BF16 query vector
/// @param candidate_ptrs   array of count pointers to BF16 vectors
/// @param dots_out         [count] output raw dot products (FP32)
/// @param count            number of candidates (1-16)
/// @param dim              vector dimension (best if multiple of 32)
void bf16_inner_product_batch_amx(
    const uint16_t* query_bf16,
    const uint16_t* const* candidate_ptrs,
    float* dots_out,
    size_t count,
    size_t dim);

/// Batch inner product using AVX512-BF16 (fallback when AMX unavailable)
/// Same interface as AMX version
void bf16_inner_product_batch_avx512(
    const uint16_t* query_bf16,
    const uint16_t* const* candidate_ptrs,
    float* dots_out,
    size_t count,
    size_t dim);

// ============================================================
// AMX tile management
// ============================================================

/// AMX tile config structure — must be 64-byte aligned
struct __attribute__((aligned(64))) AmxTileConfig {
    uint8_t palette_id;
    uint8_t start_row;
    uint8_t reserved[14];
    uint16_t colsb[16];
    uint8_t rows[16];
};

/// Configure AMX tiles for batch-16 inner product computation.
/// Call once before a series of bf16_inner_product_batch_amx() calls.
void amx_tile_config_batch16();

/// Release AMX tile state. Call after batch computation is done.
void amx_tile_release();

// ============================================================
// High-level batch distance (ID-based, like hnswlib's computeBf16BatchDistancesInto)
// ============================================================

/// Compute distances for a batch of candidate IDs from contiguous BF16 storage.
///
/// @param query_bf16    [dim] BF16 query
/// @param bf16_data     [N × dim] contiguous BF16 vector storage
/// @param candidate_ids [count] candidate internal IDs
/// @param distances_out [count] output distances
/// @param count         number of candidates
/// @param dim           vector dimension
/// @param use_amx       whether to use AMX (else AVX512)
/// @param is_ip         true=inner product, false=L2
/// @param bf16_norms    [N] precomputed ||x||² (needed for L2, can be nullptr for IP)
/// @param query_norm_sq ||query||² (needed for L2)
void bf16_batch_distances(
    const uint16_t* query_bf16,
    const uint16_t* bf16_data,
    const int64_t* candidate_ids,
    float* distances_out,
    size_t count,
    size_t dim,
    bool use_amx,
    bool is_ip,
    const float* bf16_norms = nullptr,
    float query_norm_sq = 0.0f);

} // namespace faiss
