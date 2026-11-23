/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TCG_QUANTUM_WORKER_BARRIER_H
#define TCG_QUANTUM_WORKER_BARRIER_H

#include "qemu/typedefs.h"
#include "qemu/thread.h"
#include "quantum-worker-map.h"

/*
 * Non-activated protocol prototype. No CPUState, global quantum_barrier,
 * virtual clock, qstats or checkpoint code is accessed by this module.
 * Initialize once, register exactly n_workers distinct IDs, then start.
 * Membership is fixed; WFI must never register/unregister a participant.
 * Destroy only after all wait calls have returned and threads are joined.
 * Fields below are private to the implementation, despite being embeddable.
 */
typedef struct QuantumWorkerBarrierResult {
    uint64_t generation;       /* released generation, input generation + 1 */
    uint64_t stop_epoch;       /* zero if no pending request at release */
    bool timer_touched;        /* OR of all worker submissions */
    bool last_arriver;
} QuantumWorkerBarrierResult;

typedef struct QuantumWorkerBarrier {
    QemuMutex lock;
    QemuCond changed;
    unsigned int n_workers;
    unsigned int registered;
    unsigned int arrivals;
    unsigned int departures;
    bool started;
    bool releasing;
    bool timer_touched;
    bool member[QUANTUM_WORKER_MAP_MAX_VCPUS];
    bool in_wait[QUANTUM_WORKER_MAP_MAX_VCPUS];
    uint64_t arrived_generation[QUANTUM_WORKER_MAP_MAX_VCPUS];
    uint64_t generation;
    uint64_t requested_stop;
    uint64_t acknowledged_stop;
    QuantumWorkerBarrierResult published;
} QuantumWorkerBarrier;

/*
 * Both hooks execute WITHOUT the internal lock. release runs exactly once
 * after all arrivals and before publication. All workers must supply the same
 * release policy. progress is polled during waits
 * (including the previous generation's departure fence), at ~1 ms intervals.
 * Hooks must return, must not recursively wait/acknowledge this barrier, and
 * must not throw/longjmp. Only the protocol mutex is released: callers must
 * not hold BQL, CPU-list/work locks or an exclusive/RCU execution section
 * across wait(). Hooks acquire their own required locks.
 *
 * progress is for independently proven, bounded control work. It is NOT
 * permission to drain arbitrary QEMU work: synchronous/exclusive cycles can
 * still deadlock. There is deliberately no live CPU-work adapter here.
 * A future qstats adapter brackets wait once, and release only in its hook;
 * it must account progress as barrier residency without nested switch timing.
 */
typedef struct QuantumWorkerBarrierOps {
    void (*release)(void *opaque, uint64_t generation, bool timer_touched);
    void (*progress)(void *opaque, unsigned int worker_id, uint64_t generation);
} QuantumWorkerBarrierOps;

bool quantum_worker_barrier_init(QuantumWorkerBarrier *b, unsigned int workers,
                                 Error **errp);
bool quantum_worker_barrier_register(QuantumWorkerBarrier *b, unsigned int id,
                                     Error **errp);
bool quantum_worker_barrier_start(QuantumWorkerBarrier *b, Error **errp);
bool quantum_worker_barrier_wait(QuantumWorkerBarrier *b, unsigned int id,
                                 uint64_t generation, bool timer_touched,
                                 const QuantumWorkerBarrierOps *ops,
                                 void *opaque,
                                 QuantumWorkerBarrierResult *result,
                                 Error **errp);

/*
 * Stop requests are monotonic epochs, serialized with generation publication.
 * A request before publication is included there; one after publication stays
 * pending for a later release. A stop result prevents the next arrival until
 * acknowledged. acknowledge_stop() requires every worker to have copied that
 * release's result and left its internal wait state. Its caller must ALSO
 * establish actual VM quiescence;
 * departure from a synthetic barrier alone does not establish checkpoint
 * safety. Acknowledging an old epoch never clears a newer request.
 */
bool quantum_worker_barrier_request_stop(QuantumWorkerBarrier *b,
                                         uint64_t *epoch, Error **errp);
bool quantum_worker_barrier_acknowledge_stop(QuantumWorkerBarrier *b,
                                             uint64_t epoch, Error **errp);
uint64_t quantum_worker_barrier_generation(QuantumWorkerBarrier *b);
void quantum_worker_barrier_destroy(QuantumWorkerBarrier *b);

#endif /* TCG_QUANTUM_WORKER_BARRIER_H */
