/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <random>

#include "faiss/Index.h"
#include "faiss/IndexHNSW.h"
#include "faiss/IndexIVFPQFastScan.h"
#include "faiss/index_factory.h"
#include "faiss/index_io.h"
#include "test_util.h"

namespace {
    pthread_mutex_t temp_file_mutex = PTHREAD_MUTEX_INITIALIZER;
}

std::vector<float> generate_data(
        int d,
        int n,
        std::default_random_engine rng,
        std::uniform_real_distribution<float> u) {
    std::vector<float> vectors(n * d);
    for (size_t i = 0; i < n * d; i++) {
        vectors[i] = u(rng);
    }
    return vectors;
}

void assert_float_vectors_almost_equal(
        std::vector<float> a,
        std::vector<float> b) {
    float margin = 0.000001;
    ASSERT_EQ(a.size(), b.size());
    for (int i = 0; i < a.size(); i++) {
        ASSERT_NEAR(a[i], b[i], margin);
    }
}

/// Test case test precomputed table sharing for IVFPQ indices.
template <typename T> /// T represents class cast to use for index
void test_ivfpq_table_sharing(
        const std::string& index_description,
        const std::string& filename,
        faiss::MetricType metric) {
    // Setup the index:
    // 1. Build an index
    // 2. ingest random data
    // 3. serialize to disk
    int d = 32, n = 1000;
    std::default_random_engine rng(
            std::chrono::system_clock::now().time_since_epoch().count());
    std::uniform_real_distribution<float> u(0, 100);

    std::vector<float> index_vectors = generate_data(d, n, rng, u);
    std::vector<float> query_vectors = generate_data(d, n, rng, u);

    Tempfilename index_filename(&temp_file_mutex, filename);
    {
        std::unique_ptr<faiss::Index> index_writer(
                faiss::index_factory(d, index_description.c_str(), metric));

        index_writer->train(n, index_vectors.data());
        index_writer->add(n, index_vectors.data());
        faiss::write_index(index_writer.get(), index_filename.c_str());
    }

    // Load index from disk. Confirm that the sdc table is equal to 0 when
    // disable sdc is set
    std::unique_ptr<faiss::AlignedTable<float>> sharedAlignedTable(
            new faiss::AlignedTable<float>());
    int shared_use_precomputed_table = 0;
    int k = 10;
    std::vector<float> distances_test_a(k * n);
    std::vector<faiss::idx_t> labels_test_a(k * n);
    {
        std::vector<float> distances_baseline(k * n);
        std::vector<faiss::idx_t> labels_baseline(k * n);

        std::unique_ptr<T> index_read_pq_table_enabled(
                dynamic_cast<T*>(faiss::read_index(
                        index_filename.c_str(), faiss::IO_FLAG_READ_ONLY)));
        std::unique_ptr<T> index_read_pq_table_disabled(
                dynamic_cast<T*>(faiss::read_index(
                        index_filename.c_str(),
                        faiss::IO_FLAG_READ_ONLY |
                                faiss::IO_FLAG_SKIP_PRECOMPUTE_TABLE)));
        faiss::initialize_IVFPQ_precomputed_table(
                shared_use_precomputed_table,
                index_read_pq_table_disabled->quantizer,
                index_read_pq_table_disabled->pq,
                *sharedAlignedTable,
                index_read_pq_table_disabled->by_residual,
                index_read_pq_table_disabled->verbose);
        index_read_pq_table_disabled->set_precomputed_table(
                sharedAlignedTable.get(), shared_use_precomputed_table);

        ASSERT_TRUE(index_read_pq_table_enabled->owns_precomputed_table);
        ASSERT_FALSE(index_read_pq_table_disabled->owns_precomputed_table);
        index_read_pq_table_enabled->search(
                n,
                query_vectors.data(),
                k,
                distances_baseline.data(),
                labels_baseline.data());
        index_read_pq_table_disabled->search(
                n,
                query_vectors.data(),
                k,
                distances_test_a.data(),
                labels_test_a.data());

        assert_float_vectors_almost_equal(distances_baseline, distances_test_a);
        ASSERT_EQ(labels_baseline, labels_test_a);
    }

    // The precomputed table should only be set for L2 metric type
    if (metric == faiss::METRIC_L2) {
        ASSERT_EQ(shared_use_precomputed_table, 1);
    } else {
        ASSERT_EQ(shared_use_precomputed_table, 0);
    }

    // At this point, the original has gone out of scope, the destructor has
    // been called. Confirm that initializing a new index from the table
    // preserves the functionality.
    {
        std::vector<float> distances_test_b(k * n);
        std::vector<faiss::idx_t> labels_test_b(k * n);

        std::unique_ptr<T> index_read_pq_table_disabled(
                dynamic_cast<T*>(faiss::read_index(
                        index_filename.c_str(),
                        faiss::IO_FLAG_READ_ONLY |
                                faiss::IO_FLAG_SKIP_PRECOMPUTE_TABLE)));
        index_read_pq_table_disabled->set_precomputed_table(
                sharedAlignedTable.get(), shared_use_precomputed_table);
        ASSERT_FALSE(index_read_pq_table_disabled->owns_precomputed_table);
        index_read_pq_table_disabled->search(
                n,
                query_vectors.data(),
                k,
                distances_test_b.data(),
                labels_test_b.data());
        assert_float_vectors_almost_equal(distances_test_a, distances_test_b);
        ASSERT_EQ(labels_test_a, labels_test_b);
    }
}

TEST(TestIVFPQTableSharing, L2) {
    test_ivfpq_table_sharing<faiss::IndexIVFPQ>(
            "IVF16,PQ8x4", "/tmp/ivfpql2", faiss::METRIC_L2);
}

TEST(TestIVFPQTableSharing, IP) {
    test_ivfpq_table_sharing<faiss::IndexIVFPQ>(
            "IVF16,PQ8x4", "/tmp/ivfpqip", faiss::METRIC_INNER_PRODUCT);
}

TEST(TestIVFPQTableSharing, FastScanL2) {
    test_ivfpq_table_sharing<faiss::IndexIVFPQFastScan>(
            "IVF16,PQ8x4fsr", "/tmp/ivfpqfsl2", faiss::METRIC_L2);
}

TEST(TestIVFPQTableSharing, FastScanIP) {
    test_ivfpq_table_sharing<faiss::IndexIVFPQFastScan>(
            "IVF16,PQ8x4fsr", "/tmp/ivfpqfsip", faiss::METRIC_INNER_PRODUCT);
}
