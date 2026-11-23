/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/plugin-pf.h"
#include "exec/exec-all.h"
#include "sysemu/runstate.h"
#include "sysemu/quantum.h"
#include "sysemu/quantum-stats.h"
#include "hw/boards.h"
#include "quantum-worker-barrier.h"
#include "hw/core/cpu.h"
#include "qemu/cpu-context.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "qemu/rcu.h"
#include "sysemu/cpus.h"
#include "tcg/tcg.h"
#include "tcg-accel-ops.h"
#include "tcg-accel-ops-quantum.h"
#include "quantum-worker-map.h"
#include "quantum-worker-creation.h"

/* All member state is protected by BQL. No member owns thread resources. */
typedef struct QuantumWorker {
    QemuThread thread;
    QemuCond halt_cond;
    QemuSemaphore wake;
    unsigned int id;
    bool started;
    bool paused;
    CPUState *last_selected;
    CPUState *members[2];
    Notifier force_rcu;
    int tid;
    bool sealed;
    CPUState *active; /* Atomic publication to kick callers; members never freed. */
    bool done[2];
    bool idle[2];
} QuantumWorker;

/* Process-lifetime allocation: never join, destroy, clear aliases or unref. */
static QuantumWorker *workers;
static QuantumWorkerMap worker_map;
static QuantumWorkerBarrier worker_barrier;

static void creation_force_rcu(Notifier *notifier, void *opaque)
{
    QuantumWorker *w = container_of(notifier, QuantumWorker, force_rcu);

    /* Called under rcu_registry_lock: do not acquire BQL or inspect CPUs. */
    qemu_sem_post(&w->wake);
}

/* BQL held, target current_cpu selected. Returns only outside cpu_exec. */
static void worker_dispatch(QuantumWorker *w, unsigned int slot)
{
    CPUState *cpu = w->members[slot];
    int ret;

    if (w->done[slot] || !cpu_can_run(cpu) || !runstate_is_running()) {
        return;
    }
    quantum_worker_execution_started = true;
    if (cpu->quantum_budget_in_picosecond <= 0) {
        w->done[slot] = true;
        return;
    }
    if (cpu->halted && !cpu_has_work(cpu)) {
        qemu_account_quantum_idle(cpu);
        w->idle[slot] = true;
        w->done[slot] = true;
        return;
    }
    qatomic_set(&cpu->exit_request, 0);
    cpu->quantum_budget_depleted = false;
    qatomic_set(&w->active, cpu);
    qemu_mutex_unlock_iothread();
    qstats_exec_begin();
    ret = tcg_cpus_exec(cpu);
    qstats_exec_end(cpu);
    qemu_mutex_lock_iothread();
    qatomic_set(&w->active, NULL);
    g_assert(!cpu->running && !cpu_in_exclusive_context(cpu));
    if (ret == EXCP_ATOMIC) {
        fprintf(stderr, "N worker: atomic fallback is unsupported (CPU %d)\n",
                cpu->cpu_index);
        _exit(EXIT_FAILURE);
    }
    if (ret == EXCP_YIELD) {
        cpu->qstats_yield_exits++;
    }
    if (ret == EXCP_DEBUG) {
        cpu_handle_guest_debug(cpu);
    }
    if (cpu->quantum_budget_in_picosecond <= 0) {
        w->done[slot] = true;
    }
}

/* Barrier invokes this outside its lock, with no guest/exclusive context. */
static void worker_progress(void *opaque, unsigned int id, uint64_t generation)
{
    QuantumWorker *w = opaque;
    unsigned int i;

    qemu_mutex_lock_iothread();
    if (!runstate_is_running()) {
        w->paused = true;
        if (qstats_self) {
            qstats_self->pending_flags |= QSTATS_STOP;
        }
    }
    for (i = 0; i < 2; i++) {
        CPUState *saved = cpu_context_save(&current_cpu, w->members[i]);
        qemu_wait_io_event_common(w->members[i]);
        cpu_context_restore(&current_cpu, saved);
    }
    qemu_mutex_unlock_iothread();
}

