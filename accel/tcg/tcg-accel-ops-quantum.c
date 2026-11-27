/*
 * QEMU TCG Multi Threaded vCPUs implementation
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2014 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "sysemu/cpus.h"
#include "sysemu/runstate.h"
#include "sysemu/tcg.h"
#include "qemu/plugin-pf.h"
#include "qemu/typedefs.h"
#include "sysemu/replay.h"
#include "sysemu/cpu-timers.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "qemu/guest-random.h"
#include "qemu/error-report.h"
#include "qemu/plugin.h"
#include "exec/exec-all.h"
#include "hw/boards.h"
#include "tcg/tcg.h"
#include "tcg-accel-ops.h"
#include "tcg-accel-ops-quantum.h"
#include "qemu/log-for-trace.h"

#include "qemu/dynamic_barrier.h"
#include "sysemu/quantum.h"
#include "sysemu/quantum-stats.h"
#include <bits/time.h>
#include <stdio.h>

/*
 * Host threads of the quantum mode, with N:M time multiplexing.
 *
 * vCPUs whose rows of core_info.csv have the same host_core_idx share one
 * host thread pinned to that core; with distinct values every vCPU has its
 * own thread, as in plain parallel warming. A host thread runs one quantum at
 * a time:
 *
 *   prologue  poll every member (stop requests, queued work); a member that
 *             is idle (WFI) at the start of the quantum gives up the whole
 *             quantum, as with one vCPU per thread; block only while every
 *             member is stopped (VM paused or not started)
 *   rounds    run each member in turn until it has used its budget, goes
 *             idle or is stopped (one round per time slice)
 *   barrier   wait at the global barrier once, then advance every member
 *
 * The BQL is taken only on slow paths (exit requests, queued work, stop,
 * idle accounting, exits other than EXCP_QUANTUM): a busy vCPU goes
 * exec -> barrier -> exec without it, as in the former one-vCPU loop.
 */

#define QUANTUM_MAX_GROUP 32     /* vCPUs per host thread */
#define QUANTUM_MAX_VCPUS 128    /* see the cpu_index assertion below */

typedef struct QuantumHostThread {
    int id;                   /* host thread index: tid of the qstats files */
    int64_t host_core;        /* host core the thread is pinned to */
    int n_expected;           /* members among the vCPUs [0, max_cpus) */
    int n_attached;           /* published with release semantics */
    int n_stats_attached;     /* members passed to qstats_vcpu_attach() */
    CPUState *vcpus[QUANTUM_MAX_GROUP];   /* ascending cpu_index */
    CPUState *creator;        /* member whose realization created the thread */
    QemuThread *thread;
    QemuCond *halt_cond;
    Notifier force_rcu;
    uint64_t generation;      /* barrier generation when the thread registered */
} QuantumHostThread;

static QuantumHostThread quantum_host_threads[QUANTUM_MAX_VCPUS];

// Core info table for quantum mode
static core_meta_info_t core_info_table[256];

void quantum_initialize_core_info_table(const char *file_name) {
    tcg_parse_core_info_file(file_name, core_info_table, 256);
}

static void do_nothing(CPUState *cpu, run_on_cpu_data d)
{
}

/*
 * Called with rcu_registry_lock held, using async_run_on_cpu() ensures that
 * there are no deadlocks. The notifier belongs to the host thread (the RCU
 * reader), so it kicks every member: the one inside cpu_exec(), which holds
 * rcu_read_lock(), leaves it; the parked ones run a no-op at their next turn.
 */
static void quantum_force_rcu(Notifier *notify, void *data)
{
    QuantumHostThread *t = container_of(notify, QuantumHostThread, force_rcu);
    int n = qatomic_load_acquire(&t->n_attached);

    for (int i = 0; i < n; i++) {
        async_run_on_cpu(t->vcpus[i], do_nothing, RUN_ON_CPU_NULL);
    }
}

/* ---------------------------------------------------------------------------
 * Grouping of the vCPUs into host threads
 */

static bool quantum_same_core(int a, int b)
{
    return core_info_table[a].host_core_idx == core_info_table[b].host_core_idx;
}

/* True if no vCPU below @cpu_index shares its host core. */
static bool quantum_is_lowest_member(int cpu_index)
{
    for (int i = 0; i < cpu_index; i++) {
        if (quantum_same_core(i, cpu_index)) {
            return false;
        }
    }
    return true;
}

