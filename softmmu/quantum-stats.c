/*
 * Per-quantum host-time breakdown (qstats). See include/sysemu/quantum-stats.h.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "sysemu/sysemu.h"
#include "hw/core/cpu.h"
#include "sysemu/quantum.h"
#include "sysemu/quantum-stats.h"

#define QSTATS_MAX_THREADS 256
#define QSTATS_BUF_RECORDS 65536
/* Room kept in the vCPU buffer for the records of one quantum (all co-located vCPUs). */
#define QSTATS_VCPU_SLACK  256

__thread QStatsHost *qstats_self;

/*
 * qs_lock protects the thread registry, qs_closed, the files and the
 * metadata fields. Buffers are written to the files only while holding it,
 * both by their owner (when full) and by the exit notifier.
 */
static GMutex qs_lock;
static QStatsHost *qs_threads[QSTATS_MAX_THREADS];
static int qs_nthreads;
static bool qs_closed;
static uint64_t qs_t_init;
static Notifier qs_exit_notifier;
static bool qs_global_ready;

static FILE *qs_open(const char *name, int tid, const char *header)
{
    char *path = g_strdup_printf("%s/%s_%d.csv", quantum_stats_dir, name, tid);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "qstats: cannot open %s: %s\n", path, strerror(errno));
        abort();
    }
    g_free(path);
    setvbuf(fp, NULL, _IOFBF, 1 << 20);
    fprintf(fp, "%s\n", header);
    return fp;
}

/* Writes the published records of @s. Called with qs_lock held and the files open. */
static void qs_write_buffers(QStatsHost *s)
{
    size_t hlen = qatomic_load_acquire(&s->hlen);
    size_t vlen = qatomic_load_acquire(&s->vlen);

    for (size_t i = 0; i < hlen; i++) {
        const QStatsHostRec *r = &s->hbuf[i];
        fprintf(s->hfp, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu32 ",%" PRIu32 "\n",
                r->generation, r->t_start_ns, r->exec_ns, r->barrier_ns,
                r->ctx_switch_ns, r->bql_wait_ns, r->exec_cpu_ns, r->barrier_cpu_ns,
                r->n_switches, r->flags);
    }

    for (size_t i = 0; i < vlen; i++) {
        const QStatsVcpuRec *r = &s->vbuf[i];
        fprintf(s->vfp, "%" PRIu64 ",%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRId64 ",%" PRIu32 "\n",
                r->generation, r->cpu_idx, r->insns, r->host_exec_ns, r->bql_wait_ns,
                r->wfi_ps, r->idle_entries);
    }
}

/*
 * Owner thread: writes and empties its buffers. After the exit notifier ran,
 * the records are dropped instead (the files are closed).
 */
static void qs_flush(QStatsHost *s)
{
    uint64_t t0 = qstats_now_ns();

    g_mutex_lock(&qs_lock);
    if (!qs_closed) {
        qs_write_buffers(s);
    }
    qatomic_store_release(&s->hlen, 0);
    qatomic_store_release(&s->vlen, 0);
    s->flush_ns += qstats_now_ns() - t0;
    s->n_flushes++;
    g_mutex_unlock(&qs_lock);

    s->pending_flags |= QSTATS_FLUSHED;
}

/* Reads scheduler statistics of a thread from /proc (Linux only). */
static void qs_read_sched(int ktid, uint64_t *run_ns, uint64_t *wait_ns,
                          uint64_t *slices, uint64_t *vol, uint64_t *nonvol)
{
    char path[128];
    FILE *fp;

    *run_ns = *wait_ns = *slices = *vol = *nonvol = 0;

    snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", ktid);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%" SCNu64 " %" SCNu64 " %" SCNu64,
                   run_ns, wait_ns, slices) != 3) {
            *run_ns = *wait_ns = *slices = 0;
        }
        fclose(fp);
    }

    snprintf(path, sizeof(path), "/proc/self/task/%d/status", ktid);
    fp = fopen(path, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "voluntary_ctxt_switches: %" SCNu64, vol) == 1) {
                continue;
            }
            sscanf(line, "nonvoluntary_ctxt_switches: %" SCNu64, nonvol);
        }
        fclose(fp);
    }
}

