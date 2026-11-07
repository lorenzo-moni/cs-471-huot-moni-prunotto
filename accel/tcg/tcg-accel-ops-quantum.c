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

// Core info table for quantum mode
static core_meta_info_t core_info_table[256];

void quantum_initialize_core_info_table(const char *file_name) {
    tcg_parse_core_info_file(file_name, core_info_table, 256);
}


typedef struct MttcgForceRcuNotifier {
    Notifier notifier;
    CPUState *cpu;
} MttcgForceRcuNotifier;

static void do_nothing(CPUState *cpu, run_on_cpu_data d)
{
}

static void mttcg_force_rcu(Notifier *notify, void *data)
{
    CPUState *cpu = container_of(notify, MttcgForceRcuNotifier, notifier)->cpu;

    /*
     * Called with rcu_registry_lock held, using async_run_on_cpu() ensures
     * that there are no deadlocks.
     */
    async_run_on_cpu(cpu, do_nothing, RUN_ON_CPU_NULL);
}

/*
 * Synchronization at the end of a quantum, split in two levels so that a
 * host thread running several vCPUs (time multiplexing) can wait at the
 * barrier once and then advance each of its vCPUs:
 *
 *   quantum_host_barrier_wait()  once per host thread per quantum
 *   quantum_vcpu_advance()       once per vCPU per quantum
 *
 * With one vCPU per host thread, quantum_sync_to_next() calls both.
 */

typedef enum {
    QSYNC_NEXT,          /* the vCPU entered the next quantum */
    QSYNC_STOPPED,       /* stop request: cpu_stop_current() has been called */
    QSYNC_WOKEN_INSIDE,  /* woken inside the quantum (allow_interrupt_wakeup_inside) */
} QSyncResult;

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
                                 int stop_request, bool from_idle)
{
    uint32_t old_generation_low_32bit = cpu->quantum_generation & 0xFFFFFFFF;

    qstats_vcpu_end_quantum(cpu, cpu->quantum_generation);

    if (stop_request != 2) {
        assert(new_generation == old_generation_low_32bit + 1);
        if (from_idle && cpu->quantum_budget_in_picosecond > 0) {
            // this means the last quantum is not completely depleted. We need to deplete it before moving forward.
            cpu->quantum_budget_in_picosecond = 0;
        }
        cpu->quantum_generation += 1;
        /* Budget is always one quantum of simulated time in ps. */
        cpu->quantum_budget_in_picosecond += (int64_t)quantum_size * 1000;
        cpu->touched_timer_during_last_quantum = 0;
        cpu->vts = cpu->quantum_generation * quantum_size;
    } else {
        // this means the vCPU quits due to the machine state change.
        assert(new_generation == old_generation_low_32bit);
        // clean the budget.
        cpu->quantum_budget_in_picosecond = 0;
        cpu->touched_timer_during_last_quantum = 1; // force updating the timer.
        cpu->quantum_budget_depleted = 1;
        cpu->quantum_generation += 1;
        cpu->vts = cpu->quantum_generation * quantum_size;
        assert(cpu->stop || cpu->stopped || !runstate_is_running());
    }
}

/*
 * Ends the current quantum of @cpu: waits at the barrier and advances it.
 * @from_idle: the vCPU is idle (WFI); its unused budget is dropped.
 */
static QSyncResult quantum_sync_to_next(CPUState *cpu, bool from_idle)
{
    uint32_t old_generation_low_32bit = cpu->quantum_generation & 0xFFFFFFFF;
    uint64_t finished_generation = cpu->quantum_generation;
    int stop_request = 0;

    if (from_idle) {
        cpu->sgi_sender_time_ns_valid = false;
    }
    cpu->waiting_for_quantum = 1;
    uint32_t new_generation = quantum_host_barrier_wait(cpu, &stop_request);
    cpu->waiting_for_quantum = 0;

    if (from_idle && stop_request != 2 &&
        new_generation == old_generation_low_32bit) {
        // The CPU thread is waken up in the middle of the quantum
        assert(quantum_allow_interrupt_wakeup_inside);
        qstats_barrier_cancel();
        return QSYNC_WOKEN_INSIDE;
    }

    qstats_barrier_exit(finished_generation, stop_request ? QSTATS_STOP : 0);
    quantum_vcpu_advance(cpu, new_generation, stop_request, from_idle);

    if (stop_request) {
        cpu_stop_current();
        return QSYNC_STOPPED;
    }
    return QSYNC_NEXT;
}

/*
 * In the multi-threaded case each vCPU has its own thread. The TLS
 * variable current_cpu can be used deep in the code to find the
 * current CPUState for a given thread.
 */

