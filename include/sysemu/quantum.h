/**
 * @file include/sysemu/quantum.h
 * 
 * Quantum API, for configuring and using the quantum counter.
 */

#ifndef SYSEMU_QUANTUM_H
#define SYSEMU_QUANTUM_H


void quantum_configure(QemuOpts *opts, Error **errp);

#ifdef CONFIG_TCG
extern uint64_t quantum_size;
extern uint64_t quantum_check_threshold;
extern bool quantum_allow_interrupt_wakeup_inside;
extern bool quantum_rr_mode;
extern char *quantum_stats_dir;
#define quantum_enabled() (quantum_size != 0)
#define quantum_rr_enabled() (quantum_rr_mode)
#define quantum_stats_enabled() (quantum_stats_dir != NULL)
#else
#define quantum_enabled() (0)
#define quantum_rr_enabled() (0)
#define quantum_stats_enabled() (0)
#endif



#endif