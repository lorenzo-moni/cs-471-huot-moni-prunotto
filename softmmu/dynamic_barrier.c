#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "qemu/osdep.h"
#include "qemu/dynamic_barrier.h"
#include "hw/core/cpu.h"
#include "hw/irq.h"
#include "qemu/timer.h"
#include "sysemu/cpu-timers.h"
#include "qemu/main-loop.h"
#include "sysemu/cpus.h"
#include "sysemu/runstate.h"
#include "sysemu/quantum.h"
#include "sysemu/quantum-stats.h"
#include "qemu/plugin-pf.h"


typedef struct delayed_interrupt_info {
    qemu_irq irq;
    int level;
} delayed_interrupt_info_t;

static uint64_t get_current_timestamp_ns(void) {
    struct timespec ts;
    // Get the current time
    clock_gettime(CLOCK_REALTIME, &ts);

    // Convert to nanoseconds
    // tv_sec is seconds, tv_nsec is nanoseconds
    uint64_t timestamp_ns = (uint64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;

    return timestamp_ns;
}

static void *report_time_peridically(void *arg) {
    dynamic_barrier_polling_t *barrier = arg;
    while (1) {
        sleep(10);
        uint64_t total_diff = barrier->total_diff;
        uint64_t generation = barrier->return_value.two_32.generation;
        printf("Total time spent in the barrier: %lu ns, generation: %lu, normalized_diff: %lf\n", total_diff, generation, (double)total_diff / generation);
    }
    return NULL;
}

// create a timestamp of each thread.
// static __thread uint64_t thread_start_quantum_timestamp = 0;

// // Initialize the dynamic barrier
// int dynamic_barrier_init(dynamic_barrier_t *barrier, int initial_threshold) {
//     int status;

//     status = pthread_mutex_init(&barrier->mutex, NULL);
//     if (status != 0) return status;

//     status = pthread_cond_init(&barrier->cond, NULL);
//     if (status != 0) {
//         pthread_mutex_destroy(&barrier->mutex);
//         return status;
//     }

//     barrier->threshold = initial_threshold;
//     barrier->count = 0;
//     barrier->generation = 0;

//     // start another thread to call report_time_peridically.

//     return 0;
// }

// // Destroy the dynamic barrier
// int dynamic_barrier_destroy(dynamic_barrier_t *barrier) {
//     pthread_mutex_destroy(&barrier->mutex);
//     pthread_cond_destroy(&barrier->cond);
//     return 0;
// }

// // Wait on the barrier
// int dynamic_barrier_wait(dynamic_barrier_t *barrier) {
//     pthread_mutex_lock(&barrier->mutex);

//     int gen = barrier->generation;
//     barrier->count++;

//     if (barrier->count >= barrier->threshold) {
//         barrier->generation++;
//         barrier->count = 0;
//         pthread_cond_broadcast(&barrier->cond);
//     } else {
//         while (gen == barrier->generation) {
//             pthread_cond_wait(&barrier->cond, &barrier->mutex);
//         }
//     }

//     pthread_mutex_unlock(&barrier->mutex);
//     return 0;
// }



// int dynamic_barrier_increase_by_1(dynamic_barrier_t *barrier) {
//     pthread_mutex_lock(&barrier->mutex);
//     barrier->threshold = barrier->threshold + 1;
//     if (barrier->count >= barrier->threshold) {
//         barrier->generation++;
//         barrier->count = 0;
//         pthread_cond_broadcast(&barrier->cond);
//     }
//     pthread_mutex_unlock(&barrier->mutex);
//     return 0;
// }

// int dynamic_barrier_decrease_by_1(dynamic_barrier_t *barrier) {
//     pthread_mutex_lock(&barrier->mutex);
//     if (barrier->threshold <= 0) {
//         pthread_mutex_unlock(&barrier->mutex);
//         return -1;
//     }
//     barrier->threshold = barrier->threshold - 1;
//     if (barrier->count >= barrier->threshold) {
//         barrier->generation++;
//         barrier->count = 0;
//         pthread_cond_broadcast(&barrier->cond);
//     }
//     pthread_mutex_unlock(&barrier->mutex);
//     return 0;
// }


dynamic_barrier_polling_t quantum_barrier;

int dynamic_barrier_polling_init(dynamic_barrier_polling_t *barrier, int initial_threshold) {
    barrier->lock.next_ticket = 0;
    barrier->lock.now_serving = 0;

    barrier->threshold = initial_threshold;
    barrier->count = 0;
    barrier->return_value.two_32.generation = 0;
    barrier->return_value.two_32.stop_request = 0;

    assert(quantum_enabled());

    for (int i = 0; i < 128; i++) {
        barrier->histogram[i] = create_histogram(100, 1e5, 101e5);
    }

    barrier->current_cycle = 0;
    barrier->next_check_threshold = quantum_check_threshold;

    barrier->handling_interrupts = false;

    barrier->delayed_interrupts = g_queue_new();

    barrier->stop_request = false;

    return 0;
}

int dynamic_barrier_polling_destroy(dynamic_barrier_polling_t *barrier) {
    for (int i = 0; i < 128; i++) {
        free_histogram(barrier->histogram[i]);
    }

    return 0;
}

static void dynamic_barrier_polling_acquire_lock(dynamic_barrier_polling_t *barrier) {
    uint64_t my_ticket = atomic_fetch_add(&barrier->lock.next_ticket, 1);
    while (atomic_load(&barrier->lock.now_serving) != my_ticket) {
        // do nothing
    }
}

static void dynamic_barrier_polling_release_lock(dynamic_barrier_polling_t *barrier) {
    atomic_fetch_add(&barrier->lock.now_serving, 1);
}

uint32_t dynamic_barrier_polling_wait(dynamic_barrier_polling_t *barrier, uint32_t private_generation, int *stop_request, bool check_time) {
    assert(current_cpu != NULL);

    dynamic_barrier_polling_acquire_lock(barrier);

    uint32_t current_gen = __atomic_load_n(&barrier->return_value.two_32.generation, __ATOMIC_SEQ_CST);

    assert(private_generation == current_gen);

    uint64_t waiting_count = barrier->count;

    if (waiting_count == barrier->threshold - 1) {
        qstats_release_begin();
        barrier->current_cycle += quantum_size;
        bool broadcast_stop_request = 0;
        barrier->count = 0;

        /*
         * The virtual clock and virtual timers are now driven by first_cpu as
         * it consumes its quantum budget (see quantum_flush_current_stats and
         * qemu_wait_io_event), so the barrier no longer advances time or runs
         * timers here.
         */

        // Now, process the delayed interrupts.
        barrier->handling_interrupts = true;
        qemu_mutex_lock_iothread();
        // while (!g_queue_is_empty(barrier->delayed_interrupts)) {
        //     delayed_interrupt_info_t *info = g_queue_pop_head(barrier->delayed_interrupts);
        //     qemu_invoke_irq_handler(info->irq, info->level);
        //     free(info);
        // }
        qemu_mutex_unlock_iothread();
        barrier->handling_interrupts = false;

        // Then, run the periodic check.
        if (barrier->next_check_threshold != 0 && barrier->current_cycle >= barrier->next_check_threshold) {
            if (pf_periodic_check_cb != NULL) {
                if(pf_periodic_check_cb(quantum_check_threshold)) {
                    broadcast_stop_request = 1;
                    // Notify the main loop for the incoming snapshot event.
                    qemu_notify_event();

                    // wait for the machine state to become suspended for VM.
                    while (barrier->stop_request == false) {
                        sched_yield();
                    }
                }
            }
            barrier->next_check_threshold += quantum_check_threshold;
        }

        // Check if there's a pending pause request from pause_all_vcpus
        if (barrier->stop_request) {
            broadcast_stop_request = 1;
            barrier->stop_request = false;
        }

        barrier_result_t return_value;

        return_value.stop_request = broadcast_stop_request;
        return_value.generation = current_gen + 1;

        if (barrier->plugin_quantum_generation) {
            *(barrier->plugin_quantum_generation) = return_value.generation;
        }

        // cancel the sgi waking up request, because the thread is going to wake up.
        current_cpu->sgi_sender_time_ns_valid = false;

        qstats_release_end();

        // increase the generation and notify others.
        atomic_store(&barrier->return_value.one_64, *((uint64_t *)&return_value));

        dynamic_barrier_polling_release_lock(barrier); // we can release the generation here.

        if (broadcast_stop_request) {
            *stop_request = 1;
        } else {
            *stop_request = 0;
        }
    } else {
        barrier->count += 1;
        dynamic_barrier_polling_release_lock(barrier);

        barrier_result_t barrier_return_value;

        // You just need to wait.
        while (true) {
            *((uint64_t *)&barrier_return_value) = atomic_load(&barrier->return_value.one_64);

            if (barrier_return_value.generation != current_gen) {
                current_cpu->sgi_sender_time_ns_valid = false;
                break;
            }
        }

        // read the stop request set by the last thread.
        if (barrier_return_value.stop_request) {
            *stop_request = 1;
        } else {
            *stop_request = 0;
        }

    }

    return current_gen + 1;
}

uint32_t dynamic_barrier_polling_increase_by_1(dynamic_barrier_polling_t *barrier) {
    uint32_t current_generation;
    dynamic_barrier_polling_acquire_lock(barrier);
    current_generation = __atomic_load_n(&barrier->return_value.two_32.generation, __ATOMIC_SEQ_CST);
    barrier->threshold += 1;
    dynamic_barrier_polling_release_lock(barrier);

    return current_generation;
}

int dynamic_barrier_polling_decrease_by_1(dynamic_barrier_polling_t *barrier) {
    dynamic_barrier_polling_acquire_lock(barrier);
    if (barrier->threshold <= 0) {
        dynamic_barrier_polling_release_lock(barrier);
        assert(false);
    }

    barrier->threshold -= 1;
    uint64_t waiting_count = barrier->count;

    if (waiting_count == barrier->threshold && waiting_count != 0) {
        barrier->current_cycle += quantum_size;

        if (barrier->next_check_threshold != 0 && barrier->current_cycle >= barrier->next_check_threshold) {
            if (pf_periodic_check_cb != NULL) pf_periodic_check_cb(quantum_check_threshold);
            barrier->next_check_threshold += quantum_check_threshold;
        }


        barrier->count = 0;

        // increase the generation and notify others.
        __atomic_fetch_add(&barrier->return_value.two_32.generation, 1, __ATOMIC_SEQ_CST);
    }

    dynamic_barrier_polling_release_lock(barrier);
    return 0;
}

void dynamic_barrier_polling_reset(dynamic_barrier_polling_t *barrier) {
    dynamic_barrier_polling_acquire_lock(barrier);
    __atomic_store_n(&barrier->return_value.two_32.generation, 0, __ATOMIC_SEQ_CST); // this should make everyone to not wait.
    barrier->count = 0;
    dynamic_barrier_polling_release_lock(barrier);
}



void dynamic_barrier_push_delayed_interrupt(dynamic_barrier_polling_t *barrier, qemu_irq irq, int level) {
    assert(qemu_mutex_iothread_locked());

    if (barrier->handling_interrupts == true && current_cpu) {
        // we are already handling interrupts, so we can directly invoke the interrupt handler.
        qemu_invoke_irq_handler(irq, level);
        return;
    }

    // gain lock for the global I/O thread.
    delayed_interrupt_info_t *info = malloc(sizeof(delayed_interrupt_info_t));
    info->irq = irq;
    info->level = level;
    g_queue_push_tail(barrier->delayed_interrupts, info);
}

void dynamic_barrier_broadcast_pause_all_cpu_requests(dynamic_barrier_polling_t *barrier) {
    barrier->stop_request = true;
}
