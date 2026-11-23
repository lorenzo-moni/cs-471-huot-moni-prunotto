Experimental worker checkpoint path
==================================

The worker release hook calls pf_periodic_check_cb once at the configured
check_period threshold, under BQL and outside the barrier mutex. Every member
has completed its obligation and advertises waiting_for_quantum before arrival.
The callback queues WormCache's existing main-loop snapshot request. If it
requests a save, the last arriver keeps release pending and services local CPU
work; other arrivals service their local work through the barrier progress hook.
No worker can dispatch the next generation until the main-thread save returns.

The existing save_snapshot stops the VM, then verifies all members are stopped,
outside execution, waiting at the boundary, and on the same generation. It
retains the existing global_state_store ordering, pf_savevm_cb and VM resume.
Only successful return acknowledges the request. Failure is fatal, preventing
WormCache from counting a failed save. A missing completion fails after 600 s;
arbitrary blocking callbacks are still outside this bound and require the outer
process timeout. Snapshot I/O time is not estimated from guest time.

Worker generation has stable dedicated storage for the existing pointer API.
Both pointer APIs select that storage only in worker mode. The 32-bit epoch
and registered 64-bit plugin variable are published after capture and before
barrier release. Legacy scheduler storage/publication is unchanged. The plugin
API's 32-bit limit is enforced with explicit failure before wraparound.

CLI -loadvm is permitted with -S. It loads architecture/device/plugin state
after CPU realization and membership sealing, with all workers servicing control
work and none executing guests. Scheduler epochs/budgets are process-local,
not serialized in existing ARM/CPU VM state: fresh processes start epoch zero,
as the legacy barrier does. Runtime load after dispatch has started is explicitly
rejected. Direct manual saves outside a periodic worker boundary are rejected.
There is no worker destruction, membership reset, or in-process scheduler rewind.

Real Linux acceptance
---------------------

Use only the private checkout and writable disk/checkpoint copies. Do not use
the shared repository. Preserve the established Web Search eight-CPU machine,
memory, firmware, NIC, timing CSV (all eight rows) and checkpoint chain. The
test requires a real init_warmed starting state; it creates no synthetic guest.

Prepare a JSON manifest with capture and reload objects, each containing:

* cwd: the same private experiment run directory.
* argv: the exact command as a JSON array, with an absolute private QEMU binary,
  -S, -smp 8, -quantum size=10000,check_period=530000,workers=4 and the
  established -bios/-drive/-cpu/-machine/-m/network/plugin options.
* snapshots: exact output names, without extensions, which must not exist.

Capture argv uses -loadvm init_warmed and the warm plugin with count=2,
prefix=nm_capture,init_index=0. snapshots is ["nm_capture_0","nm_capture_1"].
Reload argv uses -loadvm nm_capture_0, count=1,prefix=nm_reload,init_index=0.
Its snapshots is ["nm_reload_0"]. For a short experiment, explicitly choose
init_threshold=1060000,interval=1060000 on both invocations. Retain the real
starting checkpoint's timing model and quantum; these plugin thresholds only
limit the acceptance run. Do not provide a QMP option: the test supplies one.
Use separate fresh stats directories on the two commands; the acceptance test
requires qstats to prove instruction progress, including after the first save.

After applying nm-worker-wormcache-checkpoint.patch in the private fw-qemu::

  ninja -C build-nm -j2
  timeout 180s build-nm/pyvenv/bin/meson test -C build-nm --print-errorlogs test-quantum-worker-map test-quantum-worker-barrier test-cpu-context
  timeout 1300s python3 tests/functional/test-worker-wormcache-checkpoint.py /PRIVATE/manifest.json --logs /PRIVATE/fresh-checkpoint-logs --timeout 600

Use the configured Meson executable if this build uses a different path. The
test verifies paired Linux TIDs, resumes, requires two successful saves/exit,
then launches a fresh process loading the first save and requires another save
and exit. It also checks positive instruction counts for all eight CPUs, including
the interval between the first and second captures. Artifact checks supplement, rather than substitute for, that reload.
Inspect logs for actual workload progress and run the private QFlex qstats
validator/analyzer on both runs. Also rerun existing 2:1/4:2/8:4 tests and the
legacy 8:8 regression with fresh outputs.

No actual fixture or configured Linux runtime exists in the Windows checkout.
This source and test have not been compiled or run against WormCache. Atomic
fallback and cross-CPU synchronous work inside exclusive callbacks still fail
explicitly; real workload triggering either must be diagnosed, not bypassed.
This implementation is not evidence of checkpoint compatibility or readiness
until the real capture/reload test passes. No WormCache sources were modified.
