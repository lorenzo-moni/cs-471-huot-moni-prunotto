/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUANTUM_WORKER_CREATION_H
#define QUANTUM_WORKER_CREATION_H

#include "qemu/typedefs.h"

/*
 * Fixed-pair worker backend for 2:1, 4:2 and 8:4.
 * Call under BQL, with dense CPU indices and process-lifetime CPU ownership.
 * Callers must prevent hotplug, unplug and unrealization.
 */
void quantum_worker_creation_attach(CPUState *cpu);
void quantum_worker_creation_kick(CPUState *cpu);
/* Called under BQL after machine realization; failure leaves membership open. */
bool quantum_worker_creation_seal(Error **errp);

#endif
