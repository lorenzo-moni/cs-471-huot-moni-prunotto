Quantum worker groundwork (Phases 2b.2--2b.4)
===========================================

This is preparation only. The legacy quantum backend remains selected and
registers one barrier participant per vCPU. No N:M option, execution worker,
checkpoint adapter or CPU-work adapter for the new barrier is implemented.

Per-vCPU initialization
----------------------

``quantum_vcpu_init_time_state()`` and ``quantum_vcpu_init_budget()`` in
``accel/tcg/tcg-accel-ops-quantum.c`` contain the original assignments. Their
legacy call sites retain the original order. The budget helper accepts a
generation instead of registering a participant. Its caller still registers
at the original point, after the quantum-affiliation assertion. The helpers
are private and neither requires a new thread or changes current_cpu.

The model initialization still precedes RCU/TCG registration. Time and budget
initialization still follow BQL acquisition. Thread identity, can_do_io,
creation signaling, seeded guest RNG initialization, affinity, qstats host
initialization and vCPU attachment remain at their original call sites.
In particular, qstats counters are not newly reset. Creation signaling and
attachment cannot simply be moved into a host-neutral initializer: they
participate in startup handshakes or depend on the host's TLS state.

Scoped callback context
-----------------------

``cpu_context_save()`` / ``cpu_context_restore()`` select only thread-local
``current_cpu``. They do not grant execution ownership or locks. The immediate
same-owner path in ``do_run_on_cpu()`` scopes its callback. Queued work scopes
each callback, including its exclusive section and BQL reacquisition, before
restoring the previous selection. The queue mutex and BQL operations retain
their original order. Callback return must be normal; no longjmp may bypass
restoration. Nested scopes are supported. There is no sibling guest execution.

Known paths audited include the synchronous no-op in
``softmmu/physmem.c:tcg_log_global_after_sync``, asynchronous memory dispatch
updates in ``tcg_commit_cpu``, ARM power/reset work in ``arm-powerctl.c``,
plugin mask updates in ``plugins/core.c``, TLB work in ``accel/tcg/cputlb.c``
and exclusive-safe TB flushing in ``accel/tcg/tb-maint.c``.

Thread identity still describes ownership, not the selected vCPU. This scope
does not make a callback safe if it attempts nested guest execution or enters
an exclusive section while its owning thread still has a running sibling.
Such a call path must be deferred or otherwise resolved before live N:M use.

Independent barrier prototype
-----------------------------

``quantum-worker-barrier.c`` has no dependency on the live dynamic barrier,
CPUState, virtual clocks, qstats or snapshots. Registration explicitly seals
exactly M distinct IDs. Each worker submits its expected generation and its
own OR-reduced timer-touch flag. The barrier ORs those flags without writing
any vCPU fields. Membership never changes for WFI or pauses.

All protocol state is protected by one QemuMutex. Arrival validation rejects
invalid IDs, stale generations, concurrent/repeated arrivals and arrivals
after a released stop that has not been acknowledged. The last arrival
reserves release, drops the lock, runs the release hook, then reacquires the
lock and publishes generation plus stop result together. Other workers wait
on the condition and recheck predicates. A departure fence prevents reuse of
the published result before every consumer has copied it. Mutex ordering
publishes writes preceding every arrival to workers returning from the wait.

Stop requests receive monotonically increasing epochs under that same mutex.
A request ordered before publication is included in that result. One ordered
after publication remains pending for the next release. An older stop may be
acknowledged only after every worker has copied its release result and left
the internal wait state; this never clears a newer request. Acknowledgment
models resumption eligibility, not VM resume:
the future adapter must additionally establish all-vCPU stopped state and
complete group advancement. The prototype does not claim checkpoint safety.

Release and progress hooks run outside the internal lock. Progress is polled
with bounded condition-wait intervals; it may perform only independently
proven, bounded control work. It may not wait recursively, acknowledge this
barrier, longjmp, or rely on the barrier to release caller-owned locks. A
caller must enter without BQL, CPU work/list locks or guest execution RCU /
exclusive sections held. All hooks must return. These preconditions are not
a proof that arbitrary QEMU callbacks satisfy them.

Unresolved live integration
---------------------------

``do_run_on_cpu`` can synchronously wait on another worker. If that worker is
processing exclusive work which waits for the caller's CPU to leave guest
execution, a cycle remains even when the barrier polls CPU queues. Audit
callers of ``tcg_log_global_after_sync`` reachable from guest MMIO and queued
exclusive work ordering in ``cpus-common.c`` before introducing a real
control-work adapter. Holding the BQL while waiting for an exclusive callback
is separately forbidden by the existing queued-work contract.

The live adapter also needs worker-level registration, grouped CPU generation
commit and stop acknowledgment, legacy-compatible startup/resume boundaries,
and an explicit bridge to the plugin-visible global generation. It must
preserve first_cpu-only virtual-clock advancement; this prototype advances
neither target time nor periodic-check time. Periodic checkpoint handshakes
must not be placed in an indefinitely blocking release hook.

qstats integration is deliberately absent. A future adapter must bracket one
host wait and one last-arriver release, emit M host and N vCPU records, account
progress as barrier residency, preserve BQL attribution and avoid switch
timing overlapping barrier/BQL waits. CPU work must not create extra arrivals.
Do not expose this protocol as a live option before that adapter is validated.
WormCache waiting-state interpretation, pending atomics across checkpoints,
seeded guest RNG TLS and shared-worker teardown also remain unresolved.

Validation
----------

Run ``test-cpu-context`` and ``test-quantum-worker-barrier`` through Meson in an
isolated Linux build, followed by the existing worker-map unit test and legacy
2:2 quantum/WFI smoke test. All threaded new tests run in subprocesses with
timeouts. The barrier tests cover fixed registration, repeated generations,
one/multiple workers, payload visibility, delayed/duplicate arrivals, hooks
outside the mutex, synthetic control progress and stop publication races.
Repeated stop acknowledgment tests model protocol transitions only, not QEMU
pause/resume or usable checkpoints. Compilation and execution must be reported
separately; synthetic success would not establish real checkpoint safety.
