Experimental paused 2:1 startup
===============================

Select with -machine virt -cpu cortex-a57 -accel tcg,thread=multi
-smp 2,maxcpus=2 -S -quantum size=10000,ipns_file=core.csv,workers=1.
Only standalone AArch64 virt is accepted. Seed, replay, incoming migration,
loadvm, stats, other membership counts and resume are unsupported.

The first attachment starts the host thread; each member receives an independent
real creation acknowledgment. After machine realization, membership is sealed
and real queued callbacks verify target context and host TID. Creation and probe
waits have ten-second deadlines. No guest dispatch or quantum barrier is used.

Workers, CPU references and synchronization resources survive QEMU cleanup until
process exit. CPU hotplug/unplug and destructive unrealization are rejected;
unsupported internal rollback fails the process before member destruction.
Reusable in-process teardown is not supported. Callback payload lifetimes still
follow the existing QEMU producer contracts.

Run on Linux, under an outer process timeout::

  timeout 40s python3 tests/functional/test-worker-startup-2to1.py build-nm/qemu-system-aarch64

The test checks real CPU TIDs through QMP and /proc, startup probe completion,
resume rejection and normal process exit. It does not prove arbitrary shutdown
callback safety or checkpoint correctness. Compilation and runtime remain unrun
in the Windows development environment.
