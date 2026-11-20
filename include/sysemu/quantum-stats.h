/*
 * Per-quantum host-time breakdown of the quantum-synchronized warming (qstats).
 *
 * Enabled with `-quantum size=...,stats=<dir>` (not supported with rr=on).
 * For every quantum, each host thread records how its host time splits into
 *   - exec:        everything that is not one of the components below
 *                  (executing target instructions, handling TCG exits, queued
 *                  work, virtual timers on vCPU 0, time holding the BQL),
 *   - barrier:     waiting at the global quantum barrier,
 *   - ctx_switch:  switching between co-located target cores (N:M mode),
 *   - bql_wait:    waiting for the BQL (QEMU's global mutex), anywhere in the
 *                  vCPU thread except inside the barrier,
 * plus the thread CPU time of the exec and barrier phases, the serial work of
 * the thread that releases the barrier, and the OS context switches of the
 * quantum. For each target core it runs, it records the instructions executed,
 * the simulated time consumed by them and given up in WFI, the host time spent
 * executing it, and its BQL and WFE/YIELD activity.
 *
 * Output (in <dir>):
 *   host_<tid>.csv  one row per quantum per host thread  (QStatsHostRec)
 *   vcpu_<tid>.csv  one row per quantum per vCPU run by host thread <tid>
 *                   (QStatsVcpuRec)
 *   threads.csv     one row per host thread: core, Linux TID, flush time,
 *                   scheduler statistics at exit
 *   meta.txt        quantum size and wall-clock time of the process
 *
 * Generations with the STOP, RESUMED or FLUSHED flag in any host thread
 * should be excluded from the analysis: their times include a VM pause or the
 * delay of a thread writing its buffers, which also inflates the barrier time
 * of the other threads in that generation.
 *
 * Records are kept in per-thread buffers and written when a buffer fills up
 * and at process exit (exit notifier). At exit the vCPU threads may still be
 * running (WormCache exits from the main loop after resuming the VM), so the
 * buffers are written under a lock and nothing is written after the files are
 * closed. All hooks are no-ops when stats are disabled (qstats_self == NULL).
 *
 * The bookkeeping is per *host thread*: today a host thread runs exactly one
 * vCPU (r = 1), with time multiplexing it runs several.
 */

#ifndef SYSEMU_QUANTUM_STATS_H
#define SYSEMU_QUANTUM_STATS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

struct CPUState;

/* Bits of QStatsHostRec.flags. */
#define QSTATS_LAST_ARRIVER                                                    \
  (1u << 0) /* this thread released the barrier (ran the periodic check) */
#define QSTATS_STOP                                                            \
  (1u << 1) /* the barrier returned a stop request (snapshot / pause) */
#define QSTATS_WAKE_INSIDE                                                     \
  (1u << 2) /* a vCPU was woken up inside the quantum; never set by            \
               the current barrier, which always ends the quantum */
#define QSTATS_FLUSHED                                                         \
  (1u << 3) /* the thread wrote its buffers just before this quantum */
#define QSTATS_RESUMED                                                         \
  (1u << 4) /* first quantum after the VM was (re)started */

/*
 * One row of host_<tid>.csv. Host times are CLOCK_MONOTONIC_RAW nanoseconds,
 * *_cpu_ns are CLOCK_THREAD_CPUTIME_ID nanoseconds.
 * exec + barrier + ctx_switch + bql_wait == t_start_ns of the next row,
 * unless the next row is FLUSHED or RESUMED.
 */
typedef struct QStatsHostRec {
  uint64_t generation; /* quantum that has just ended */
  uint64_t
      t_start_ns; /* start of the quantum, relative to qstats_global_init() */
  uint64_t
      exec_ns; /* t_start -> barrier entry, minus ctx_switch and bql_wait */
  uint64_t
      barrier_ns; /* barrier entry -> barrier release seen by this thread */
  uint64_t ctx_switch_ns; /* switches between co-located vCPUs (0 with r = 1) */
  uint64_t bql_wait_ns;   /* BQL waits outside the barrier */
  uint64_t exec_cpu_ns;   /* thread CPU time from t_start to the barrier */
  uint64_t barrier_cpu_ns; /* thread CPU time inside the barrier */
  uint32_t n_switches;     /* vCPU switches (N:M), not OS context switches */
  uint32_t flags;          /* QSTATS_* */
  uint64_t release_ns; /* last arriver only: serial work before releasing the
                          barrier */
  uint64_t barrier_bql_ns; /* BQL wait inside the barrier (last arriver), part
                              of barrier_ns */
  uint32_t vol_csw;   /* OS voluntary context switches since the previous row */
  uint32_t invol_csw; /* OS involuntary context switches (preemptions) since the
                         previous row */
} QStatsHostRec;

/* One row of vcpu_<tid>.csv. */
typedef struct QStatsVcpuRec {
  uint64_t generation;
  uint64_t insns; /* guest instructions (whole TBs) */
  uint64_t
      host_exec_ns;     /* host time in TCG for this vCPU, BQL waits excluded */
  uint64_t bql_wait_ns; /* BQL waits of its thread outside the barrier */
  int64_t wfi_ps; /* simulated time given up idle (budget left when halting) */
  uint64_t busy_ps; /* simulated time deducted from the budget by execution */
  uint64_t bql_wait_exec_ns; /* part of bql_wait_ns spent inside TCG */
  uint32_t cpu_idx;
  uint32_t idle_entries;
  uint32_t bql_acq;     /* BQL acquisitions outside the barrier */
  uint32_t yield_exits; /* TCG exits with EXCP_YIELD (WFE/YIELD) */
} QStatsVcpuRec;

