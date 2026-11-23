/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "accel/tcg/quantum-worker-barrier.h"

#define TEST_WORKERS 4

typedef struct BarrierTest {
    QuantumWorkerBarrier barrier;
    QemuEvent progress_seen;
    QemuEvent release_entered;
    QemuEvent release_continue;
    unsigned int payload[TEST_WORKERS];
    int releases;
    int last_arrivals;
    int work_pending;
    int work_done;
    bool block_release;
    uint64_t requested_epoch;
} BarrierTest;

typedef struct WorkerTest {
    BarrierTest *test;
    unsigned int id;
    unsigned int rounds;
    uint64_t generation;
    bool visibility;
    QuantumWorkerBarrierResult result;
    QemuThread thread;
} WorkerTest;

/* Subprocess timeouts bound joins, hooks and protocol deadlocks. */
static bool bounded_case(void)
{
    if (g_test_subprocess()) {
        return true;
    }
    g_test_trap_subprocess(NULL, 10 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_passed();
    return false;
}

static void release_hook(void *opaque, uint64_t generation, bool touched)
{
    BarrierTest *s = opaque;

    /* Reenter a lock-taking API to check that the hook holds no mutex. */
    g_assert_cmpuint(quantum_worker_barrier_generation(&s->barrier), ==,
                     generation);
    qatomic_inc(&s->releases);
    if (s->block_release) {
        qemu_event_set(&s->release_entered);
        qemu_event_wait(&s->release_continue);
    }
}

static void progress_hook(void *opaque, unsigned int id, uint64_t generation)
{
    BarrierTest *s = opaque;

    /* Model permitted, bounded control work, not a real QEMU CPU work queue. */
    quantum_worker_barrier_generation(&s->barrier);
    if (qatomic_cmpxchg(&s->work_pending, 1, 0) == 1) {
        qatomic_inc(&s->work_done);
    }
    qemu_event_set(&s->progress_seen);
}

static const QuantumWorkerBarrierOps ops = {
    .release = release_hook,
    .progress = progress_hook,
};

static void setup(BarrierTest *s, unsigned int workers)
{
    unsigned int i;

    memset(s, 0, sizeof(*s));
    g_assert_true(quantum_worker_barrier_init(&s->barrier, workers,
                                            &error_abort));
    for (i = 0; i < workers; i++) {
        g_assert_true(quantum_worker_barrier_register(&s->barrier, i,
                                                    &error_abort));
    }
    g_assert_true(quantum_worker_barrier_start(&s->barrier, &error_abort));
    qemu_event_init(&s->progress_seen, false);
    qemu_event_init(&s->release_entered, false);
    qemu_event_init(&s->release_continue, false);
}

static void cleanup(BarrierTest *s)
{
    quantum_worker_barrier_destroy(&s->barrier);
    qemu_event_destroy(&s->progress_seen);
    qemu_event_destroy(&s->release_entered);
    qemu_event_destroy(&s->release_continue);
}

static void wait_once(WorkerTest *w, bool touched)
{
    g_assert_true(quantum_worker_barrier_wait(&w->test->barrier, w->id,
                                            w->generation, touched, &ops,
                                            w->test, &w->result, &error_abort));
    g_assert_cmpuint(w->result.generation, ==, w->generation + 1);
    w->generation++;
    if (w->result.last_arriver) {
        qatomic_inc(&w->test->last_arrivals);
    }
}

static void *worker_thread(void *opaque)
{
    WorkerTest *w = opaque;
    unsigned int round, i;

    for (round = 0; round < w->rounds; round++) {
        w->test->payload[w->id] = round + 1;
        wait_once(w, round % 2 == 0 &&
                  w->id == w->test->barrier.n_workers - 1);
        if (w->visibility) {
            g_assert_cmpuint(w->result.stop_epoch, ==, 0);
            g_assert_cmpint(w->result.timer_touched, ==, round % 2 == 0);
            for (i = 0; i < w->test->barrier.n_workers; i++) {
                g_assert_cmpuint(w->test->payload[i], ==, round + 1);
            }
            /* Readers finish before any worker overwrites the next payload. */
            wait_once(w, false);
            g_assert_false(w->result.timer_touched);
        }
    }
    return NULL;
}

static void launch(WorkerTest *w, BarrierTest *s, unsigned int id,
                   unsigned int rounds, bool visibility)
{
    *w = (WorkerTest) {
        .test = s,
        .id = id,
        .rounds = rounds,
        .visibility = visibility,
        .generation = quantum_worker_barrier_generation(&s->barrier),
    };
    qemu_thread_create(&w->thread, "worker-barrier", worker_thread, w,
                       QEMU_THREAD_JOINABLE);
}

static void run_round(BarrierTest *s, WorkerTest *workers)
{
    unsigned int i;

    for (i = 0; i < s->barrier.n_workers; i++) {
        launch(&workers[i], s, i, 1, false);
    }
    for (i = 0; i < s->barrier.n_workers; i++) {
        qemu_thread_join(&workers[i].thread);
    }
}

static void check_rounds(unsigned int n)
{
    BarrierTest s;
    WorkerTest workers[TEST_WORKERS];
    unsigned int i;

    if (!bounded_case()) {
        return;
    }
    setup(&s, n);
    for (i = 0; i < n; i++) {
        launch(&workers[i], &s, i, 16, true);
    }
    for (i = 0; i < n; i++) {
        qemu_thread_join(&workers[i].thread);
    }
    g_assert_cmpuint(quantum_worker_barrier_generation(&s.barrier), ==, 32);
    g_assert_cmpint(qatomic_read(&s.releases), ==, 32);
    g_assert_cmpint(qatomic_read(&s.last_arrivals), ==, 32);
    cleanup(&s);
}

static void test_one(void)
{
    check_rounds(1);
}

static void test_multiple(void)
{
    check_rounds(TEST_WORKERS);
}

static void clear_error(Error **err)
{
    g_assert_nonnull(*err);
    error_free(*err);
    *err = NULL;
}

static void test_invalid(void)
{
    QuantumWorkerBarrier b;
    QuantumWorkerBarrierResult result;
    Error *err = NULL;

    if (!bounded_case()) {
        return;
    }
    g_assert_false(quantum_worker_barrier_init(&b, 0, &err));
    clear_error(&err);
    g_assert_false(quantum_worker_barrier_init(&b, 129, &err));
    clear_error(&err);
    g_assert_true(quantum_worker_barrier_init(&b, 2, &error_abort));
    g_assert_false(quantum_worker_barrier_wait(&b, 0, 0, false, NULL, NULL,
                                             &result, &err));
    clear_error(&err);
    g_assert_false(quantum_worker_barrier_register(&b, 2, &err));
    clear_error(&err);
    g_assert_true(quantum_worker_barrier_register(&b, 0, &error_abort));
    g_assert_false(quantum_worker_barrier_register(&b, 0, &err));
    clear_error(&err);
    g_assert_false(quantum_worker_barrier_start(&b, &err));
    clear_error(&err);
    g_assert_true(quantum_worker_barrier_register(&b, 1, &error_abort));
    g_assert_true(quantum_worker_barrier_start(&b, &error_abort));
    g_assert_false(quantum_worker_barrier_start(&b, &err));
    clear_error(&err);
    g_assert_false(quantum_worker_barrier_wait(&b, 2, 0, false, NULL, NULL,
                                             &result, &err));
    clear_error(&err);
    g_assert_false(quantum_worker_barrier_wait(&b, 0, 1, false, NULL, NULL,
                                             &result, &err));
    clear_error(&err);
    quantum_worker_barrier_destroy(&b);
}

static void test_delayed_and_duplicate(void)
{
    BarrierTest s;
    WorkerTest workers[2];
    QuantumWorkerBarrierResult result;
    Error *err = NULL;

    if (!bounded_case()) {
        return;
    }
    setup(&s, 2);
    qatomic_set(&s.work_pending, 1);
    launch(&workers[0], &s, 0, 1, false);
    qemu_event_wait(&s.progress_seen);
    g_assert_cmpint(qatomic_read(&s.work_done), ==, 1);
    g_assert_cmpuint(quantum_worker_barrier_generation(&s.barrier), ==, 0);
    g_assert_cmpint(qatomic_read(&s.releases), ==, 0);
    g_assert_false(quantum_worker_barrier_wait(&s.barrier, 0, 0, false, &ops,
                                             &s, &result, &err));
    clear_error(&err);
    launch(&workers[1], &s, 1, 1, false);
    qemu_thread_join(&workers[0].thread);
    qemu_thread_join(&workers[1].thread);
    g_assert_cmpint(qatomic_read(&s.releases), ==, 1);
    g_assert_cmpint(qatomic_read(&s.last_arrivals), ==, 1);
    cleanup(&s);
}

static uint64_t request_stop(BarrierTest *s)
{
    uint64_t epoch;

    g_assert_true(quantum_worker_barrier_request_stop(&s->barrier, &epoch,
                                                     &error_abort));
    return epoch;
}

static void assert_stop(WorkerTest *workers, unsigned int n, uint64_t epoch)
{
    unsigned int i;

    for (i = 0; i < n; i++) {
        g_assert_cmpuint(workers[i].result.stop_epoch, ==, epoch);
    }
}

static void test_stop_transitions(void)
{
    BarrierTest s;
    WorkerTest workers[2];
    QuantumWorkerBarrierResult result;
    Error *err = NULL;
    uint64_t epoch, newer;
    unsigned int i;

    if (!bounded_case()) {
        return;
    }
    setup(&s, 2);
    for (i = 0; i < 8; i++) {
        epoch = request_stop(&s); /* Before arrival. */
        g_assert_false(quantum_worker_barrier_acknowledge_stop(&s.barrier,
                                                              epoch, &err));
        clear_error(&err);
        run_round(&s, workers);
        assert_stop(workers, 2, epoch);
        g_assert_false(quantum_worker_barrier_wait(&s.barrier, 0,
                         quantum_worker_barrier_generation(&s.barrier), false,
                         &ops, &s, &result, &err));
        clear_error(&err);
        newer = request_stop(&s); /* After publication, before old stop ack. */
        g_assert_true(quantum_worker_barrier_acknowledge_stop(&s.barrier,
                                                             epoch,
                                                             &error_abort));
        run_round(&s, workers);
        assert_stop(workers, 2, newer);
        g_assert_true(quantum_worker_barrier_acknowledge_stop(&s.barrier,
                                                             newer,
                                                             &error_abort));
        run_round(&s, workers);
        assert_stop(workers, 2, 0);
    }
    cleanup(&s);
}

static void test_stop_waiting(void)
{
    BarrierTest s;
    WorkerTest workers[2];
    uint64_t epoch;

    if (!bounded_case()) {
        return;
    }
    setup(&s, 2);
    launch(&workers[0], &s, 0, 1, false);
    qemu_event_wait(&s.progress_seen);
    epoch = request_stop(&s);
    launch(&workers[1], &s, 1, 1, false);
    qemu_thread_join(&workers[0].thread);
    qemu_thread_join(&workers[1].thread);
    assert_stop(workers, 2, epoch);
    cleanup(&s);
}

static void *stop_thread(void *opaque)
{
    BarrierTest *s = opaque;

    qemu_event_wait(&s->release_continue);
    s->requested_epoch = request_stop(s);
    return NULL;
}

static void test_stop_release_race(void)
{
    BarrierTest s;
    WorkerTest workers[2];
    QemuThread requester;
    uint64_t epoch;
    unsigned int i;

    if (!bounded_case()) {
        return;
    }
    for (i = 0; i < 8; i++) {
        setup(&s, 2);
        s.block_release = true;
        launch(&workers[0], &s, 0, 1, false);
        launch(&workers[1], &s, 1, 1, false);
        qemu_event_wait(&s.release_entered);
        /* A stop must also be accepted while release_hook is executing. */
        if (i == 0) {
            s.requested_epoch = request_stop(&s);
        } else {
            qemu_thread_create(&requester, "barrier-stop", stop_thread, &s,
                               QEMU_THREAD_JOINABLE);
        }
        qemu_event_set(&s.release_continue);
        qemu_thread_join(&workers[0].thread);
        qemu_thread_join(&workers[1].thread);
        if (i != 0) {
            qemu_thread_join(&requester);
        }
        epoch = s.requested_epoch;
        g_assert_cmpuint(epoch, ==, 1);
        assert_stop(workers, 2, workers[0].result.stop_epoch);
        if (!workers[0].result.stop_epoch) {
            /* Request lost the publication race: it must survive to next g. */
            s.block_release = false;
            run_round(&s, workers);
        }
        assert_stop(workers, 2, epoch);
        g_assert_true(quantum_worker_barrier_acknowledge_stop(&s.barrier, epoch,
                                                             &error_abort));
        cleanup(&s);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/worker-barrier/one", test_one);
    g_test_add_func("/worker-barrier/multiple-visibility", test_multiple);
    g_test_add_func("/worker-barrier/invalid", test_invalid);
    g_test_add_func("/worker-barrier/delayed-duplicate-progress",
                    test_delayed_and_duplicate);
    g_test_add_func("/worker-barrier/stop-transitions", test_stop_transitions);
    g_test_add_func("/worker-barrier/stop-waiting", test_stop_waiting);
    g_test_add_func("/worker-barrier/stop-release-race",
                    test_stop_release_race);
    return g_test_run();
}
