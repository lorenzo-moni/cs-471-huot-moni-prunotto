/*
 * Static quantum worker assignment.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "quantum-worker-map.h"

bool quantum_worker_map_init(QuantumWorkerMap *map, unsigned int n_vcpus,
                            unsigned int n_workers, Error **errp)
{
    if (!n_vcpus || n_vcpus > QUANTUM_WORKER_MAP_MAX_VCPUS) {
        error_setg(errp, "quantum worker map: vCPU count must be in [1, %u]",
                   QUANTUM_WORKER_MAP_MAX_VCPUS);
        return false;
    }
    if (!n_workers) {
        n_workers = n_vcpus;
    }
    if (n_workers > n_vcpus || n_vcpus % n_workers) {
        error_setg(errp, "quantum worker map: worker count must not exceed "
                   "vCPU count and must divide it evenly");
        return false;
    }

    *map = (QuantumWorkerMap) {
        .n_vcpus = n_vcpus,
        .n_workers = n_workers,
        .vcpus_per_worker = n_vcpus / n_workers,
    };
    return true;
}

unsigned int quantum_worker_map_owner(const QuantumWorkerMap *map,
                                     unsigned int cpu_index)
{
    assert(map->vcpus_per_worker && cpu_index < map->n_vcpus);
    return cpu_index / map->vcpus_per_worker;
}

unsigned int quantum_worker_map_cpu(const QuantumWorkerMap *map,
                                   unsigned int worker_id, unsigned int slot)
{
    assert(worker_id < map->n_workers && slot < map->vcpus_per_worker);
    return worker_id * map->vcpus_per_worker + slot;
}
