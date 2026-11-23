Fixed-pair multiworker execution (unvalidated)
=============================================

workers=1,2,4 select 2:1,4:2,8:4 respectively; maxcpus must equal the CPU
count. Other existing standalone AArch64 virt restrictions remain unchanged.
The immutable QuantumWorkerMap assigns contiguous pairs. Each worker is created
on its first attachment and acknowledges each CPU independently. All members
are probed and sealed under BQL at generation zero before execution is allowed.

There is one shared QuantumWorkerBarrier with M registered participants. Each
worker completes both member obligations before one arrival. Wait is outside
BQL and guest/exclusive execution. The last-arriver hook runs outside the
barrier lock; it emits a bounded release diagnostic and does not advance time.
The barrier publishes generation and fences departures before the next release.
Each member then receives exactly one refill retaining signed debt. first_cpu
alone advances virtual time using existing execution and idle helpers.

Wait-progress callbacks acquire BQL, select each local member, service common
CPU work, restore TLS and release BQL. They never dispatch guests. Synchronous
cross-CPU dispatch from an exclusive callback now fails explicitly for remote
as well as local targets: waiting for another exclusive owner can otherwise
cycle through a target's blocked control queue. Atomic fallback remains fatal.
Arbitrary callbacks that never return or have their own dependency cycles are
not made safe by barrier polling; no general callback correctness is claimed.

Stop retains local obligation flags and any barrier arrivals already submitted.
Waiting workers acknowledge CPU stop through progress callbacks. A partial
barrier stays pending while paused and resumes when unfinished members run.
If all obligations completed, the generation may commit while paused, but no
new guest dispatch occurs while runstate is stopped. Thus this adapter uses
CPU stop predicates and runstate, not the prototype's stop-epoch publication
API. It provides no checkpoint-safe all-worker snapshot or reusable teardown.

The new bounded real-QEMU test supports both configurations::

  ninja -C build-nm -j2
  timeout 45s python3 tests/functional/test-worker-execution-8to4.py build-nm/qemu-system-aarch64 4
  timeout 45s python3 tests/functional/test-worker-execution-8to4.py build-nm/qemu-system-aarch64 8

It starts secondaries with PSCI, verifies sixteen arrivals/releases per worker
and the final eight fully active generations, reads independent guest counters,
checks stop/inspection/resume progress and quits. Early startup idle generations
are allowed because other workers may arrive before all PSCI requests complete.
Existing 2:1 tests and legacy unit/regression commands remain unchanged.
No Linux compilation, runtime or sanitizer validation was run in Windows.