/*
 * Host thread index of @cpu_index. Groups are numbered by their lowest
 * member, so with one vCPU per host core the index is the cpu_index.
 */
static int quantum_group_id(int cpu_index)
{
    int lowest = cpu_index;
    int id = 0;

    for (int i = 0; i < cpu_index; i++) {
        if (quantum_same_core(i, cpu_index)) {
            lowest = i;
            break;
        }
    }
    for (int i = 0; i < lowest; i++) {
        if (quantum_is_lowest_member(i)) {
            id++;
        }
    }
    return id;
}

static void quantum_log_group(QuantumHostThread *t)
{
    GString *s = g_string_new(NULL);

    for (int i = 0; i < t->n_attached; i++) {
        g_string_append_printf(s, "%s%d", i ? "," : "", t->vcpus[i]->cpu_index);
    }
    info_report("quantum: host thread %d on host core %" PRId64 " runs vCPU%s %s",
                t->id, t->host_core, t->n_attached > 1 ? "s" : "", s->str);
    g_string_free(s, true);
}

/* Adds @cpu to the group of its host core. Called with the BQL held. */
static QuantumHostThread *quantum_attach_vcpu(CPUState *cpu)
{
    int max_cpus = current_machine->smp.max_cpus;
    QuantumHostThread *t;
    int pos;

    assert(cpu->cpu_index < QUANTUM_MAX_VCPUS);
    t = &quantum_host_threads[quantum_group_id(cpu->cpu_index)];

    if (t->n_expected == 0) {
        long host_cpus = sysconf(_SC_NPROCESSORS_CONF);

        t->id = quantum_group_id(cpu->cpu_index);
        t->host_core = core_info_table[cpu->cpu_index].host_core_idx;
        for (int i = 0; i < max_cpus && i < QUANTUM_MAX_VCPUS; i++) {
            if (quantum_same_core(i, cpu->cpu_index)) {
                t->n_expected++;
            }
        }
        if (t->n_expected > QUANTUM_MAX_GROUP) {
            error_report("quantum: %d vCPUs share host core %" PRId64
                         " (core_info.csv), at most %d are supported",
                         t->n_expected, t->host_core, QUANTUM_MAX_GROUP);
            exit(1);
        }
        if (t->host_core < 0 || (host_cpus > 0 && t->host_core >= host_cpus)) {
            warn_report("quantum: host core %" PRId64 " of vCPU %d does not "
                        "exist on this host (%ld CPUs)",
                        t->host_core, cpu->cpu_index, host_cpus);
        }
    }

    assert(t->n_attached < t->n_expected);
    /* Keep the members sorted by cpu_index (they are realized in order). */
    pos = t->n_attached;
    while (pos > 0 && t->vcpus[pos - 1]->cpu_index > cpu->cpu_index) {
        t->vcpus[pos] = t->vcpus[pos - 1];
        pos--;
    }
    t->vcpus[pos] = cpu;
    qatomic_store_release(&t->n_attached, t->n_attached + 1);

    if (t->n_attached == t->n_expected) {
        quantum_log_group(t);
    }
    return t;
}

/*
 * Per-vCPU quantum state, set before the VM runs: by the host thread for the
 * member that created it, by quantum_start_vcpu_thread() for the others.
 */

/*
 * Slice floor at the start of a quantum that begins with @budget_ps: the
 * first slice ends when the budget has fallen by one slice. The floor is 0
 * when the slice is the whole quantum (one turn per quantum).
 */
static int64_t quantum_first_slice_floor(int64_t budget_ps)
{
    if (quantum_slice_size >= quantum_size) {
        return 0;
    }
    return MAX(0, budget_ps - (int64_t)quantum_slice_size * 1000);
}