static void *mttcg_cpu_thread_fn(void *arg)
{
    MttcgForceRcuNotifier force_rcu;
    CPUState *cpu = arg;

    // Copy core info from table to CPUState
    cpu->ip100ns = (uint64_t)(core_info_table[cpu->cpu_index].ipns * 100);
    cpu->active_coeffs = core_info_table[cpu->cpu_index].coeffs;

    /* Record whether this is an ipc-model core (enables ASID switching). */
    cpu->is_ipc_model = !core_model_is_constant(&core_info_table[cpu->cpu_index]);

    /* Save defaults so the TTBR handler can fall back to them. */
    cpu->default_coeffs = cpu->active_coeffs;

    assert(tcg_enabled());
    g_assert(!icount_enabled());

    rcu_register_thread();
    force_rcu.notifier.notify = mttcg_force_rcu;
    force_rcu.cpu = cpu;
    rcu_add_force_rcu_notifier(&force_rcu.notifier);
    tcg_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
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

    cpu->quantum_generation = dynamic_barrier_polling_increase_by_1(&quantum_barrier);
    /* Budget is always in picoseconds: quantum_size (ns) * 1000 = ps. */
    cpu->quantum_budget_in_picosecond = (int64_t)quantum_size * 1000;
    assert(cpu->quantum_budget_in_picosecond > 0);
    cpu->last_tb_instruction_count_for_quantum = 0;
    cpu->quantum_budget_depleted = 0;

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



    current_cpu = cpu;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    assert(cpu->cpu_index < 128);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_info_table[cpu->cpu_index].host_core_idx, &cpuset);
    int res = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    assert(res == 0 && "Failed to set thread affinity");

    /* One host thread per vCPU: the host thread index is the cpu_index. */
    qstats_thread_init(cpu->cpu_index, core_info_table[cpu->cpu_index].host_core_idx);
    qstats_vcpu_attach(cpu);

    /* process any pending work */
    cpu->exit_request = 1;

    do {
        if (cpu_can_run(cpu)) {
            int r;
            qemu_mutex_unlock_iothread();
continue_to_run:
            qstats_exec_begin();
            r = tcg_cpus_exec(cpu);
            qstats_exec_end(cpu);
            // check the quantum budget and sync before doing I/O operation.
            if (cpu->quantum_budget_depleted) {
                cpu->quantum_budget_depleted = false;
                while (cpu->quantum_budget_in_picosecond <= 0) {
                    if (quantum_sync_to_next(cpu, false) != QSYNC_NEXT) {
                        break;
                    }
                }

                if (r == EXCP_QUANTUM && !cpu->exit_request) {
                    goto continue_to_run;
                }

                if (r == EXCP_HLT) {
                    // this is WFI.
                    // In this case, we should directly wait for the quantum barrier, without getting the iothread.
                }
            }
            qemu_mutex_lock_iothread();
            switch (r) {
            case EXCP_DEBUG:
                cpu_handle_guest_debug(cpu);
                break;
            case EXCP_HALTED:
                /*
                 * Usually cpu->halted is set, but may have already been
                 * reset by another thread by the time we arrive here.
                 */
                break;
            case EXCP_ATOMIC:
                qemu_mutex_unlock_iothread();
                // Well, it is possible that this atomic step may deplete the quantum budget.
                // What we have to do now is to give enough quantum budget to this CPU, and remove it afterwards.
                // Convert instruction count to picoseconds so the comparison is in consistent units.
                int64_t quantum_for_deduction_ps =
                    (int64_t)cpu->last_tb_instruction_count_for_quantum * 100000
                    / (cpu->ip100ns ? (int64_t)cpu->ip100ns : 1);
                // We need to sync immediately to get the quantum budget.
                while (cpu->quantum_budget_in_picosecond <= quantum_for_deduction_ps) {
                    if (quantum_sync_to_next(cpu, false) != QSYNC_NEXT) {
                        break;
                    }
                }
                assert(cpu->quantum_budget_depleted == false);
                qstats_exec_begin();
                cpu_exec_step_atomic(cpu);
                qstats_exec_end(cpu);
                qemu_mutex_lock_iothread();
            default:
                /* Ignore everything else? */
                break;
            }
        }

        qatomic_set_mb(&cpu->exit_request, 0);
        qemu_wait_io_event(cpu); // This function will not decouple the thread from the barrier anymore.
        qemu_mutex_unlock_iothread();
        // If the VM was stopped (startup, snapshot, pause), do not account the paused time.
        qstats_resume_if_needed(cpu);

        // it is possible that the quantum budget is depleted due to the idle state.
        if (cpu->quantum_budget_depleted && !cpu->stopped) {
            cpu->quantum_budget_depleted = false;
            do {
                if (quantum_sync_to_next(cpu, true) != QSYNC_NEXT) {
                    break;
                }
            } while (cpu->quantum_budget_in_picosecond <= 0);
        }

        qemu_mutex_lock_iothread();
    } while (!cpu->unplug || cpu_can_run(cpu));

    tcg_cpus_destroy(cpu);
    qemu_mutex_unlock_iothread();
    rcu_remove_force_rcu_notifier(&force_rcu.notifier);
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

    g_assert(tcg_enabled());
    tcg_cpu_init_cflags(cpu, current_machine->smp.max_cpus > 1);

    cpu->thread = g_new0(QemuThread, 1);
    cpu->halt_cond = g_malloc0(sizeof(QemuCond));
    qemu_cond_init(cpu->halt_cond);

    /* create a thread per vCPU with TCG (MTTCG) */
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/TCG",
             cpu->cpu_index);

    qemu_thread_create(cpu->thread, thread_name, mttcg_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}

void quantum_initialize_barrier(void) {
    dynamic_barrier_polling_init(&quantum_barrier, 0);
    qstats_global_init();
}