typedef struct QStatsHost {
  int tid;        /* host thread index (== cpu_index for r = 1) */
  int host_core;  /* host core the thread is pinned to, -1 if unknown */
  int kernel_tid; /* Linux TID, for /proc/self/task/<tid>/... */

  /* State of the quantum in progress (owner thread only). */
  uint64_t t_start;
  uint64_t t_bar_enter;
  uint64_t t_ctx_start;
  uint64_t t_exec_start;
  uint64_t t_release_start;
  uint64_t cpu_start;
  uint64_t cpu_bar_enter;
  uint64_t ctx_acc;
  uint64_t bql_acc;
  uint64_t barrier_bql_acc;
  uint64_t release_ns;
  long nvcsw_prev;
  long nivcsw_prev;
  uint32_t n_switches;
  uint32_t pending_flags;
  bool in_barrier;
  bool in_exec;
  bool need_restart;

  /*
   * Record buffers, filled by the owner thread only. hlen/vlen are
   * published with release stores so that the exit notifier, which may run
   * concurrently in another thread, only reads complete records.
   */
  QStatsHostRec *hbuf;
  size_t hlen;
  QStatsVcpuRec *vbuf;
  size_t vlen;
  FILE *hfp;
  FILE *vfp;

  /* Metadata for threads.csv (updated under the module lock). */
  uint64_t flush_ns;
  uint64_t n_flushes;
} QStatsHost;

/* The calling thread's state; NULL when stats are disabled or not initialized.
 */
extern __thread QStatsHost *qstats_self;

/*
 * Called by qemu_mutex_lock_iothread_impl() (softmmu/cpus.c, where it is
 * defined) with the time a vCPU thread waited for the BQL, for vCPUs with
 * qstats_bql_on set. Installed by qstats_global_init(); NULL otherwise.
 */
extern void (*qstats_bql_wait_cb)(struct CPUState *cpu, uint64_t wait_ns);

static inline uint64_t qstats_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* CPU time consumed by the calling thread (a syscall: once per phase per
 * quantum). */
static inline uint64_t qstats_thread_cpu_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Main thread, once, before the vCPU threads start. No-op if stats are
 * disabled. */
void qstats_global_init(void);

/* Per host thread, after pinning. No-op if stats are disabled. */
void qstats_thread_init(int host_tid, int host_core);

/* For each vCPU run by the calling host thread, after qstats_thread_init(). */
void qstats_vcpu_attach(struct CPUState *cpu);

/*
 * Call after qemu_wait_io_event(): if the thread blocked there because @cpu
 * was stopped (startup, snapshot, pause), restart the timing of the current
 * quantum so that the paused time is not accounted to it.
 */
void qstats_resume_if_needed(struct CPUState *cpu);

/* Around the wait at the global barrier. */
static inline void qstats_barrier_enter(void) {
  QStatsHost *s = qstats_self;
  if (s) {
    s->t_bar_enter = qstats_now_ns();
    s->cpu_bar_enter = qstats_thread_cpu_ns();
    s->in_barrier = true;
  }
}
void qstats_barrier_exit(uint64_t generation, uint32_t flags);

/* The barrier returned inside the same quantum (woken up): keep the quantum
 * open. */
static inline void qstats_barrier_cancel(void) {
  QStatsHost *s = qstats_self;
  if (s) {
    s->in_barrier = false;
    s->pending_flags |= QSTATS_WAKE_INSIDE;
  }
}

/*
 * Called by dynamic_barrier_polling_wait() in the thread that releases the
 * barrier: at the start of its serial section and right before it publishes
 * the new generation.
 */
static inline void qstats_release_begin(void) {
  QStatsHost *s = qstats_self;
  if (s) {
    s->pending_flags |= QSTATS_LAST_ARRIVER;
    s->t_release_start = qstats_now_ns();
  }
}
static inline void qstats_release_end(void) {
  QStatsHost *s = qstats_self;
  if (s) {
    s->release_ns = qstats_now_ns() - s->t_release_start;
  }
}

/* Around tcg_cpus_exec() / cpu_exec_step_atomic() of a vCPU. */
static inline void qstats_exec_begin(void) {
  QStatsHost *s = qstats_self;
  if (s) {
    s->t_exec_start = qstats_now_ns();
    s->in_exec = true;
  }
}
void qstats_exec_end(struct CPUState *cpu);

/* Emits the vCPU record of the quantum that just ended and resets its counters.
 */
void qstats_vcpu_end_quantum(struct CPUState *cpu, uint64_t generation);

/* Around a switch between co-located vCPUs (time multiplexing). */
static inline void qstats_ctx_begin(void) {
  if (qstats_self) {
    qstats_self->t_ctx_start = qstats_now_ns();
  }
}
static inline void qstats_ctx_end(void) {
  if (qstats_self) {
    qstats_self->ctx_acc += qstats_now_ns() - qstats_self->t_ctx_start;
    qstats_self->n_switches++;
  }
}

#endif /* SYSEMU_QUANTUM_STATS_H */