static void quantum_vcpu_init(CPUState *cpu, uint64_t generation)
{
    // Copy core info from table to CPUState
    cpu->ip100ns = (uint64_t)(core_info_table[cpu->cpu_index].ipns * 100);
    cpu->active_coeffs = core_info_table[cpu->cpu_index].coeffs;

    /* Record whether this is an ipc-model core (enables ASID switching). */
    cpu->is_ipc_model = !core_model_is_constant(&core_info_table[cpu->cpu_index]);

    /* Save defaults so the TTBR handler can fall back to them. */
    cpu->default_coeffs = cpu->active_coeffs;

    cpu->can_do_io = 1;

    // initialize the field that are related to the time.
    cpu->unknown_time = 0;
    cpu->enter_idle_time = 0;
    cpu->target_cycle_on_idle = 0;
    cpu->target_cycle_on_instruction = 0;
    cpu->touched_timer_during_last_quantum = 0;

    cpu->sgi_sender_time_ns_valid = false;
    cpu->sgi_sender_remaining_time_ns = 0;
    cpu->sgi_sender_quantum_generation = 0;

    cpu->waiting_for_quantum = 0;
    cpu->wakeup_during_quantum_spinning = 0;
    cpu->wakeup_while_given_ts_is_smaller_than_before = 0;

    bool affiliated_with_quantum = cpu->ip100ns && quantum_enabled();
    assert(affiliated_with_quantum);

    cpu->quantum_generation = generation;
    /* Budget is always in picoseconds: quantum_size (ns) * 1000 = ps. */
    cpu->quantum_budget_in_picosecond = (int64_t)quantum_size * 1000;
    assert(cpu->quantum_budget_in_picosecond > 0);
    cpu->last_tb_instruction_count_for_quantum = 0;
    cpu->quantum_budget_depleted = 0;
    cpu->quantum_slice_floor_ps = quantum_first_slice_floor(cpu->quantum_budget_in_picosecond);
    cpu->quantum_done = false;
    cpu->quantum_done_idle = false;

    qemu_log("======================================\n");
    qemu_log("Core%u Quantum Count: %lu ns.\n", cpu->cpu_index, quantum_size);
    // print coefficients
    qemu_log("Core%u bx_private_icache_miss_coeff: %"PRIu32"\n", cpu->cpu_index, cpu->active_coeffs.bx_private_icache_miss_coeff);
    qemu_log("Core%u bx_private_dcache_miss_coeff: %"PRIu32"\n", cpu->cpu_index, cpu->active_coeffs.bx_private_dcache_miss_coeff);
    qemu_log("Core%u bx_shared_cache_miss_coeff: %"PRIu32"\n", cpu->cpu_index, cpu->active_coeffs.bx_shared_cache_miss_coeff);
    qemu_log("Core%u bx_bp_miss_coeff: %"PRIu32"\n", cpu->cpu_index, cpu->active_coeffs.bx_bp_miss_coeff);
    qemu_log("Core%u bx_drain_store_buffer_coeff: %"PRIu32"\n", cpu->cpu_index, cpu->active_coeffs.bx_drain_store_buffer_coeff);
    qemu_log("Core%u bx_instruction_coeff: %"PRIu32"\n", cpu->cpu_index, cpu->active_coeffs.bx_instruction_coeff);
    qemu_log("======================================\n");

    /* process any pending work */
    cpu->exit_request = 1;
}

/* ---------------------------------------------------------------------------
 * End of a quantum, split in two levels:
 *
 *   quantum_host_barrier_wait()  once per host thread per quantum
 *   quantum_vcpu_advance()       once per vCPU per quantum
 */

/* Waits at the global barrier. Returns the new barrier generation. */
static uint32_t quantum_host_barrier_wait(CPUState *cpu, int *stop_request)
{
    *stop_request = 0;
    qstats_barrier_enter();
    return dynamic_barrier_polling_wait(
        &quantum_barrier,
        cpu->quantum_generation,
        stop_request,
        cpu->touched_timer_during_last_quantum != 0
    );
}

/* Moves @cpu to the next quantum after the barrier returned @new_generation. */
static void quantum_vcpu_advance(CPUState *cpu, uint32_t new_generation,
                                 int stop_request)
{
    uint32_t old_generation_low_32bit = cpu->quantum_generation & 0xFFFFFFFF;

    qstats_vcpu_end_quantum(cpu, cpu->quantum_generation);

    /*
     * dynamic_barrier_polling_wait() only returns stop requests 0 and 1 and
     * always ends the quantum, even with a stop request.
     */
    assert(stop_request != 2);
    assert(new_generation == old_generation_low_32bit + 1);

    if (cpu->quantum_done_idle && cpu->quantum_budget_in_picosecond > 0) {
        // this means the last quantum is not completely depleted. We need to deplete it before moving forward.
        cpu->quantum_budget_in_picosecond = 0;
    }
    cpu->quantum_generation += 1;
    /* Budget is always one quantum of simulated time in ps. */
    cpu->quantum_budget_in_picosecond += (int64_t)quantum_size * 1000;
    cpu->touched_timer_during_last_quantum = 0;
    cpu->vts = cpu->quantum_generation * quantum_size;

    /* The first slice of the new quantum (the whole quantum if slice == size). */
    cpu->quantum_slice_floor_ps = quantum_first_slice_floor(cpu->quantum_budget_in_picosecond);
    cpu->quantum_done = false;
    qatomic_set(&cpu->quantum_done_idle, false);
    cpu->waiting_for_quantum = 0;
}