static void worker_release(void *opaque, uint64_t generation, bool touched)
{
    /* Exactly one last-arriver hook; no BQL or barrier lock held here.
     * Virtual time/timers are already advanced by first_cpu's timing helpers.
     */
    QuantumWorker *w = opaque;
    static uint64_t next_check;
    uint64_t cycle;
    bool requested = false;

    qstats_release_begin();
    if (generation >= UINT32_MAX) {
        fprintf(stderr, "N worker generation exceeds plugin API width\n");
        _exit(EXIT_FAILURE);
    }
    cycle = (generation + 1) * quantum_size;
    qemu_mutex_lock_iothread();
    if (!next_check) {
        next_check = quantum_check_threshold;
    }
    if (next_check && cycle >= next_check) {
        next_check += quantum_check_threshold;
        /* The main loop cannot consume SNAPSHOT_INFO until BQL is released. */
        quantum_worker_checkpoint_pending = true;
        requested = pf_periodic_check_cb && pf_periodic_check_cb(quantum_check_threshold);
        if (!requested) {
            quantum_worker_checkpoint_pending = false;
        }
    }
    qemu_mutex_unlock_iothread();
    if (requested) {
        int64_t deadline = g_get_monotonic_time() + 600 * G_TIME_SPAN_SECOND;
        qemu_notify_event();
        for (;;) {
            bool pending;
            /* This last arriver must also service work/stop during savevm. */
            worker_progress(w, w->id, generation);
            qemu_mutex_lock_iothread();
            pending = quantum_worker_checkpoint_pending;
            qemu_mutex_unlock_iothread();
            if (!pending) {
                break;
            }
            if (g_get_monotonic_time() >= deadline) {
                fprintf(stderr, "N worker checkpoint acknowledgment timed out\n");
                _exit(EXIT_FAILURE);
            }
            qemu_sem_timedwait(&w->wake, 1);
        }
    }
    /* All workers are still in this barrier; no guest can observe a torn epoch. */
    qemu_mutex_lock_iothread();
    qatomic_set(&quantum_worker_generation, (uint32_t)(generation + 1));
    if (quantum_worker_plugin_generation) {
        qatomic_set(quantum_worker_plugin_generation, generation + 1);
    }
    qemu_mutex_unlock_iothread();
    if (worker_map.n_workers > 1 && generation < 16) {
        fprintf(stderr, "N release=%" PRIu64 " arrivals=%u\n",
                generation + 1, worker_map.n_workers);
    }
    qstats_release_end();
}

static const QuantumWorkerBarrierOps worker_barrier_ops = {
    .release = worker_release,
    .progress = worker_progress,
};

/* All obligations are complete. No CPU is executing or in an exclusive region. */
static void worker_advance(QuantumWorker *w)
{
    QuantumWorkerBarrierResult result;
    uint64_t generation = w->members[0]->quantum_generation;
    bool touched = w->members[0]->touched_timer_during_last_quantum ||
                   w->members[1]->touched_timer_during_last_quantum;
    unsigned int i;
    CPUState *saved_context;

    g_assert(w->members[1]->quantum_generation == generation);
    if (worker_map.n_workers > 1 && generation < 16) {
        fprintf(stderr, "N arrival worker=%u generation=%" PRIu64 "\n",
                w->id, generation + 1);
    }
    w->members[0]->waiting_for_quantum = 1;
    w->members[1]->waiting_for_quantum = 1;
    saved_context = cpu_context_save(&current_cpu, w->members[0]);
    qstats_barrier_enter();
    qemu_mutex_unlock_iothread();
    quantum_worker_barrier_wait(&worker_barrier, w->id, generation, touched,
                               &worker_barrier_ops, w, &result, &error_abort);
    qemu_mutex_lock_iothread();
    qstats_barrier_exit(generation, w->paused ? QSTATS_STOP : 0);
    cpu_context_restore(&current_cpu, saved_context);
    if (result.generation <= (worker_map.n_workers == 1 ? 8 : 16)) {
        fprintf(stderr, "N generation=%" PRIu64 " boundary=%" PRIu64
                " insns0=%" PRIu64 " insns1=%" PRIu64 " idle=%d,%d\n",
                result.generation, result.generation,
                w->members[0]->qstats_insns, w->members[1]->qstats_insns,
                w->idle[0], w->idle[1]);
    }
    if (worker_map.n_workers > 1 && result.generation <= 16) {
        fprintf(stderr, "N worker=%u generation=%" PRIu64
                " cpus=%d,%d insns=%" PRIu64 ",%" PRIu64 " idle=%d,%d\n",
                w->id, result.generation, w->members[0]->cpu_index,
                w->members[1]->cpu_index, w->members[0]->qstats_insns,
                w->members[1]->qstats_insns, w->idle[0], w->idle[1]);
    }
    for (i = 0; i < 2; i++) {
        CPUState *saved = cpu_context_save(&current_cpu, w->members[i]);
        quantum_vcpu_advance(w->members[i], (uint32_t)result.generation,
                             0, w->idle[i]);
        w->members[i]->quantum_budget_depleted =
            w->members[i]->quantum_budget_in_picosecond <= 0;
        w->members[i]->waiting_for_quantum = 0;
        w->done[i] = false;
        w->idle[i] = false;
        cpu_context_restore(&current_cpu, saved);
    }
}

