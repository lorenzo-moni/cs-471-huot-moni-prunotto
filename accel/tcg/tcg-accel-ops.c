/*
 * QEMU TCG vCPU common functionality
 *
 * Functionality common to all TCG vCPU variants: mttcg, rr and icount.
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
#include "sysemu/tcg.h"
#include "sysemu/replay.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/quantum.h"
#include "sysemu/quantum-stats.h"
#include "sysemu/asid-coeff.h"
#include "qemu/main-loop.h"
#include "qemu/guest-random.h"
#include "qemu/timer.h"
#include "exec/exec-all.h"
#include "exec/hwaddr.h"
#include "exec/gdbstub.h"
#include <math.h>

#include "tcg-accel-ops.h"
#include "tcg-accel-ops-mttcg.h"
#include "tcg-accel-ops-rr.h"
#include "tcg-accel-ops-icount.h"
#include "tcg-accel-ops-quantum.h"
#include "tcg-accel-ops-quantum-rr.h"

/* common functionality among all TCG variants */

void tcg_cpu_init_cflags(CPUState *cpu, bool parallel)
{
    uint32_t cflags;

    /*
     * Include the cluster number in the hash we use to look up TBs.
     * This is important because a TB that is valid for one cluster at
     * a given physical address and set of CPU flags is not necessarily
     * valid for another:
     * the two clusters may have different views of physical memory, or
     * may have different CPU features (eg FPU present or absent).
     */
    cflags = cpu->cluster_index << CF_CLUSTER_SHIFT;

    cflags |= parallel ? CF_PARALLEL : 0;
    cflags |= icount_enabled() ? CF_USE_ICOUNT : 0;
    cpu->tcg_cflags |= cflags;
}

void tcg_cpus_destroy(CPUState *cpu)
{
    cpu_thread_signal_destroyed(cpu);
}

int tcg_cpus_exec(CPUState *cpu)
{
    int ret;
    assert(tcg_enabled());
    cpu_exec_start(cpu);
    ret = cpu_exec(cpu);
    cpu_exec_end(cpu);
    return ret;
}

/* mask must never be zero, except for A20 change call */
void tcg_handle_interrupt(CPUState *cpu, int mask)
{
    g_assert(qemu_mutex_iothread_locked());

    cpu->interrupt_request |= mask;

    /* qstats: count interrupts to a vCPU that stays idle until the next quantum. */
    if (unlikely(qstats_late_wake_on)) {
        qstats_note_interrupt(cpu);
    }

    /*
     * If called from iothread context, wake the target cpu in
     * case its halted.
     */
    if (!qemu_cpu_is_self(cpu)) {
        qemu_cpu_kick(cpu);
    } else {
        qatomic_set(&cpu_neg(cpu)->icount_decr.u16.high, -1);
    }
}

static bool tcg_supports_guest_debug(void)
{
    return true;
}

/* Translate GDB watchpoint type to a flags value for cpu_watchpoint_* */
static inline int xlat_gdb_type(CPUState *cpu, int gdbtype)
{
    static const int xlat[] = {
        [GDB_WATCHPOINT_WRITE]  = BP_GDB | BP_MEM_WRITE,
        [GDB_WATCHPOINT_READ]   = BP_GDB | BP_MEM_READ,
        [GDB_WATCHPOINT_ACCESS] = BP_GDB | BP_MEM_ACCESS,
    };

    CPUClass *cc = CPU_GET_CLASS(cpu);
    int cputype = xlat[gdbtype];

    if (cc->gdb_stop_before_watchpoint) {
        cputype |= BP_STOP_BEFORE_ACCESS;
    }
    return cputype;
}

static int tcg_insert_breakpoint(CPUState *cs, int type, vaddr addr, vaddr len)
{
    CPUState *cpu;
    int err = 0;

    switch (type) {
    case GDB_BREAKPOINT_SW:
    case GDB_BREAKPOINT_HW:
        CPU_FOREACH(cpu) {
            err = cpu_breakpoint_insert(cpu, addr, BP_GDB, NULL);
            if (err) {
                break;
            }
        }
        return err;
    case GDB_WATCHPOINT_WRITE:
    case GDB_WATCHPOINT_READ:
    case GDB_WATCHPOINT_ACCESS:
        CPU_FOREACH(cpu) {
            err = cpu_watchpoint_insert(cpu, addr, len,
                                        xlat_gdb_type(cpu, type), NULL);
            if (err) {
                break;
            }
        }
        return err;
    default:
        return -ENOSYS;
    }
}