/*
 * @cpu has finished its quantum: it used its budget, it is idle (@idle) or
 * it is stopped. It waits for its host thread to cross the barrier.
 */
static void quantum_mark_done(CPUState *cpu, bool idle)
{
    cpu->quantum_done = true;
    /* Read by other threads (qstats_note_interrupt()). */
    qatomic_set(&cpu->quantum_done_idle, idle);
    cpu->quantum_budget_depleted = 0;
    if (idle) {
        cpu->sgi_sender_time_ns_valid = false;
    }
    /* Read by plugins: the vCPU cannot take an interrupt before the next quantum. */
    cpu->waiting_for_quantum = 1;
}

/* ---------------------------------------------------------------------------
 * Polling of a vCPU (the non-blocking counterpart of qemu_wait_io_event())
 */

/*
 * @cpu is idle (WFI): it gives up the rest of its quantum. The unused budget
 * is the simulated time spent in WFI; first_cpu is the timekeeper, so it
 * banks it into the virtual clock (time keeps advancing while first_cpu is
 * halted) and runs any timer now due. Called with the BQL held, so
 * quantum_run_due_timers() does not relock it.
 */
static void quantum_account_idle(CPUState *cpu)
{
    int64_t left_ps = cpu->quantum_budget_in_picosecond;

    if (left_ps > 0) {
        // record the idle time.
        record_statistics_to_plugin(cpu->cpu_index, 4, left_ps / 1000);
        /* qstats: the unused budget is the simulated time spent in WFI. */
        cpu->qstats_wfi_ps += left_ps;
        if (cpu == first_cpu) {
            advance_quantum_time_ps(left_ps);
            quantum_run_due_timers();
        }
    }
    cpu->qstats_idle_entries++;
    quantum_mark_done(cpu, true);
}

/*
 * Called with the BQL held and current_cpu == @cpu. An idle vCPU is accounted
 * as WFI and done for the quantum, a stop request makes it stopped (and done),
 * and its queued work is processed. Never blocks.
 */
static void quantum_poll_vcpu(CPUState *cpu)
{
    qatomic_set_mb(&cpu->exit_request, 0);

    if (!cpu->quantum_done && !cpu->stop && !cpu->stopped &&
        cpu_thread_is_idle(cpu)) {
        qemu_plugin_vcpu_idle_cb(cpu);
        quantum_account_idle(cpu);
        qemu_plugin_vcpu_resume_cb(cpu);
    }

    qemu_wait_io_event_common(cpu);

    if (cpu->stopped && !cpu->quantum_done) {
        quantum_mark_done(cpu, false);
    }
}

/* Called with the BQL held. */
static void quantum_poll_members(QuantumHostThread *t)
{
    for (int i = 0; i < t->n_attached; i++) {
        current_cpu = t->vcpus[i];
        quantum_poll_vcpu(t->vcpus[i]);
    }
}

/* True if every member is stopped (the VM is paused or not started yet). */
static bool quantum_all_stopped(QuantumHostThread *t, bool check_idle)
{
    for (int i = 0; i < t->n_attached; i++) {
        CPUState *cpu = t->vcpus[i];
        if (!cpu->stopped || (check_idle && !cpu_thread_is_idle(cpu))) {
            return false;
        }
    }
    return true;
}

/* True if the prologue of the next quantum needs the BQL. Read without it. */
static bool quantum_prologue_needed(QuantumHostThread *t)
{
    if (t->n_stats_attached != t->n_attached) {
        return true;
    }
    for (int i = 0; i < t->n_attached; i++) {
        CPUState *cpu = t->vcpus[i];
        if (qatomic_read(&cpu->exit_request) || cpu->stop || cpu->stopped ||
            cpu->halted || !QSIMPLEQ_EMPTY_ATOMIC(&cpu->work_list)) {
            return true;
        }
    }
    return false;
}