/* threads.csv and meta.txt. Called with qs_lock held. */
static void qs_write_metadata(uint64_t t_exit)
{
    char *path = g_strdup_printf("%s/threads.csv", quantum_stats_dir);
    FILE *fp = fopen(path, "w");
    g_free(path);
    if (fp) {
        fprintf(fp, "tid,host_core,kernel_tid,n_flushes,flush_ns,sched_run_ns,"
                    "sched_wait_ns,sched_timeslices,voluntary_ctxt_switches,"
                    "nonvoluntary_ctxt_switches\n");
        for (int i = 0; i < qs_nthreads; i++) {
            QStatsHost *s = qs_threads[i];
            uint64_t run_ns, wait_ns, slices, vol, nonvol;
            qs_read_sched(s->kernel_tid, &run_ns, &wait_ns, &slices, &vol, &nonvol);
            fprintf(fp, "%d,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                    ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                    s->tid, s->host_core, s->kernel_tid, s->n_flushes, s->flush_ns,
                    run_ns, wait_ns, slices, vol, nonvol);
        }
        fclose(fp);
    }

    path = g_strdup_printf("%s/meta.txt", quantum_stats_dir);
    fp = fopen(path, "w");
    g_free(path);
    if (fp) {
        fprintf(fp, "quantum_size_ns=%" PRIu64 "\n", quantum_size);
        fprintf(fp, "wall_ns=%" PRIu64 "\n", t_exit - qs_t_init);
        fprintf(fp, "host_threads=%d\n", qs_nthreads);
        fclose(fp);
    }
}

/*
 * Runs from atexit(). The vCPU threads may still be running: WormCache exits
 * from the main loop right after save_snapshot(), which resumes the VM.
 * Holding qs_lock and setting qs_closed guarantees that no thread writes to
 * the files concurrently or after they are closed; records published after
 * this point are dropped.
 */
static void qs_exit(Notifier *n, void *data)
{
    uint64_t t_exit = qstats_now_ns();

    g_mutex_lock(&qs_lock);
    if (qs_closed) {
        g_mutex_unlock(&qs_lock);
        return;
    }
    qs_closed = true;
    for (int i = 0; i < qs_nthreads; i++) {
        QStatsHost *s = qs_threads[i];
        qs_write_buffers(s);
        fclose(s->hfp);
        fclose(s->vfp);
        s->hfp = s->vfp = NULL;
    }
    qs_write_metadata(t_exit);
    g_mutex_unlock(&qs_lock);
}

/*
 * BQL waits of vCPU threads (installed in qstats_bql_wait_cb). Waits inside
 * the barrier (the last arriver takes the BQL there) belong to barrier_ns.
 */
static void qs_bql_wait(CPUState *cpu, uint64_t wait_ns)
{
    QStatsHost *s = qstats_self;
    if (!s || s->in_barrier) {
        return;
    }
    s->bql_acc += wait_ns;
    cpu->qstats_bql_wait_ns += wait_ns;
}

void qstats_global_init(void)
{
    if (!quantum_stats_enabled() || qs_global_ready) {
        return;
    }
    if (g_mkdir_with_parents(quantum_stats_dir, 0755) != 0) {
        fprintf(stderr, "qstats: cannot create %s: %s\n", quantum_stats_dir,
                strerror(errno));
        abort();
    }
    qs_t_init = qstats_now_ns();
    qs_exit_notifier.notify = qs_exit;
    qemu_add_exit_notifier(&qs_exit_notifier);
    qstats_bql_wait_cb = qs_bql_wait;
    qs_global_ready = true;
}

void qstats_thread_init(int host_tid, int host_core)
{
    if (!quantum_stats_enabled()) {
        return;
    }
    assert(qs_global_ready);

    QStatsHost *s = g_new0(QStatsHost, 1);
    s->tid = host_tid;
    s->host_core = host_core;
    s->kernel_tid = qemu_get_thread_id();
    s->hbuf = g_new(QStatsHostRec, QSTATS_BUF_RECORDS);
    s->vbuf = g_new(QStatsVcpuRec, QSTATS_BUF_RECORDS);
    s->hfp = qs_open("host", host_tid,
                     "generation,t_start_ns,exec_ns,barrier_ns,ctx_switch_ns,"
                     "bql_wait_ns,exec_cpu_ns,barrier_cpu_ns,n_switches,flags");
    s->vfp = qs_open("vcpu", host_tid,
                     "generation,cpu_idx,insns,host_exec_ns,bql_wait_ns,wfi_ps,"
                     "idle_entries");
    /* The thread starts with the VM stopped: time starts at the first resume. */
    s->need_restart = true;
    s->t_start = qstats_now_ns();
    s->cpu_start = qstats_thread_cpu_ns();

    g_mutex_lock(&qs_lock);
    assert(qs_nthreads < QSTATS_MAX_THREADS);
    qs_threads[qs_nthreads++] = s;
    g_mutex_unlock(&qs_lock);

    qstats_self = s;
}

