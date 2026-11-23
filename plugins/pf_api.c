/**
 * @file pf_api.c
 *
 * @brief This file defines ParaFlex's API for QEMU.
 *
 *
 * Some plugins defined in this file can be removed in the future if QEMU
 * officially supports them.
 *
 * Currently, all plugins are only for ARM processor.
 */

#include <stdint.h>
#ifndef CONFIG_USER_ONLY

#include "qemu/osdep.h"
#include "qemu/plugin.h"
#include "qemu/log.h"
#include "qemu/qemu-plugin.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/cpus.h"
#include "tcg/tcg.h"
#include "exec/exec-all.h"
#include "exec/ram_addr.h"
#include "disas/disas.h"
#include "plugin.h"
#include "qemu/plugin-memory.h"
#include "hw/boards.h"
#include "softmmu/timers-state.h"
#include "exec/cpu-common.h"
#include "qemu/plugin-pf.h"
#include "sysemu/quantum.h"
#include "sysemu/runstate.h"
#include "qemu/main-loop.h"
#include "qemu/atomic.h"
#include "migration/snapshot.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "qemu/dynamic_barrier.h"


// All cyan callback functions
qemu_plugin_vcpu_branch_resolved_cb_t pf_br_cb = NULL;
qemu_plugin_snapshot_cb_t pf_savevm_cb = NULL;
qemu_plugin_snapshot_cb_t pf_loadvm_cb = NULL;
qemu_plugin_event_loop_poll_cb_t pf_el_pool_cb = NULL;
qemu_plugin_periodic_check_cb_t pf_periodic_check_cb = NULL;
qemu_plugin_flushing_local_tlb_t pf_flushing_local_tlb_cb = NULL;
qemu_plugin_on_deliver_interrupt_cb_t pf_on_deliver_interrupt_cb = NULL;
qemu_plugin_on_deliver_interrupt_with_time_cb_t pf_on_deliver_interrupt_with_time_cb = NULL;
qemu_plugin_save_statistics_callback_t pf_save_statistics_cb = NULL;
qemu_plugin_record_statistics_cb_t pf_record_statistics_cb = NULL;

/* Global statistics array exposed to plugins - aligned to prevent false sharing */
struct qemu_plugin_exposed_statistics g_exposed_statistics[QEMU_PLUGIN_MAX_CORES] __attribute__((aligned(64)));
bool g_statistics_managed_by_plugin = false;

/* Global timing info for host-side checkpoint time breakdown */
struct qemu_plugin_timing_info g_timing_info __attribute__((aligned(64))) = {0};

struct qemu_plugin_exposed_statistics *qemu_plugin_get_exposed_statistics(uint32_t core_idx)
{
    g_statistics_managed_by_plugin = true; // this means the plugin is managing the statistics.
    if (core_idx >= QEMU_PLUGIN_MAX_CORES) {
        return NULL;
    }
    return &g_exposed_statistics[core_idx];
}

struct qemu_plugin_timing_info *qemu_plugin_get_timing_info(void)
{
    return &g_timing_info;
}

uint64_t qemu_plugin_read_cpu_integer_register(int reg_index) {
  g_assert_cmpstr(TARGET_NAME, ==, "aarch64");

  g_assert(reg_index >= 0 && reg_index < 32);

  CPUState *cpu = current_cpu;
  g_assert(cpu != NULL);

  return (uint64_t)cpu->env_ptr->xregs[reg_index];
}

uint64_t qemu_plugin_read_ttbr_el1(int which_tbbr) {
  g_assert_cmpstr(TARGET_NAME, ==, "aarch64");

  CPUState *cpu = current_cpu;
  g_assert(cpu != NULL);

  g_assert(which_tbbr == 0 || which_tbbr == 1);

  if (which_tbbr == 0) {
    return (uint64_t)cpu->env_ptr->cp15.ttbr0_el[1];
  } else {
    return (uint64_t)cpu->env_ptr->cp15.ttbr1_el[1];
  }
}