static int tcg_remove_breakpoint(CPUState *cs, int type, vaddr addr, vaddr len)
{
    CPUState *cpu;
    int err = 0;

    switch (type) {
    case GDB_BREAKPOINT_SW:
    case GDB_BREAKPOINT_HW:
        CPU_FOREACH(cpu) {
            err = cpu_breakpoint_remove(cpu, addr, BP_GDB);
            if (err) {
                break;
            }
        }
        return err;
    case GDB_WATCHPOINT_WRITE:
    case GDB_WATCHPOINT_READ:
    case GDB_WATCHPOINT_ACCESS:
        CPU_FOREACH(cpu) {
            err = cpu_watchpoint_remove(cpu, addr, len,
                                        xlat_gdb_type(cpu, type));
            if (err) {
                break;
            }
        }
        return err;
    default:
        return -ENOSYS;
    }
}

static inline void tcg_remove_all_breakpoints(CPUState *cpu)
{
    cpu_breakpoint_remove_all(cpu, BP_GDB);
    cpu_watchpoint_remove_all(cpu, BP_GDB);
}

static void tcg_accel_ops_init(AccelOpsClass *ops)
{
    if (quantum_enabled() && quantum_rr_enabled()) {
        /* Quantum-RR mode: single-threaded round-robin with quantum */
        ops->create_vcpu_thread = quantum_rr_start_vcpu_thread;
        ops->kick_vcpu_thread = quantum_rr_kick_vcpu_thread;
        quantum_rr_initialize();
    } else if (quantum_enabled()) {
        ops->create_vcpu_thread = quantum_start_vcpu_thread;
        ops->kick_vcpu_thread = quantum_kick_vcpu_thread;
        ops->handle_interrupt = tcg_handle_interrupt;
        quantum_initialize_barrier();
    }  else if (qemu_tcg_mttcg_enabled()) {
        ops->create_vcpu_thread = mttcg_start_vcpu_thread;
        ops->kick_vcpu_thread = mttcg_kick_vcpu_thread;
        ops->handle_interrupt = tcg_handle_interrupt;
    } else {
        ops->create_vcpu_thread = rr_start_vcpu_thread;
        ops->kick_vcpu_thread = rr_kick_vcpu_thread;

        if (icount_enabled()) {
            ops->handle_interrupt = icount_handle_interrupt;
            ops->get_virtual_clock = icount_get;
            ops->get_elapsed_ticks = icount_get;
        } else {
            ops->handle_interrupt = tcg_handle_interrupt;
        }
    }

    ops->supports_guest_debug = tcg_supports_guest_debug;
    ops->insert_breakpoint = tcg_insert_breakpoint;
    ops->remove_breakpoint = tcg_remove_breakpoint;
    ops->remove_all_breakpoints = tcg_remove_all_breakpoints;
}

static void tcg_accel_ops_class_init(ObjectClass *oc, void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);

    ops->ops_init = tcg_accel_ops_init;
}

static const TypeInfo tcg_accel_ops_type = {
    .name = ACCEL_OPS_NAME("tcg"),

    .parent = TYPE_ACCEL_OPS,
    .class_init = tcg_accel_ops_class_init,
    .abstract = true,
};
module_obj(ACCEL_OPS_NAME("tcg"));

static void tcg_accel_ops_register_types(void)
{
    type_register_static(&tcg_accel_ops_type);
}
type_init(tcg_accel_ops_register_types);

