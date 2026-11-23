/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/cpu-context.h"
#include "qemu/thread.h"

/* Opaque CPU tokens suffice: context selection must not access CPU fields. */
struct CPUState {
    int token;
};

__thread CPUState *current_cpu;

static void test_nested(void)
{
    CPUState a = { 0 }, b = { 1 }, c = { 2 };
    CPUState *outer, *inner, *same;

    g_assert_null(current_cpu);
    outer = cpu_context_save(&current_cpu, &a);
    g_assert_null(outer);
    inner = cpu_context_save(&current_cpu, &b);
    g_assert_true(current_cpu == &b);
    same = cpu_context_save(&current_cpu, &b);
    g_assert_true(current_cpu == &b);
    cpu_context_restore(&current_cpu, same);
    same = cpu_context_save(&current_cpu, &c);
    g_assert_true(current_cpu == &c);
    cpu_context_restore(&current_cpu, same);
    g_assert_true(current_cpu == &b);
    cpu_context_restore(&current_cpu, inner);
    g_assert_true(current_cpu == &a);
    cpu_context_restore(&current_cpu, outer);
    g_assert_null(current_cpu);
}

typedef struct ContextThread {
    QemuEvent selected;
    QemuEvent finish;
} ContextThread;

static void *context_thread(void *opaque)
{
    ContextThread *s = opaque;
    CPUState cpu = { 3 };
    CPUState *saved;

    g_assert_null(current_cpu);
    saved = cpu_context_save(&current_cpu, &cpu);
    qemu_event_set(&s->selected);
    qemu_event_wait(&s->finish);
    g_assert_true(current_cpu == &cpu);
    cpu_context_restore(&current_cpu, saved);
    g_assert_null(current_cpu);
    return NULL;
}

static void test_thread_local(void)
{
    ContextThread s;
    QemuThread thread;
    CPUState cpu = { 4 };
    CPUState *saved;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        return;
    }
    qemu_event_init(&s.selected, false);
    qemu_event_init(&s.finish, false);
    saved = cpu_context_save(&current_cpu, &cpu);
    qemu_thread_create(&thread, "cpu-context", context_thread, &s,
                       QEMU_THREAD_JOINABLE);
    qemu_event_wait(&s.selected);
    g_assert_true(current_cpu == &cpu);
    qemu_event_set(&s.finish);
    qemu_thread_join(&thread);
    cpu_context_restore(&current_cpu, saved);
    g_assert_null(current_cpu);
    qemu_event_destroy(&s.selected);
    qemu_event_destroy(&s.finish);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/cpu-context/nested", test_nested);
    g_test_add_func("/cpu-context/thread-local", test_thread_local);
    return g_test_run();
}
