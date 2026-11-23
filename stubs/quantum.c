#include <stdint.h>
#include "qemu/osdep.h"
#include "qemu/dynamic_barrier.h"

uint64_t quantum_size = 0;
uint64_t quantum_workers = 0;

dynamic_barrier_polling_t quantum_barrier;

void dynamic_barrier_push_delayed_interrupt(dynamic_barrier_polling_t *barrier, qemu_irq irq, int level) {
    assert(false);
}