/* Parse core_info.csv file and fill the core_info_table */
void tcg_parse_core_info_file(const char *file_name, core_meta_info_t *core_info_table, int max_cores)
{
    // By default, all cores' IPC is 0, which means not managed by the IPC and the quantum.
    for (int i = 0; i < max_cores; ++i) {
        core_info_table[i].host_core_idx = i;
        core_info_table[i].ipns = 0.0;
        core_info_table[i].is_constant = false;
        core_info_table[i].coeffs = (bx_coeff_t){0};
    }

    // Load the IPC from the file.
    FILE *fp = fopen(file_name, "r");
    if (!fp) {
        return;  // File not found, use defaults
    }

    char line[1024];
    int core_id = 0;

    // Read and validate header
    if (fgets(line, sizeof(line), fp) == NULL) {
        fprintf(stderr, "Error: Empty core_info.csv file\n");
        fclose(fp);
        exit(1);
    }

    // Check for old format (2 columns)
    if (strstr(line, "ipns") != NULL && strstr(line, "affinity_core_idx") != NULL) {
        fprintf(stderr, "Error: core_info.csv uses deprecated 2-column format.\n");
        fprintf(stderr, "Please migrate to the 9-column coefficient format.\n");
        fclose(fp);
        exit(2);
    }

    // Check for new format header
    if (strstr(line, "host_core_idx") == NULL ||
        strstr(line, "model_type") == NULL ||
        strstr(line, "bx_private_icache_miss_coeff") == NULL) {
        fprintf(stderr, "Error: Invalid core_info.csv header. Expected 9-column format.\n");
        fclose(fp);
        exit(1);
    }

    // Parse data rows
    while (fgets(line, sizeof(line), fp) != NULL) {
        // Skip empty lines and comments
        if (line[0] == '\n' || line[0] == '#' || line[0] == '\0') {
            continue;
        }

        // Trim trailing newline
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        char *saveptr = NULL;
        char *token = strtok_r(line, ",", &saveptr);
        if (!token) {
            fprintf(stderr, "Error: Row %d: failed to parse host_core_idx\n", core_id);
            fclose(fp);
            exit(1);
        }
        core_info_table[core_id].host_core_idx = strtoll(token, NULL, 10);

        token = strtok_r(NULL, ",", &saveptr);
        if (!token) {
            fprintf(stderr, "Error: Row %d: failed to parse model_type\n", core_id);
            fclose(fp);
            exit(1);
        }

        // Trim whitespace from model_type
        while (*token == ' ' || *token == '\t') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\t' || *end == '\r')) *end-- = '\0';

        bool is_constant_model = false;
        if (strcmp(token, "constant") == 0) {
            is_constant_model = true;
        } else if (strcmp(token, "ipc-model") == 0) {
            is_constant_model = false;
        } else {
            fprintf(stderr, "Error: Row %d: model_type must be 'constant' or 'ipc-model', got '%s'\n",
                    core_id, token);
            fclose(fp);
            exit(1);
        }

        token = strtok_r(NULL, ",", &saveptr);
        if (!token) {
            fprintf(stderr, "Error: Row %d: failed to parse ipns\n", core_id);
            fclose(fp);
            exit(1);
        }
        double ipns = strtod(token, NULL);
        if (ipns < 0) {
            fprintf(stderr, "Error: Row %d: ipns must be non-negative, got %f\n", core_id, ipns);
            fclose(fp);
            exit(1);
        }
        core_info_table[core_id].ipns = ipns;

        // For constant model, coefficients are not parsed from CSV
        if (is_constant_model) {
            core_info_table[core_id].is_constant = true;
            // All feature coefficients remain 0; timing uses ipns directly via ip100ns path.
            // Just verify it's positive
            if (ipns <= 0) {
                fprintf(stderr, "Error: Row %d: constant model requires positive ipns, got %f\n",
                        core_id, ipns);
                fclose(fp);
                exit(1);
            }
        } else {
            // IPC model: validate ipns == 1.0
            if (fabs(ipns - 1.0) > 1e-9) {
                fprintf(stderr, "Error: Row %d: ipc-model requires ipns=1.0, got %f\n",
                        core_id, ipns);
                fclose(fp);
                exit(1);
            }

            // Parse all 6 coefficients (stored as fixed-point: value * 1000)
            const char *coeff_names[] = {
                "bx_private_icache_miss_coeff",
                "bx_private_dcache_miss_coeff",
                "bx_shared_cache_miss_coeff",
                "bx_bp_miss_coeff",
                "bx_drain_store_buffer_coeff",
                "bx_instruction_coeff"
            };
            uint32_t *coeffs[] = {
                &core_info_table[core_id].coeffs.bx_private_icache_miss_coeff,
                &core_info_table[core_id].coeffs.bx_private_dcache_miss_coeff,
                &core_info_table[core_id].coeffs.bx_shared_cache_miss_coeff,
                &core_info_table[core_id].coeffs.bx_bp_miss_coeff,
                &core_info_table[core_id].coeffs.bx_drain_store_buffer_coeff,
                &core_info_table[core_id].coeffs.bx_instruction_coeff
            };

            for (int i = 0; i < 6; i++) {
                token = strtok_r(NULL, ",", &saveptr);
                if (!token) {
                    fprintf(stderr, "Error: Row %d: failed to parse %s\n",
                            core_id, coeff_names[i]);
                    fclose(fp);
                    exit(1);
                }
                double val = strtod(token, NULL);
                if (val < 0) {
                    fprintf(stderr, "Error: Row %d: %s must be non-negative, got %f\n",
                            core_id, coeff_names[i], val);
                    fclose(fp);
                    exit(1);
                }
                // Convert to fixed-point: multiply by 1000 and round to nearest integer
                uint64_t fixed = (uint64_t)(val * 1000.0 + 0.5);
                if (fixed > UINT32_MAX) {
                    fprintf(stderr, "Error: Row %d: %s value %f overflows uint32 in fixed-point\n",
                            core_id, coeff_names[i], val);
                    fclose(fp);
                    exit(1);
                }
                *coeffs[i] = (uint32_t)fixed;
            }
        }

        core_id += 1;
        if (core_id >= max_cores) {
            fprintf(stderr, "Error: Too many cores in core_info.csv (max %d)\n", max_cores);
            fclose(fp);
            exit(1);
        }
    }

    fclose(fp);
}

