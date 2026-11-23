/**
 * @file include/sysemu/quantum.h
 * 
 * Quantum API, for configuring and using the quantum counter.
 */

#ifndef SYSEMU_QUANTUM_H
#define SYSEMU_QUANTUM_H


void quantum_configure(QemuOpts *opts, Error **errp);

#ifdef CONFIG_TCG
extern uint64_t quantum_workers;
/* Stable plugin API storage. Publication occurs with every guest at the barrier. */
extern uint32_t quantum_worker_generation;
extern uint64_t *quantum_worker_plugin_generation;
/* BQL protects checkpoint request/ack and the execution-started guard. */
extern bool quantum_worker_checkpoint_pending;
extern bool quantum_worker_execution_started;
#define quantum_workers_enabled() (quantum_workers != 0)
extern uint64_t quantum_size;
extern uint64_t quantum_check_threshold;
extern bool quantum_allow_interrupt_wakeup_inside;
extern bool quantum_rr_mode;
extern char *quantum_stats_dir;
#define quantum_enabled() (quantum_size != 0)
#define quantum_rr_enabled() (quantum_rr_mode)
#define quantum_stats_enabled() (quantum_stats_dir != NULL)
#else
#define quantum_workers_enabled() (0)
#define quantum_enabled() (0)
#define quantum_rr_enabled() (0)
#define quantum_stats_enabled() (0)
#endif



#endif