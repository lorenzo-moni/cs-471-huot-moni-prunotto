N:M warming readiness: incomplete
================================

The worker qstats hooks are now connected independently of WormCache. They
register one host per worker and attach each member, bracket TCG execution,
member context selection, barrier wait and last-arrival release, and emit
per-member rows through quantum_vcpu_advance. Existing CSV schemas, metadata,
buffer publication and exit flushing are reused. STOP/RESUMED mark interrupted
intervals for exclusion. Host affinity remains unknown (-1); this change does
not pin workers or establish an affinity policy. Statistics are unvalidated.

Bare-metal statistics validation in a private Linux checkout::

  ninja -C build-nm -j2
  QFLEX_TEST_QSTATS_DIR=/ABSOLUTE/FRESH/qstats timeout 45s python3 tests/functional/test-worker-execution-8to4.py build-nm/qemu-system-aarch64 8

From the private QFlex checkout::

  python3 analysis/qstats_validate_nm.py /ABSOLUTE/FRESH/qstats --workers 4 --cpus 8
  python3 analysis/qstats_analyze.py /ABSOLUTE/FRESH/qstats --json /ABSOLUTE/FRESH/report.json

The launcher gains --qemu-binary to resolve an explicit executable before
changing to the experiment run directory. Defaults are unchanged. It does not
yet expose workers selection for warming: enabling it would imply unsupported
snapshot startup. No warmed Web Search configuration is ready to run.

Concrete unresolved checkpoint integration
-----------------------------------------

worker_release currently contains no periodic callback. The legacy dynamic
barrier increments current_cycle, checks next_check_threshold, calls
pf_periodic_check_cb and notifies the main loop. WormCache warm.rs queues
SNAPSHOT_INFO in that callback. Its event_loop_callback calls save_snapshot
through qemu_plugin_savevm. save_snapshot calls vm_stop, captures QEMU state
and pf_savevm_cb, then vm_start. The new barrier needs a checkpoint handshake
that permits CPU control work while preventing publication/next-generation
execution throughout capture. Copying the legacy stop_request spin is invalid:
the worker stop path does not write that legacy flag.

plugins/pf_api.c still gives the plugin pointers into quantum_barrier, whereas
workers execute through worker_barrier. No publication bridge updates these
legacy generation locations. Enabling periodic callbacks alone is insufficient.
Snapshot load is still rejected by vl.c. Neither guard is removed here.

The Windows WormCache working tree has tracked files marked deleted; committed
source was inspected read-only with git show. No checkout or index restoration
was attempted. No Web Search image, baseline experiment settings or executable
Linux environment were available. Do not substitute synthetic tests for capture
and reload of the actual workload.

Required future acceptance uses an isolated copy of the established 8:8 Web
Search starting disk and checkpoint chain, preserving all eight timing rows
(including phantom/client CPUs 4-7), quantum=10000, memory, firmware and NIC
settings. Only affinity may be changed: pair rows 0/1,2/3,4/5,6/7 must name a
compatible available physical core after an actual worker-pinning policy exists.
Use a unique experiment and qstats directory, count=2, an explicit private QEMU
binary and an outer timeout. Verify capture, reload, resumed guest progress and
clean exit, then validate four host/eight guest records per complete generation
and run the unchanged legacy 8:8 baseline. None of these workload tests was run.
