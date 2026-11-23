Experimental single-worker execution
====================================

workers=1 now permits cont after the existing sealed 2:1 startup. Membership,
standalone and process-lifetime restrictions remain. The earlier startup test
now tests paused startup only; resume rejection is superseded by live execution.

Each member services work under BQL, selects current_cpu, releases BQL for
TCG execution and reacquires it before another selection. Completed obligations
are immutable until both members complete. Architectural idle is credited once;
stop-induced inactivity is not credited. The M=1 worker barrier is called once
outside BQL and execution, then each member receives exactly one budget refill.
Signed debt is carried across refills. first_cpu remains the timekeeper through
the existing execution and idle helpers. Stops retain incomplete obligations;
completed groups commit together under BQL, including a stop arriving at the
boundary. Resume continues the retained generation.

Kicks exit the addressed CPU and the atomically published active member before
waking the shared worker. Queued callbacks retain the existing scoped context
and exclusive-work machinery. Atomic fallback is explicitly unsupported and
terminates the process with a diagnostic before a sibling switch. This is an
experimental limitation, not successful atomic execution. No seeded RNG,
qstats output, checkpoint or WormCache integration is provided.

The first eight boundaries emit narrow development diagnostics with instruction
and idle counters. The real test loads a branch loop through QEMU's generic
loader, verifies instruction progress while CPU 1 is powered off, stops and quits.
It does not establish interrupt timing, arbitrary callback safety, or general
scheduler correctness. Run on isolated Linux::

  ninja -C build-nm -j2
  timeout 40s python3 tests/functional/test-worker-execution-2to1.py build-nm/qemu-system-aarch64
  timeout 40s python3 tests/functional/test-worker-startup-2to1.py build-nm/qemu-system-aarch64

Run the established legacy 2:2 regression without workers and with fresh qstats
output, plus the existing worker-map, worker-barrier and CPU-context unit tests.
Compilation and QEMU runtime tests have not been run in Windows.