void qstats_vcpu_attach(CPUState *cpu)
{
    if (qstats_self) {
        cpu->qstats_bql_on = true;
    }
}

void qstats_resume_if_needed(CPUState *cpu)
{
    QStatsHost *s = qstats_self;
    bool paused = cpu->qstats_paused;

    cpu->qstats_paused = false;
    if (!s || !(s->need_restart || paused)) {
        return;
    }
    s->need_restart = false;
    s->t_start = qstats_now_ns();
    s->cpu_start = qstats_thread_cpu_ns();
    s->ctx_acc = 0;
    s->bql_acc = 0;
    s->n_switches = 0;
    s->in_barrier = false;
    s->pending_flags |= QSTATS_RESUMED;
}

void qstats_barrier_exit(uint64_t generation, uint32_t flags)
{
    QStatsHost *s = qstats_self;
    if (!s) {
        return;
    }

    uint64_t t = qstats_now_ns();
    uint64_t cpu_t = qstats_thread_cpu_ns();
    uint64_t span = s->t_bar_enter - s->t_start;
    uint64_t other = s->ctx_acc + s->bql_acc;
    size_t n = s->hlen;

    if (n == QSTATS_BUF_RECORDS) {
        qs_flush(s);
        n = 0;
    }
    QStatsHostRec *r = &s->hbuf[n];
    r->generation = generation;
    r->t_start_ns = s->t_start - qs_t_init;
    r->exec_ns = span > other ? span - other : 0;
    r->barrier_ns = t - s->t_bar_enter;
    r->ctx_switch_ns = s->ctx_acc;
    r->bql_wait_ns = s->bql_acc;
    r->exec_cpu_ns = s->cpu_bar_enter - s->cpu_start;
    r->barrier_cpu_ns = cpu_t - s->cpu_bar_enter;
    r->n_switches = s->n_switches;
    r->flags = s->pending_flags | flags;
    /* Publish only the complete record. */
    qatomic_store_release(&s->hlen, n + 1);

    s->in_barrier = false;
    s->ctx_acc = 0;
    s->bql_acc = 0;
    s->n_switches = 0;
    s->pending_flags = 0;

    if (flags & QSTATS_STOP) {
        /* The thread is about to be stopped: restart timing on resume. */
        s->need_restart = true;
    }

    if (n + 1 == QSTATS_BUF_RECORDS ||
        s->vlen + QSTATS_VCPU_SLACK > QSTATS_BUF_RECORDS) {
        qs_flush(s);
        /* The flush is not accounted to the next quantum (it is flagged FLUSHED). */
        t = qstats_now_ns();
        cpu_t = qstats_thread_cpu_ns();
    }

    s->t_start = t;
    s->cpu_start = cpu_t;
}

void qstats_exec_end(CPUState *cpu)
{
    if (qstats_self) {
        cpu->qstats_host_exec_ns += qstats_now_ns() - qstats_self->t_exec_start;
    }
}

void qstats_vcpu_end_quantum(CPUState *cpu, uint64_t generation)
{
    QStatsHost *s = qstats_self;

    if (s) {
        size_t n = s->vlen;
        if (n == QSTATS_BUF_RECORDS) {
            qs_flush(s);
            n = 0;
        }
        QStatsVcpuRec *r = &s->vbuf[n];
        r->generation = generation;
        r->cpu_idx = cpu->cpu_index;
        r->insns = cpu->qstats_insns;
        r->host_exec_ns = cpu->qstats_host_exec_ns;
        r->bql_wait_ns = cpu->qstats_bql_wait_ns;
        r->wfi_ps = cpu->qstats_wfi_ps;
        r->idle_entries = cpu->qstats_idle_entries;
        qatomic_store_release(&s->vlen, n + 1);
    }

    cpu->qstats_insns = 0;
    cpu->qstats_host_exec_ns = 0;
    cpu->qstats_bql_wait_ns = 0;
    cpu->qstats_wfi_ps = 0;
    cpu->qstats_idle_entries = 0;
}
