#ifndef QEMU_CPUS_H
#define QEMU_CPUS_H

extern bool qemu_worker_standalone;
void cpus_worker_startup_complete(void);

#include "sysemu/accel-ops.h"

/* register accel-specific operations */
void cpus_register_accel(const AccelOpsClass *i);

/* return registers ops */
const AccelOpsClass *cpus_get_accel(void);

/* accel/dummy-cpus.c */

/* Create a dummy vcpu for AccelOpsClass->create_vcpu_thread */
void dummy_start_vcpu_thread(CPUState *);

/* interface available for cpus accelerator threads */

/* For temporary buffers for forming a name */
#define VCPU_THREAD_NAME_SIZE 16

void cpus_kick_thread(CPUState *cpu);
bool cpu_work_list_empty(CPUState *cpu);
bool cpu_thread_is_idle(CPUState *cpu);
bool all_cpu_threads_idle(void);
bool cpu_can_run(CPUState *cpu);
void qemu_wait_io_event_common(CPUState *cpu);
/*
 * Account one quantum idle event after the caller has selected the idle path
 * for a quantum-managed CPU (quantum enabled, ip100ns != 0, not stopped or
 * stopping). Call on the owning CPU thread with current_cpu == cpu and the
 * BQL held; no additional lock is required. The BQL remains held on return.
 *
 * Records plugin/qstats idle time, advances first_cpu's clock and runs due
 * timers for a positive budget, then sets quantum_budget_depleted. Does not
 * directly change the budget, generation or halted state, wait at a barrier,
 * or emit idle/resume callbacks. Timer callbacks may wake CPUs or queue work.
 * Not idempotent: the caller must not account the same idle remainder twice.
 */
void qemu_account_quantum_idle(CPUState *cpu);

void qemu_wait_io_event(CPUState *cpu); 
void cpu_thread_signal_created(CPUState *cpu);
void cpu_thread_signal_destroyed(CPUState *cpu);
void cpu_handle_guest_debug(CPUState *cpu);

/* end interface for cpus accelerator threads */

bool qemu_in_vcpu_thread(void);
void qemu_init_cpu_loop(void);
void resume_all_vcpus(void);
void pause_all_vcpus(void);
void cpu_stop_current(void);

extern int icount_align_option;

/* Unblock cpu */
void qemu_cpu_kick_self(void);

bool cpus_are_resettable(void);

void cpu_synchronize_all_states(void);
void cpu_synchronize_all_post_reset(void);
void cpu_synchronize_all_post_init(void);
void cpu_synchronize_all_pre_loadvm(void);

#ifndef CONFIG_USER_ONLY
/* vl.c */
/* *-user doesn't have configurable SMP topology */
extern int smp_cores;
extern int smp_threads;
#endif

#endif
