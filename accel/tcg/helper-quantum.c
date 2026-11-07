// This file is added by Cyan for the quantum mechanism.

#include "qemu/osdep.h"
#include "qemu/typedefs.h"
#include "qemu/units.h"
#include "gdbstub/helpers.h"
#include "exec/helper-proto.h"
#include "cpu.h"
#include "hw/core/cpu.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/quantum.h"
#include "sysemu/asid-coeff.h"
#include "qemu/plugin-pf.h"
/*
 * quantum_flush_current_stats - deduct accumulated stats into the quantum budget.
 *
 * Computes required_picoseconds from the current plugin-exposed statistics
 * using the core's active_coeffs, zeroes the statistics, updates
 * target_cycle_on_instruction and cpu_virtual_time[].vts, and subtracts
 * from quantum_budget_in_picosecond.
 *
 * Does NOT set quantum_budget_depleted — the caller is responsible for
 * checking the budget afterward.
 *
 * Safe to call from translated-code context (e.g. TTBR write handlers) where
 * current_cpu is valid.  Uses the passed @cpu parameter throughout.
 */
#ifdef CONFIG_AVX2_OPT
__attribute__((target("avx2")))
#endif
void quantum_flush_current_stats(CPUState *cpu)
{
    if (!quantum_enabled() || cpu->ip100ns == 0) {
        return;
    }

    uint64_t required_picoseconds = 0;

    if (g_statistics_managed_by_plugin && cpu->is_ipc_model) {
        /*
         * Plugin path: use the bx_* IPC model.
         * Constant-IPNS cores fall through to the instruction-count path below.
         */
        struct qemu_plugin_exposed_statistics *this_core_info =
            &g_exposed_statistics[cpu->cpu_index];

        const uint32_t *stats  = this_core_info->arr;
        const uint32_t *coeffs = cpu->active_coeffs.arr;
        for (int i = 0; i < 6; i++) {
            required_picoseconds += (uint64_t)stats[i] * coeffs[i];
        }

        /* Reset statistics so they are not counted again. */
        memset(this_core_info, 0, sizeof(*this_core_info));
    } else {
        /*
         * Non-plugin path: convert instruction count to picoseconds using ip100ns.
         *
         * ip100ns = instrs / 100ns, so:
         *   time_ns  = instr_count / (ip100ns / 100)
         *   time_ps  = time_ns * 1000
         *            = instr_count * 100 * 1000 / ip100ns
         *            = instr_count * 100000 / ip100ns
         *
         * ip100ns is guaranteed non-zero (checked at top of function).
         */
        required_picoseconds = cpu->last_tb_instruction_count_for_quantum
                               * 100000 / cpu->ip100ns;
    }

    /* Per-quantum instruction count for the qstats breakdown. */
    cpu->qstats_insns += cpu->last_tb_instruction_count_for_quantum;
    cpu->last_tb_instruction_count_for_quantum = 0;

    /* Convert picoseconds to nanoseconds for target_cycle tracking. */
    cpu->target_cycle_on_instruction += required_picoseconds / 1000;

    /* Deduct from quantum budget (in picoseconds). */
    cpu->quantum_budget_in_picosecond -= required_picoseconds;

    /* Advance virtual time (vts is in nanoseconds). */
    cpu->vts += required_picoseconds / 1000;

    /*
     * first_cpu is the global timekeeper: fold its exact picosecond
     * consumption into the virtual clock.  No /1000 here, so the global clock
     * stays free of the per-flush rounding that vts still carries.
     *
     * It then runs any virtual timer that just came due.  Doing this here
     * rather than in the check_and_deduce_quantum helper also covers the
     * TTBR-write flush path, so timers fire as soon as the clock crosses them.
     */
    if (cpu == first_cpu) {
        advance_quantum_time_ps(required_picoseconds);
        quantum_run_due_timers();
    }
}

uint32_t HELPER(check_and_deduce_quantum)(CPUArchState *env) {
    assert(quantum_enabled());
    assert(current_cpu->env_ptr == env);

    if (current_cpu->ip100ns == 0) {
        // this is not a thread managed by quantum.
        return false;
    }

    /*
     * Flush accumulated statistics and deduct from the quantum budget.  For
     * first_cpu this also advances the virtual clock and runs any due timer.
     */
    quantum_flush_current_stats(current_cpu);

    if (current_cpu->quantum_budget_in_picosecond <= 0) {
        current_cpu->quantum_budget_depleted = 1;
        current_cpu->vts = (current_cpu->quantum_generation + 1) * quantum_size;
        return true;
    }
    return false;
}

void HELPER(set_instruction_count_for_quantum)(CPUArchState *env, uint32_t requirement) {
    assert(quantum_enabled());
    current_cpu->last_tb_instruction_count_for_quantum = requirement;
}

void HELPER(increase_target_cycle)(CPUArchState *env) {
    assert(icount_enabled());

    current_cpu->vts += current_cpu->last_tb_instruction_count_for_quantum * 10000 / current_cpu->ip100ns;

    current_cpu->last_tb_instruction_count_for_quantum = 0;
}
