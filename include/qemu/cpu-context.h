/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_CPU_CONTEXT_H
#define QEMU_CPU_CONTEXT_H

#include "qemu/typedefs.h"

/*
 * Select a callback's CPU in the supplied TLS slot (pass &current_cpu),
 * returning the previous selection. No TLS symbol is redeclared here.
 * Pair with cpu_context_restore() on normal return, including nested calls.
 * This changes TLS only: it grants no CPU/thread ownership, BQL, RCU or
 * exclusive-execution permission. The caller must already be allowed to run
 * the callback here. Callbacks must not longjmp past the restoration, execute
 * a sibling's guest code, or block in an incompatible exclusive section.
 */
static inline CPUState *cpu_context_save(CPUState **slot, CPUState *cpu)
{
    CPUState *saved = *slot;

    *slot = cpu;
    return saved;
}

static inline void cpu_context_restore(CPUState **slot, CPUState *saved)
{
    *slot = saved;
}

#endif /* QEMU_CPU_CONTEXT_H */