/*
 * Start of a quantum, BQL held. Polls every member; a member idle now gives
 * up the whole quantum (idle snapshot), even if a sibling wakes it before its
 * turn. While every member is stopped, blocks on the shared halt_cond; after
 * the resume every member starts with an empty quantum, as with one vCPU per
 * thread, so that all threads meet again at the barrier.
 */
static void quantum_prologue(QuantumHostThread *t)
{
    if (t->n_stats_attached != t->n_attached) {
        for (int i = 0; i < t->n_attached; i++) {
            qstats_vcpu_attach(t->vcpus[i]);
        }
        t->n_stats_attached = t->n_attached;
    }

    quantum_poll_members(t);

    if (!quantum_all_stopped(t, true)) {
        return;
    }

    for (int i = 0; i < t->n_attached; i++) {
        qemu_plugin_vcpu_idle_cb(t->vcpus[i]);
    }
    do {
        // The VM has been stopped (e.g., via pause_all_vcpus). Wait until we're resumed.
        qemu_cond_wait_iothread(t->halt_cond);
    } while (quantum_all_stopped(t, true));

    for (int i = 0; i < t->n_attached; i++) {
        CPUState *cpu = t->vcpus[i];
        qemu_plugin_vcpu_resume_cb(cpu);
        /*
         * After every wake-up, resumed or not (as the former one-vCPU loop
         * did): the VM can be resumed before this thread reaches the barrier,
         * and the first quantum after a resume must be empty.
         */
        cpu->qstats_paused = true; // qstats: do not account the paused time to a quantum.
        cpu->quantum_budget_in_picosecond = 0; // force entering the barrier.
        cpu->quantum_slice_floor_ps = 0;
    }

    if (quantum_all_stopped(t, false)) {
        /*
         * Woken by queued work (e.g. the force-RCU kick, even during machine
         * init, before every member is attached) while the VM is still
         * stopped: process it; the next prologue blocks again.
         */
        quantum_poll_members(t);
        return;
    }

    /* Resumed: all members were realized before the VM started. */
    assert(t->n_attached == t->n_expected);
    for (int i = 0; i < t->n_attached; i++) {
        CPUState *cpu = t->vcpus[i];
        cpu->quantum_done = false;
        qatomic_set(&cpu->quantum_done_idle, false);
    }
    if (t->n_stats_attached != t->n_attached) {
        for (int i = 0; i < t->n_attached; i++) {
            qstats_vcpu_attach(t->vcpus[i]);
        }
        t->n_stats_attached = t->n_attached;
    }
    quantum_poll_members(t);
}

/* ---------------------------------------------------------------------------
 * Running a vCPU
 */

/* True if @cpu has work that needs the BQL before it runs. Read without it. */
static bool quantum_vcpu_needs_bql(CPUState *cpu)
{
    return qatomic_read(&cpu->exit_request) || cpu->stop ||
           !QSIMPLEQ_EMPTY_ATOMIC(&cpu->work_list);
}

/*
 * EXCP_ATOMIC: execute the instruction in an exclusive section. Without
 * enough budget left in this quantum the vCPU is done: cpu_loop_exit_atomic()
 * restored its state to the instruction, which raises EXCP_ATOMIC again when
 * the vCPU runs in the next quantum. Called without the BQL.
 */
static void quantum_step_atomic(CPUState *cpu)
{
    // Convert instruction count to picoseconds so the comparison is in consistent units.
    int64_t quantum_for_deduction_ps =
        (int64_t)cpu->last_tb_instruction_count_for_quantum * 100000
        / (cpu->ip100ns ? (int64_t)cpu->ip100ns : 1);

    if (cpu->quantum_budget_in_picosecond <= quantum_for_deduction_ps) {
        quantum_mark_done(cpu, false);
        return;
    }
    assert(cpu->quantum_budget_depleted == false);
    qstats_exec_begin();
    cpu_exec_step_atomic(cpu);
    qstats_exec_end(cpu);
}

/*
 * Runs @cpu until the current slice is over (its budget fell to the slice
 * floor), it is idle or it is stopped. Entered and left without the BQL.
 * *@ctx_open: a context-switch window is open; it is closed right before the
 * first execution of @cpu.
 */
