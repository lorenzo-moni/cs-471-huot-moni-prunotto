/*
 * QEMU TCG Multi Threaded vCPUs implementation with Quantum
 *
 * Copyright 2025 PARSA
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef TCG_ACCEL_OPS_QUANTUM_H
#define TCG_ACCEL_OPS_QUANTUM_H

/* kick MTTCG vCPU thread */
void quantum_kick_vcpu_thread(CPUState *cpu);

/* start an mttcg vCPU thread */
void quantum_start_vcpu_thread(CPUState *cpu);

/* initialize the barrier for quantum */
void quantum_initialize_barrier(void);

/* initialize the IPC table for each thread */
void quantum_initialize_core_info_table(const char *file_name);

/* Per-member initialization under BQL, before creation acknowledgement. */
bool quantum_vcpu_validate_model(CPUState *cpu, Error **errp);
void quantum_vcpu_init_model(CPUState *cpu);
void quantum_vcpu_init_time_state(CPUState *cpu);
void quantum_vcpu_init_budget(CPUState *cpu, uint32_t generation);

/*
 * Wait once per host thread with the BQL released and current_cpu selected.
 * Starts qstats barrier accounting; the caller must finish or cancel it and
 * manage the vCPU waiting flags and any stop request.
 */
uint32_t quantum_host_barrier_wait(CPUState *cpu, int *stop_request);

/*
 * Emit the completed vCPU quantum's qstats record and advance its time state.
 * Called by the owning thread after the host barrier accounting is finished.
 * Does not wait at the barrier or stop the CPU.
 */
void quantum_vcpu_advance(CPUState *cpu, uint32_t new_generation,
                         int stop_request, bool from_idle);

#endif /* TCG_ACCEL_OPS_QUANTUM_H */