static void *creation_thread(void *opaque)
{
    QuantumWorker *w = opaque;
    unsigned int i;

    rcu_register_thread();
    w->force_rcu.notify = creation_force_rcu;
    rcu_add_force_rcu_notifier(&w->force_rcu);
    tcg_register_thread();
    qstats_thread_init(w->id, -1);

    qemu_mutex_lock_iothread();
    w->tid = qemu_get_thread_id();
    for (;;) {
        if (w->sealed && !runstate_is_running()) {
            w->paused = true;
            if (qstats_self) {
                qstats_self->pending_flags |= QSTATS_STOP;
            }
        }
        if (w->sealed && runstate_is_running()) {
            if (w->paused) {
                w->members[0]->qstats_paused = true;
                w->paused = false;
            }
            qstats_resume_if_needed(w->members[0]);
        }
        for (i = 0; i < 2; i++) {
            CPUState *cpu = w->members[i];
            CPUState *saved;

            if (!cpu) {
                continue;
            }
            if (w->last_selected && w->last_selected != cpu) {
                qstats_ctx_begin();
                saved = cpu_context_save(&current_cpu, cpu);
                qstats_ctx_end();
            } else {
                saved = cpu_context_save(&current_cpu, cpu);
            }
            w->last_selected = cpu;
            if (!cpu->created) {
                cpu->thread_id = w->tid;
                cpu->can_do_io = 1;
                quantum_vcpu_init_model(cpu);
                quantum_vcpu_init_time_state(cpu);
                quantum_vcpu_init_budget(cpu, 0);
                cpu->exit_request = 1;
                qstats_vcpu_attach(cpu);
                /* Real per-member signal; never wait for the sibling. */
                cpu_thread_signal_created(cpu);
            }
            g_assert(!cpu->running);
            qemu_wait_io_event_common(cpu);
            if (w->sealed) {
                worker_dispatch(w, i);
            }
            cpu_context_restore(&current_cpu, saved);
        }
        if (w->sealed && w->done[0] && w->done[1]) {
            worker_advance(w);
        }
        if (w->sealed && runstate_is_running() &&
            !w->members[0]->stopped && !w->members[1]->stopped) {
            /* Drop BQL between turns so QMP and stop requests can progress. */
            current_cpu = w->last_selected;
            qemu_mutex_unlock_iothread();
            qemu_mutex_lock_iothread();
            current_cpu = NULL;
            continue;
        }
        current_cpu = w->last_selected;
        qemu_mutex_unlock_iothread();
        /* Persistent wake tokens avoid a lost enqueue/kick wakeup. */
        qemu_sem_timedwait(&w->wake, 250);
        qemu_mutex_lock_iothread();
        current_cpu = NULL;
    }
    return NULL;
}

void quantum_worker_creation_attach(CPUState *cpu)
{
    QuantumWorker *w;
    unsigned int slot;
    Error *err = NULL;

    g_assert(qemu_mutex_iothread_locked());
    if (!quantum_vcpu_validate_model(cpu, &err)) {
        error_report_err(err);
        /* An earlier sibling may already have a process-lifetime worker. */
        _exit(EXIT_FAILURE);
    }
    if (!workers) {
        unsigned int id;
        quantum_worker_map_init(&worker_map, current_machine->smp.cpus,
                                quantum_workers, &error_abort);
        workers = g_new0(QuantumWorker, worker_map.n_workers);
        for (id = 0; id < worker_map.n_workers; id++) {
            workers[id].id = id;
            qemu_cond_init(&workers[id].halt_cond);
            qemu_sem_init(&workers[id].wake, 0);
        }
    }
    g_assert(cpu->cpu_index >= 0 && cpu->cpu_index < worker_map.n_vcpus);
    w = &workers[quantum_worker_map_owner(&worker_map, cpu->cpu_index)];
    slot = cpu->cpu_index % worker_map.vcpus_per_worker;
    g_assert(cpu->stopped && !cpu->created && !cpu->thread);
    g_assert(!w->members[slot] && !w->sealed);
    object_ref(OBJECT(cpu));
    cpu->worker_attached = true;

    w->members[slot] = cpu;
    cpu->thread = &w->thread;
    cpu->halt_cond = &w->halt_cond;
    {
        Error *reason = NULL;
        error_setg(&reason, "process-lifetime worker CPU cannot be unplugged");
        qdev_add_unplug_blocker(DEVICE(cpu), reason);
    }
    tcg_cpu_init_cflags(cpu, true);
    if (!w->started) {
        char name[VCPU_THREAD_NAME_SIZE];
        w->started = true;
        snprintf(name, sizeof(name), "QW%u", w->id);
        qemu_thread_create(&w->thread, name, creation_thread, w,
                           QEMU_THREAD_JOINABLE);
    } else {
        qemu_sem_post(&w->wake);
    }
}