static void quantum_run_vcpu(CPUState *cpu, bool *ctx_open)
{
    for (;;) {
        int r;

        if (quantum_vcpu_needs_bql(cpu)) {
            qemu_mutex_lock_iothread();
            quantum_poll_vcpu(cpu);
            qemu_mutex_unlock_iothread();
            if (cpu->quantum_done) {
                return;
            }
        }

        if (cpu->quantum_budget_in_picosecond <= cpu->quantum_slice_floor_ps) {
            cpu->quantum_budget_depleted = 0;
            if (cpu->quantum_budget_in_picosecond <= 0) {
                quantum_mark_done(cpu, false);
            }
            return;
        }

        if (*ctx_open) {
            qstats_ctx_end();
            *ctx_open = false;
        }
        qstats_exec_begin();
        r = tcg_cpus_exec(cpu);
        qstats_exec_end(cpu);
        if (r == EXCP_YIELD) {
            cpu->qstats_yield_exits++; // WFE/YIELD: the guest is spin-waiting
        }

        if (cpu->quantum_budget_depleted) {
            cpu->quantum_budget_depleted = 0;
            if (r == EXCP_QUANTUM && !qatomic_read(&cpu->exit_request)) {
                continue;   /* fast path: the slice is over, no BQL */
            }
        }

        if (r == EXCP_ATOMIC) {
            quantum_step_atomic(cpu);
            if (cpu->quantum_done) {
                return;
            }
            continue;
        }

        qemu_mutex_lock_iothread();
        if (r == EXCP_DEBUG) {
            cpu_handle_guest_debug(cpu);
        }
        /*
         * EXCP_HALTED: usually cpu->halted is set, but may have already been
         * reset by another thread by the time we arrive here. EXCP_HLT (WFI):
         * the vCPU is idle, accounted by quantum_poll_vcpu(). Ignore
         * everything else.
         */
        quantum_poll_vcpu(cpu);
        qemu_mutex_unlock_iothread();
        if (cpu->quantum_done) {
            return;
        }
    }
}

/*
 * One turn of @cpu. With several vCPUs on the host thread, the time from the
 * start of the turn to the start of the execution (selection, current_cpu,
 * slow-path polling) is accounted as a context switch (qstats ctx_switch_ns,
 * n_switches); BQL waits inside it are accounted as bql_wait_ns only.
 */
static void quantum_dispatch(QuantumHostThread *t, CPUState *cpu)
{
    bool ctx_open = t->n_attached > 1;

    if (ctx_open) {
        qstats_ctx_begin();
    }
    current_cpu = cpu;
    quantum_run_vcpu(cpu, &ctx_open);
    if (ctx_open) {
        qstats_ctx_end();
    }
}

/*
 * Runs the members in rounds, in ascending cpu_index order, until each of
 * them is done for the quantum. A dispatch returns at the end of the vCPU's
 * slice; after each round the slice floor of every vCPU not done yet drops by
 * one slice, down to 0, where the dispatch only returns once the vCPU is done.
 * So a quantum takes at most ceil(budget / slice) rounds, a single one when
 * the slice is the whole quantum.
 */
static void quantum_run_rounds(QuantumHostThread *t)
{
    const int64_t slice_ps = (int64_t)quantum_slice_size * 1000;

    for (;;) {
        bool all_done = true;

        for (int i = 0; i < t->n_attached; i++) {
            if (!t->vcpus[i]->quantum_done) {
                quantum_dispatch(t, t->vcpus[i]);
            }
        }
        for (int i = 0; i < t->n_attached; i++) {
            if (!t->vcpus[i]->quantum_done) {
                all_done = false;
            }
        }
        if (all_done) {
            return;
        }
        for (int i = 0; i < t->n_attached; i++) {
            CPUState *cpu = t->vcpus[i];
            if (!cpu->quantum_done) {
                cpu->quantum_slice_floor_ps =
                    MAX(0, cpu->quantum_slice_floor_ps - slice_ps);
            }
        }
    }
}

/* Waits at the barrier once and moves every member to the next quantum. */
static void quantum_end_quantum(QuantumHostThread *t)
{
    CPUState *lead = t->vcpus[0];
    uint64_t finished_generation = lead->quantum_generation;
    int stop_request = 0;
    uint32_t new_generation;

    current_cpu = lead;
    new_generation = quantum_host_barrier_wait(lead, &stop_request);
    qstats_barrier_exit(finished_generation, stop_request ? QSTATS_STOP : 0);

    for (int i = 0; i < t->n_attached; i++) {
        quantum_vcpu_advance(t->vcpus[i], new_generation, stop_request);
    }
    if (stop_request) {
        /* The VM is being paused: stop every member (cpu_stop_current() stops one). */
        for (int i = 0; i < t->n_attached; i++) {
            t->vcpus[i]->stop = true;
            cpu_exit(t->vcpus[i]);
        }
    }
}