uint64_t qemu_plugin_read_tcr_el1(void) {
  g_assert_cmpstr(TARGET_NAME, ==, "aarch64");

  CPUState *cpu = current_cpu;
  g_assert(cpu != NULL);

  return (uint64_t)cpu->env_ptr->cp15.tcr_el[1];
}

const uint64_t *qemu_plugin_hwaddr_translate_walk_trace(
    const struct qemu_plugin_hwaddr *hwaddr) {
  g_assert_cmpstr(TARGET_NAME, ==, "aarch64");
  if (hwaddr) {
    if (!hwaddr->is_io) {
      return &hwaddr->v.ram.walk_trace[0];
    }
  }
  return NULL;
}

void qemu_plugin_read_physical_memory(uint64_t physical_address, uint64_t size,
                                      void *buf) {
  cpu_physical_memory_rw(physical_address, buf, size, false);
}

void qemu_plugin_write_physical_memory(uint64_t physical_address, uint64_t size,
                                       const void *buf) {
  cpu_physical_memory_rw(physical_address, (void *)buf, size, true);
}

bool qemu_plugin_register_vcpu_branch_resolved_cb(qemu_plugin_vcpu_branch_resolved_cb_t cb) {
  if (pf_br_cb) {
    return false;
  }
  pf_br_cb = cb;
  return true;
}

// uint8_t qemu_plugin_get_cvnz(void) {
//   g_assert_cmpstr(TARGET_NAME, ==, "aarch64");

//   CPUState *cpu = current_cpu;
//   g_assert(cpu != NULL);

//   uint8_t cvnz = 0;

//   // C
//   cvnz |= (cpu->env_ptr->CF == 1 ? 1 : 0) << 0;

//   // V
//   cvnz |= ((cpu->env_ptr->VF & 0x80000000) != 0 ? 1 : 0) << 1;

//   // N
//   cvnz |= ((cpu->env_ptr->NF & 0x80000000) != 0 ? 1 : 0) << 2;

//   // Z
//   cvnz |= (cpu->env_ptr->ZF == 0 ? 1 : 0) << 3;

//   return cvnz;
// }

// uint64_t helper_autia(CPUARMState *env, uint64_t pointer, uint64_t modifier);
// uint64_t helper_autib(CPUARMState *env, uint64_t pointer, uint64_t modifier);

// uint64_t qemu_plugin_resolve_pointer_authentication(uint64_t pointer, uint64_t key, uint64_t modifier){
//   g_assert_cmpstr(TARGET_NAME, ==, "aarch64");

//   CPUState *cpu = current_cpu;
//   g_assert(cpu != NULL);

//   if (key == 0) {
//     return helper_autia(cpu->env_ptr, pointer, modifier);
//   }

//   return helper_autib(cpu->env_ptr, pointer, modifier);
// }

uint64_t qemu_plugin_read_pc_vpn(void) {

  CPUState *cpu = current_cpu;
  g_assert(cpu != NULL);

  return (uint64_t)cpu->env_ptr->pc >> 12;
}

bool qemu_plugin_register_savevm_cb(qemu_plugin_snapshot_cb_t cb) {
  if (pf_savevm_cb) {
    return false;
  }
  pf_savevm_cb = cb;
  return true;
}

bool qemu_plugin_register_loadvm_cb(qemu_plugin_snapshot_cb_t cb) {
  if (pf_loadvm_cb) {
    return false;
  }
  pf_loadvm_cb = cb;
  return true;
}

uint64_t qemu_plugin_get_quantum_size(void) {
  if(quantum_enabled()) return quantum_size;

  printf("Warning: quantum is not enabled, return 0\n");
  return 0;
}

void qemu_plugin_savevm(const char *name, qemu_plugin_snapshot_format_t format) {
  Error *err = NULL;
  bool saved;

  if (quantum_workers_enabled()) {
    g_assert(qemu_mutex_iothread_locked());
    if (!quantum_worker_checkpoint_pending) {
      error_report("N worker plugin save requires a global checkpoint boundary");
      _exit(EXIT_FAILURE);
    }

  }
  saved = save_snapshot(name, true, NULL, false, NULL, (SnapshotFormat)format, &err);

  if (err) {
    error_reportf_err(err, "Error: ");
  }
  if (quantum_workers_enabled()) {
    if (!saved) {
      error_report("N worker checkpoint capture failed: %s", name);
      _exit(EXIT_FAILURE);
    }
    /* save_snapshot has finished QEMU and plugin capture and its resume. */
    quantum_worker_checkpoint_pending = false;
    fprintf(stderr, "N checkpoint complete generation=%u name=%s\n",
            quantum_worker_generation, name);
  }
}