void quantum_worker_creation_kick(CPUState *cpu)
{
    /* CPU and worker aliases must remain live for the process lifetime. */
    QuantumWorker *w = &workers[quantum_worker_map_owner(&worker_map,
                                                       cpu->cpu_index)];
    g_assert(cpu->thread == &w->thread);
    {
        CPUState *active = qatomic_read(&w->active);
        cpu_exit(cpu);
        if (active && active != cpu) {
            cpu_exit(active);
        }
    }
    qemu_sem_post(&w->wake);
}

static void creation_probe(CPUState *cpu, run_on_cpu_data data)
{
    g_assert(current_cpu == cpu);
    g_assert(qemu_cpu_is_self(cpu));
    g_assert(cpu->thread_id == qemu_get_thread_id());
    g_assert(cpu->created && cpu->stopped && !cpu->running);
    *(bool *)data.host_ptr = true;
}

bool quantum_worker_creation_seal(Error **errp)
{
    CPUState *cpu;
    unsigned int count = 0, id;
    bool done[8] = { false };
    QemuCond poll;
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    g_assert(qemu_mutex_iothread_locked());
    if (!workers) {
        error_setg(errp, "worker membership absent");
        return false;
    }
    CPU_FOREACH(cpu) {
        QuantumWorker *w;
        if (cpu->cpu_index < 0 || cpu->cpu_index >= worker_map.n_vcpus) {
            error_setg(errp, "invalid worker CPU index");
            return false;
        }
        w = &workers[quantum_worker_map_owner(&worker_map, cpu->cpu_index)];
        if (w->sealed || !w->tid || w->members[cpu->cpu_index % 2] != cpu ||
            !cpu->created || !cpu->stopped || cpu->running ||
            cpu->thread != &w->thread || cpu->halt_cond != &w->halt_cond ||
            cpu->thread_id != w->tid || cpu->quantum_generation != 0) {
            error_setg(errp, "invalid worker membership");
            return false;
        }
        count++;
    }
    if (count != worker_map.n_vcpus) {
        error_setg(errp, "incomplete worker membership");
        return false;
    }
    qemu_cond_init(&poll);
    CPU_FOREACH(cpu) {
        async_run_on_cpu(cpu, creation_probe,
                         RUN_ON_CPU_HOST_PTR(&done[cpu->cpu_index]));
    }
    for (;;) {
        unsigned int ready = 0;
        for (id = 0; id < count; id++) {
            ready += done[id];
        }
        if (ready == count) {
            break;
        }
        if (g_get_monotonic_time() >= deadline) {
            fprintf(stderr, "worker membership probe timed out\n");
            _exit(EXIT_FAILURE);
        }
        qemu_cond_timedwait_iothread(&poll, 100);
    }
    qemu_cond_destroy(&poll);
    quantum_worker_barrier_init(&worker_barrier, worker_map.n_workers, &error_abort);
    for (id = 0; id < worker_map.n_workers; id++) {
        quantum_worker_barrier_register(&worker_barrier, id, &error_abort);
    }
    quantum_worker_barrier_start(&worker_barrier, &error_abort);
    for (id = 0; id < worker_map.n_workers; id++) {
        workers[id].sealed = true;
        fprintf(stderr, "quantum worker %u paused-ready: CPUs %u,%u tid=%d\n",
                id, id * 2, id * 2 + 1, workers[id].tid);
    }
    return true;
}
