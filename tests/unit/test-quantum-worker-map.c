/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "accel/tcg/quantum-worker-map.h"

static void test_mapping(void)
{
    QuantumWorkerMap map;
    unsigned int n, m, cpu, worker, slot;

    /* All supported equal-group maps, including 8:4 contiguous pairs. */
    for (n = 1; n <= QUANTUM_WORKER_MAP_MAX_VCPUS; n++) {
        for (m = 1; m <= n; m++) {
            if (n % m) {
                continue;
            }
            g_assert_true(quantum_worker_map_init(&map, n, m, &error_abort));
            g_assert_cmpuint(map.n_vcpus, ==, n);
            g_assert_cmpuint(map.n_workers, ==, m);
            g_assert_cmpuint(map.vcpus_per_worker, ==, n / m);
            cpu = 0;
            for (worker = 0; worker < m; worker++) {
                for (slot = 0; slot < map.vcpus_per_worker; slot++, cpu++) {
                    g_assert_cmpuint(quantum_worker_map_cpu(&map, worker, slot),
                                     ==, cpu);
                    g_assert_cmpuint(quantum_worker_map_owner(&map, cpu),
                                     ==, worker);
                }
            }
            g_assert_cmpuint(cpu, ==, n);
        }
        g_assert_true(quantum_worker_map_init(&map, n, 0, &error_abort));
        g_assert_cmpuint(map.n_workers, ==, n);
        g_assert_cmpuint(map.vcpus_per_worker, ==, 1);
    }
}

static void test_invalid(void)
{
    const unsigned int invalid[][2] = {
        { 0, 0 }, { 0, 1 }, { 129, 0 }, { 8, 9 }, { 8, 3 },
        { UINT_MAX, 4 }, { 8, UINT_MAX },
    };
    QuantumWorkerMap map = { 8, 4, 2 };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(invalid); i++) {
        Error *err = NULL;

        g_assert_false(quantum_worker_map_init(&map, invalid[i][0],
                                              invalid[i][1], &err));
        g_assert_nonnull(err);
        error_free(err);
        g_assert_cmpuint(map.n_vcpus, ==, 8);
        g_assert_cmpuint(map.n_workers, ==, 4);
        g_assert_cmpuint(map.vcpus_per_worker, ==, 2);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/quantum-worker-map/mapping", test_mapping);
    g_test_add_func("/quantum-worker-map/invalid", test_invalid);
    return g_test_run();
}
