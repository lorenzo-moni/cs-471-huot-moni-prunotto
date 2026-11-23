/*
 * Static quantum worker assignment, independent of live thread ownership.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TCG_QUANTUM_WORKER_MAP_H
#define TCG_QUANTUM_WORKER_MAP_H

#include "qemu/typedefs.h"

/* Mirrors the current parallel quantum loop's cpu_index < 128 limit. */
#define QUANTUM_WORKER_MAP_MAX_VCPUS 128u

/*
 * Describes dense vCPU indices [0, n_vcpus) in equal contiguous groups.
 * Worker IDs are scheduling indices, not CPU indices, Linux TIDs or core IDs.
 * This object owns no CPUState, QemuThread, RCU registration or barrier slot.
 * Initialize before publication; readers may share an immutable map. No BQL
 * is needed for the map itself. Do not mutate it while readers can access it.
 *
 * Future binding to live CPUs must validate dense, fixed CPU membership under
 * the BQL/CPU-list synchronization. The map grants no access to CPUState:
 * execution fields remain owned by the actual execution thread, other fields
 * retain their existing locks/atomics, and CPU callbacks require the target's
 * current_cpu context with restoration before returning to a sibling.
 * No CPU pointer is retained here that could outlive CPU removal.
 */
typedef struct QuantumWorkerMap {
    unsigned int n_vcpus;
    unsigned int n_workers;
    unsigned int vcpus_per_worker;
} QuantumWorkerMap;

/* Zero workers selects n_vcpus workers. Failure leaves *map unchanged. */
bool quantum_worker_map_init(QuantumWorkerMap *map, unsigned int n_vcpus,
                            unsigned int n_workers, Error **errp);

/* Accessors require a successfully initialized map and in-range indices. */
unsigned int quantum_worker_map_owner(const QuantumWorkerMap *map,
                                     unsigned int cpu_index);
unsigned int quantum_worker_map_cpu(const QuantumWorkerMap *map,
                                   unsigned int worker_id, unsigned int slot);

#endif /* TCG_QUANTUM_WORKER_MAP_H */