/* -----------------------------------------------------------------------
 * Per-ASID coefficient table
 * ----------------------------------------------------------------------- */

/*
 * Global ASID coefficient hash table.  Written once at startup
 * (tcg_parse_asid_info_file), then read-only — concurrent reads from
 * multiple vCPU threads are therefore safe without locking.
 *
 * Keys:   GUINT_TO_POINTER((guint)asid)  [uint16_t ASID, fits in pointer]
 * Values: heap-allocated asid_coeff_t *  (freed by the table destructor)
 */
static GHashTable *asid_coeff_table;

GHashTable *tcg_get_asid_coeff_table(void)
{
    return asid_coeff_table;
}

void tcg_parse_asid_info_file(const char *file_name)
{
    FILE *fp = fopen(file_name, "r");
    if (!fp) {
        /* Optional file — silently ignore if absent */
        return;
    }

    /* Create table on first successful open */
    asid_coeff_table = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                              NULL, g_free);

    char line[1024];

    /* Read and validate header */
    if (fgets(line, sizeof(line), fp) == NULL) {
        fprintf(stderr, "Error: Empty asid_info.csv file\n");
        fclose(fp);
        exit(1);
    }

    if (strstr(line, "asid") == NULL ||
        strstr(line, "bx_private_icache_miss_coeff") == NULL) {
        fprintf(stderr,
                "Error: Invalid asid_info.csv header. "
                "Expected: asid,bx_private_icache_miss_coeff,...\n");
        fclose(fp);
        exit(1);
    }

    int row = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        /* Skip blank lines and comments */
        if (line[0] == '\n' || line[0] == '#' || line[0] == '\0') {
            continue;
        }

        /* Trim trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        char *saveptr = NULL;

        /* Column 1: asid */
        char *token = strtok_r(line, ",", &saveptr);
        if (!token) {
            fprintf(stderr, "Error: asid_info.csv row %d: failed to parse asid\n", row);
            fclose(fp);
            exit(1);
        }
        uint64_t asid = strtoull(token, NULL, 10);
        if (asid > 0xFFFF) {
            fprintf(stderr,
                    "Error: asid_info.csv row %d: ASID %lu exceeds 16-bit range\n",
                    row, asid);
            fclose(fp);
            exit(1);
        }

        asid_coeff_t *entry = g_new0(asid_coeff_t, 1);

        /* Columns 2–7: the 6 bx_* coefficients */
        const char *coeff_names[] = {
            "bx_private_icache_miss_coeff",
            "bx_private_dcache_miss_coeff",
            "bx_shared_cache_miss_coeff",
            "bx_bp_miss_coeff",
            "bx_drain_store_buffer_coeff",
            "bx_instruction_coeff"
        };
        uint32_t *coeffs[] = {
            &entry->bx_private_icache_miss_coeff,
            &entry->bx_private_dcache_miss_coeff,
            &entry->bx_shared_cache_miss_coeff,
            &entry->bx_bp_miss_coeff,
            &entry->bx_drain_store_buffer_coeff,
            &entry->bx_instruction_coeff
        };

        for (int i = 0; i < 6; i++) {
            token = strtok_r(NULL, ",", &saveptr);
            if (!token) {
                fprintf(stderr,
                        "Error: asid_info.csv row %d: failed to parse %s\n",
                        row, coeff_names[i]);
                g_free(entry);
                fclose(fp);
                exit(1);
            }
            double val = strtod(token, NULL);
            if (val < 0) {
                fprintf(stderr,
                        "Error: asid_info.csv row %d: %s must be non-negative, got %f\n",
                        row, coeff_names[i], val);
                g_free(entry);
                fclose(fp);
                exit(1);
            }
            /* Fixed-point: multiply by 1000 and round */
            uint64_t fixed = (uint64_t)(val * 1000.0 + 0.5);
            if (fixed > UINT32_MAX) {
                fprintf(stderr,
                        "Error: asid_info.csv row %d: %s value %f overflows uint32 in fixed-point\n",
                        row, coeff_names[i], val);
                g_free(entry);
                fclose(fp);
                exit(1);
            }
            *coeffs[i] = (uint32_t)fixed;
        }

        g_hash_table_insert(asid_coeff_table,
                            GUINT_TO_POINTER((guint)asid),
                            entry);
        row++;
    }

    fclose(fp);
    fprintf(stderr, "Loaded %d ASID entries from %s\n", row, file_name);
}
