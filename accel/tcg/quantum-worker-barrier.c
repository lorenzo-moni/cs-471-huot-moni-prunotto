/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "quantum-worker-barrier.h"

bool quantum_worker_barrier_init(QuantumWorkerBarrier *b, unsigned int workers,
                                 Error **errp)
{
    unsigned int i;

    if (!workers || workers > QUANTUM_WORKER_MAP_MAX_VCPUS) {
        error_setg(errp, "worker barrier: worker count must be in [1, %u]",
                   QUANTUM_WORKER_MAP_MAX_VCPUS);
        return false;
    }
    memset(b, 0, sizeof(*b));
    qemu_mutex_init(&b->lock);
    qemu_cond_init(&b->changed);
    b->n_workers = workers;
    b->departures = workers;
    for (i = 0; i < workers; i++) {
        b->arrived_generation[i] = UINT64_MAX;
    }
    return true;
}

bool quantum_worker_barrier_register(QuantumWorkerBarrier *b, unsigned int id,
                                     Error **errp)
{
    bool ok = false;

    qemu_mutex_lock(&b->lock);
    if (b->started || id >= b->n_workers || b->member[id]) {
        error_setg(errp, "worker barrier: invalid or duplicate registration");
    } else {
        b->member[id] = true;
        b->registered++;
        ok = true;
    }
    qemu_mutex_unlock(&b->lock);
    return ok;
}

bool quantum_worker_barrier_start(QuantumWorkerBarrier *b, Error **errp)
{
    bool ok = false;

    qemu_mutex_lock(&b->lock);
    if (b->started || b->registered != b->n_workers) {
        error_setg(errp, "worker barrier: start requires all workers once");
    } else {
        b->started = true;
        ok = true;
    }
    qemu_mutex_unlock(&b->lock);
    return ok;
}

/* Called with the lock held and returns with it held. */
static void progress_locked(QuantumWorkerBarrier *b, unsigned int id,
                            uint64_t generation,
                            const QuantumWorkerBarrierOps *ops, void *opaque)
{
    if (ops && ops->progress) {
        qemu_mutex_unlock(&b->lock);
        ops->progress(opaque, id, generation);
        qemu_mutex_lock(&b->lock);
    }
}

bool quantum_worker_barrier_wait(QuantumWorkerBarrier *b, unsigned int id,
                                 uint64_t generation, bool timer_touched,
                                 const QuantumWorkerBarrierOps *ops,
                                 void *opaque,
                                 QuantumWorkerBarrierResult *result,
                                 Error **errp)
{
    bool last;
    bool touched;

    qemu_mutex_lock(&b->lock);
    if (!b->started || id >= b->n_workers || !b->member[id] ||
        b->in_wait[id] || generation != b->generation ||
        b->arrived_generation[id] == generation || generation == UINT64_MAX ||
        b->published.stop_epoch > b->acknowledged_stop) {
        error_setg(errp,
                   "worker barrier: invalid, duplicate or stopped arrival");
        qemu_mutex_unlock(&b->lock);
        return false;
    }
    b->in_wait[id] = true;

    /* Keep the previous published result stable until all consumers leave. */
    while (b->departures != b->n_workers) {
        progress_locked(b, id, generation, ops, opaque);
        if (b->departures != b->n_workers) {
            qemu_cond_timedwait(&b->changed, &b->lock, 1);
        }
    }
    b->arrived_generation[id] = generation;
    b->timer_touched |= timer_touched;
    b->arrivals++;
    last = b->arrivals == b->n_workers;

    if (last) {
        assert(!b->releasing);
        b->releasing = true;
        touched = b->timer_touched;
        qemu_mutex_unlock(&b->lock);
        if (ops && ops->release) {
            ops->release(opaque, generation, touched);
        }
        qemu_mutex_lock(&b->lock);
        assert(b->generation == generation && b->arrivals == b->n_workers);
        b->published = (QuantumWorkerBarrierResult) {
            .generation = generation + 1,
            .stop_epoch = b->requested_stop > b->acknowledged_stop ?
                          b->requested_stop : 0,
            .timer_touched = touched,
        };
        b->arrivals = 0;
        b->departures = 0;
        b->timer_touched = false;
        b->releasing = false;
        /* The mutex publishes payloads and stop state together. */
        b->generation = generation + 1;
        qemu_cond_broadcast(&b->changed);
    } else {
        while (b->generation == generation) {
            progress_locked(b, id, generation, ops, opaque);
            if (b->generation == generation) {
                qemu_cond_timedwait(&b->changed, &b->lock, 1);
            }
        }
    }

    *result = b->published;
    result->last_arriver = last;
    b->departures++;
    b->in_wait[id] = false;
    qemu_cond_broadcast(&b->changed);
    qemu_mutex_unlock(&b->lock);
    return true;
}

bool quantum_worker_barrier_request_stop(QuantumWorkerBarrier *b,
                                         uint64_t *epoch, Error **errp)
{
    qemu_mutex_lock(&b->lock);
    if (b->requested_stop == UINT64_MAX) {
        error_setg(errp, "worker barrier: stop epoch overflow");
        qemu_mutex_unlock(&b->lock);
        return false;
    }
    *epoch = ++b->requested_stop;
    qemu_cond_broadcast(&b->changed);
    qemu_mutex_unlock(&b->lock);
    return true;
}

bool quantum_worker_barrier_acknowledge_stop(QuantumWorkerBarrier *b,
                                             uint64_t epoch, Error **errp)
{
    unsigned int i;
    bool busy = false;

    qemu_mutex_lock(&b->lock);
    for (i = 0; i < b->n_workers; i++) {
        busy |= b->in_wait[i];
    }
    if (!epoch || epoch != b->published.stop_epoch ||
        epoch <= b->acknowledged_stop || busy || b->arrivals || b->releasing ||
        b->departures != b->n_workers) {
        error_setg(errp, "worker barrier: stop is not ready to acknowledge");
        qemu_mutex_unlock(&b->lock);
        return false;
    }
    b->acknowledged_stop = epoch;
    qemu_cond_broadcast(&b->changed);
    qemu_mutex_unlock(&b->lock);
    return true;
}

uint64_t quantum_worker_barrier_generation(QuantumWorkerBarrier *b)
{
    uint64_t generation;

    qemu_mutex_lock(&b->lock);
    generation = b->generation;
    qemu_mutex_unlock(&b->lock);
    return generation;
}

void quantum_worker_barrier_destroy(QuantumWorkerBarrier *b)
{
    unsigned int i;

    assert(!b->arrivals && !b->releasing);
    for (i = 0; i < b->n_workers; i++) {
        assert(!b->in_wait[i]);
    }
    qemu_cond_destroy(&b->changed);
    qemu_mutex_destroy(&b->lock);
}