bool qemu_plugin_register_event_loop_poll_cb(qemu_plugin_event_loop_poll_cb_t cb) {
  if (pf_el_pool_cb) {
    return false;
  }
  pf_el_pool_cb = cb;
  return true;
}

bool qemu_plugin_is_icount_mode(void) {
  return icount_enabled();
}

bool qemu_plugin_register_periodic_check_cb(qemu_plugin_periodic_check_cb_t cb) {
  if (pf_periodic_check_cb) {
    return false;
  }

  assert(
    ((icount_enabled() == true && icount_checking_period != 0) ||
    (quantum_enabled() == true && quantum_check_threshold != 0) ) &&
    "icount or quantum periodic check is not enabled"
  );

  pf_periodic_check_cb = cb;
  return true;
}

uint64_t qemu_plugin_get_vcpu_vtime(uint32_t cpu_idx) {
  CPUState *cpu = qemu_get_cpu(cpu_idx);
  if (!cpu) return 0;
  return cpu->vts;
}

bool qemu_plugin_register_flushing_local_tlb_cb(
    qemu_plugin_flushing_local_tlb_t cb) {

  if (pf_flushing_local_tlb_cb) {
    return false;
  }

  pf_flushing_local_tlb_cb = cb;
  return true;
}

bool qemu_plugin_register_on_deliver_interrupt_cb(
    qemu_plugin_on_deliver_interrupt_cb_t cb) {

  if (pf_on_deliver_interrupt_cb) {
    return false;
  }

  pf_on_deliver_interrupt_cb = cb;
  return true;
}

bool qemu_plugin_register_plugin_quantum_generation_increment_variable(
    uint64_t *var) {
  if (!quantum_enabled()) {
    return false;
  }

  if (quantum_workers_enabled()) {
    if (!var || quantum_worker_plugin_generation) {
      return false;
    }
    quantum_worker_plugin_generation = var;
    qatomic_set(var, quantum_worker_generation);
    return true;
  }
  if (quantum_barrier.plugin_quantum_generation != NULL) {
    return false;
  }

  quantum_barrier.plugin_quantum_generation = var;

  return true;
}

bool qemu_plugin_register_save_statistics_callback(
    qemu_plugin_save_statistics_callback_t cb) {
  if (pf_save_statistics_cb) {
    return false;
  }

  pf_save_statistics_cb = cb;
  return true;
}

bool qemu_plugin_register_record_statistics_cb(
    qemu_plugin_record_statistics_cb_t cb) {
  if (pf_record_statistics_cb) {
    return false;
  }

  pf_record_statistics_cb = cb;
  return true;
}

uint32_t *qemu_plugin_get_global_quantum_generation_ptr(void) {
  if (quantum_workers_enabled()) {
    return &quantum_worker_generation;
  }
  return &quantum_barrier.return_value.two_32.generation;
}

uint64_t *qemu_plugin_get_vcpu_target_time_ptr(uint32_t cpu_idx) {
  CPUState *cpu = qemu_get_cpu(cpu_idx);
  if (!cpu) return NULL;
  return &cpu->vts;
}

uint32_t *qemu_plugin_get_vcpu_waiting_for_quantum_ptr(uint32_t cpu_idx) {
  CPUState *cpu = qemu_get_cpu(cpu_idx);
  if (!cpu) return NULL;
  return &cpu->waiting_for_quantum;
}

bool qemu_plugin_register_on_deliver_interrupt_with_time_cb(
    qemu_plugin_on_deliver_interrupt_with_time_cb_t cb) {

  if (pf_on_deliver_interrupt_with_time_cb) {
    return false;
  }

  pf_on_deliver_interrupt_with_time_cb = cb;
  return true;
}


#endif