static bool quantum_host_thread_alive(QuantumHostThread *t)
{
    for (int i = 0; i < t->n_attached; i++) {
        if (!t->vcpus[i]->unplug || cpu_can_run(t->vcpus[i])) {
            return true;
        }
    }
    return false;
}

/*
 * In the multi-threaded case each host thread runs one or more vCPUs. The TLS
 * variable current_cpu can be used deep in the code to find the current
 * CPUState for a given thread; it is set before running or polling a vCPU.
 */
static void *quantum_host_thread_fn(void *arg)
{
    QuantumHostThread *t = arg;
    CPUState *cpu = t->creator;

    assert(tcg_enabled());
    g_assert(!icount_enabled());

    rcu_register_thread();
    t->force_rcu.notify = quantum_force_rcu;
    rcu_add_force_rcu_notifier(&t->force_rcu);
    tcg_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(t->thread);

    cpu->thread_id = qemu_get_thread_id();

    /* One registration per host thread: the barrier threshold counts threads. */
    t->generation = dynamic_barrier_polling_increase_by_1(&quantum_barrier);
    quantum_vcpu_init(cpu, t->generation);

    current_cpu = cpu;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(t->host_core, &cpuset);
    int res = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    assert(res == 0 && "Failed to set thread affinity");

    qstats_thread_init(t->id, t->host_core);
    qemu_mutex_unlock_iothread();

    do {
        if (quantum_prologue_needed(t)) {
            qemu_mutex_lock_iothread();
            quantum_prologue(t);
            qemu_mutex_unlock_iothread();
        }

        quantum_run_rounds(t);

        /*
         * Every member stopped outside a barrier stop request (vm_stop() from
         * a vCPU thread): do not cross the barrier, the next prologue blocks.
         */
        if (quantum_all_stopped(t, false)) {
            continue;
        }

        /*
         * If the VM was stopped (startup, snapshot, pause), do not account the
         * paused time: restart the timing of this quantum right before the
         * barrier, also when the resume came after the prologue.
         */
        for (int i = 0; i < t->n_attached; i++) {
            qstats_resume_if_needed(t->vcpus[i]);
        }

        quantum_end_quantum(t);
    } while (quantum_host_thread_alive(t));

    qemu_mutex_lock_iothread();
    for (int i = 0; i < t->n_attached; i++) {
        tcg_cpus_destroy(t->vcpus[i]);
    }
    qemu_mutex_unlock_iothread();
    rcu_remove_force_rcu_notifier(&t->force_rcu);
    rcu_unregister_thread();

    // resign the current thread from the barrier.
    dynamic_barrier_polling_decrease_by_1(&quantum_barrier);

    return NULL;
}

void quantum_kick_vcpu_thread(CPUState *cpu)
{
    cpu_exit(cpu);
}

void quantum_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];
    QuantumHostThread *t;

    g_assert(tcg_enabled());
    tcg_cpu_init_cflags(cpu, current_machine->smp.max_cpus > 1);

    t = quantum_attach_vcpu(cpu);

    if (!t->thread) {
        /* First member of the group: create its host thread. */
        t->creator = cpu;
        t->thread = g_new0(QemuThread, 1);
        t->halt_cond = g_malloc0(sizeof(QemuCond));
        qemu_cond_init(t->halt_cond);
        cpu->thread = t->thread;
        cpu->halt_cond = t->halt_cond;

        if (t->n_expected == 1) {
            snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/TCG",
                     cpu->cpu_index);
        } else {
            snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "QHT %d/TCG", t->id);
        }
        qemu_thread_create(t->thread, thread_name, quantum_host_thread_fn,
                           t, QEMU_THREAD_JOINABLE);
    } else {
        /*
         * The host thread exists and has registered with the barrier (it
         * signalled the creator's creation): share it, as
         * quantum_rr_start_vcpu_thread() does.
         */
        cpu->thread = t->thread;
        cpu->halt_cond = t->halt_cond;
        cpu->thread_id = t->creator->thread_id;
        quantum_vcpu_init(cpu, t->generation);
        cpu->created = true;
    }
}

void quantum_initialize_barrier(void) {
    dynamic_barrier_polling_init(&quantum_barrier, 0);
    qstats_global_init();
}
