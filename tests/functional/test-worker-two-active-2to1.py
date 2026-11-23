#!/usr/bin/env python3
"""Two active CPUs via PSCI CPU_ON; no assembler or external guest required.

Source contracts: virt.c selects HVC with virtualization=off; boot.c leaves
that conduit enabled at EL1 and powers off secondary CPUs. tcg/psci.c reads
x0..x3 and arm-powerctl.c queues reset/entry setup on the target CPU. Affinity
1 identifies the second CPU for this two-CPU virt topology.
"""
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import struct
import threading


BASE = 0x40200000
SECONDARY = BASE + 0x1000
COUNTERS = BASE + 0x2000
CONTEXT = 0x1234


def mov32(register, value):
    # MOVZ Xd,#lo16; MOVK Xd,#hi16,LSL #16.
    return [0xD2800000 | ((value & 0xFFFF) << 5) | register,
            0xF2A00000 | ((value >> 16) << 5) | register]


def store(register, base, offset=0):
    assert offset % 8 == 0 and 0 <= offset // 8 < 4096
    return 0xF9000000 | ((offset // 8) << 10) | (base << 5) | register


def workload():
    # Primary: PSCI CPU_ON64(x1=affinity 1,x2=entry,x3=context) via HVC #0.
    primary = (mov32(0, 0xC4000003) + mov32(1, 1) +
               mov32(2, SECONDARY) + mov32(3, CONTEXT) + [0xD4000002] +
               mov32(10, COUNTERS) + [store(0, 10, 16)])
    # On PSCI failure park here. Test requires return status zero AND CPU1 work.
    # CBNZ X0, . : a nonzero PSCI status cannot fall through into the loop.
    primary += [0xB5000000] + mov32(9, 0)
    # add x9,x9,#1; str x9,[x10]; b .-8
    primary += [0x91000529, store(9, 10), 0x17FFFFFE]
    # Secondary records PSCI context, then uses a different increment pattern.
    secondary = mov32(10, COUNTERS + 8) + [store(0, 10, 16)] + mov32(9, 0)
    secondary += [0x91000D29, store(9, 10), 0x17FFFFFE]  # add x9,x9,#3
    blob = bytearray(0x1000 + 4 * len(secondary))
    struct.pack_into('<' + 'I' * len(primary), blob, 0, *primary)
    struct.pack_into('<' + 'I' * len(secondary), blob, 0x1000, *secondary)
    return bytes(blob)


def test(qemu):
    with tempfile.TemporaryDirectory(prefix="qw-two-active-") as directory:
        root = pathlib.Path(directory)
        core = root / "core.csv"
        core.write_text(
            "host_core_idx,model_type,ipns,bx_private_icache_miss_coeff,"
            "bx_private_dcache_miss_coeff,bx_shared_cache_miss_coeff,"
            "bx_bp_miss_coeff,bx_drain_store_buffer_coeff,bx_instruction_coeff\n"
            "0,constant,1,0,0,0,0,0,0\n"
            "1,constant,1,0,0,0,0,0,0\n")
        guest = root / "two-active.bin"
        guest.write_bytes(workload())
        address = str(root / "qmp.sock")
        command = [str(pathlib.Path(qemu).resolve()), "-machine", "virt,secure=off,virtualization=off",
                   "-cpu", "cortex-a57", "-accel", "tcg,thread=multi",
                   "-smp", "2,maxcpus=2", "-m", "128M", "-S",
                   "-nodefaults", "-display", "none", "-serial", "none",
                   "-monitor", "none",
                   "-device", f"loader,file={guest},addr={BASE:#x},force-raw=on,cpu-num=0",
                   "-quantum",
                   f"size=10000,ipns_file={core},workers=1",
                   "-qmp", f"unix:{address},server=on,wait=off"]
        with (root / "stderr").open("w+") as log:
            process = subprocess.Popen(command, stderr=log)
            # Hard process deadline even if a stream of events defeats socket timeouts.
            watchdog = threading.Timer(35, lambda: process.kill() if process.poll() is None else None)
            watchdog.start()
            connection = socket.socket(socket.AF_UNIX)
            connection.settimeout(5)
            try:
                deadline = time.monotonic() + 15
                while True:
                    try:
                        connection.connect(address)
                        break
                    except (FileNotFoundError, ConnectionRefusedError):
                        if process.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError("QEMU startup failed or timed out")
                        time.sleep(0.05)
                stream = connection.makefile("rwb")
                assert "QMP" in json.loads(stream.readline())

                def execute(name, arguments=None):
                    request = {"execute": name}
                    if arguments is not None:
                        request["arguments"] = arguments
                    stream.write(json.dumps(request).encode() + b"\n")
                    stream.flush()
                    while True:
                        response = json.loads(stream.readline())
                        if "event" not in response:
                            return response

                assert "return" in execute("qmp_capabilities")
                cpus = execute("query-cpus-fast")["return"]
                assert sorted(cpu["cpu-index"] for cpu in cpus) == [0, 1]
                tids = {cpu["thread-id"] for cpu in cpus}
                assert len(tids) == 1
                tid = tids.pop()
                assert tid != process.pid
                assert pathlib.Path(f"/proc/{process.pid}/task/{tid}").exists()
                assert not execute("query-status")["return"]["running"]
                assert "return" in execute("cont")
                deadline = time.monotonic() + 10
                while True:
                    log.flush()
                    text = (root / "stderr").read_text()
                    if "N generation=8 boundary=8" in text:
                        break
                    assert process.poll() is None
                    assert time.monotonic() < deadline, text
                    time.sleep(0.05)
                assert "return" in execute("stop")
                assert not execute("query-status")["return"]["running"]
                assert len(execute("query-cpus-fast")["return"]) == 2
                import re
                records = re.findall(r"N generation=(\d+) boundary=(\d+) insns0=(\d+) insns1=(\d+) idle=(\d),(\d)", text)
                assert [int(row[0]) for row in records] == list(range(1, 9))
                assert all(row[0] == row[1] for row in records)
                assert all(int(row[2]) > 0 and int(row[3]) > 0 for row in records), records
                assert all(row[4:] == ("0", "0") for row in records), records
                # Guest-generated RAM counters are independent of host diagnostics.
                dump = root / "progress.bin"
                assert "return" in execute("pmemsave", {
                    "val": COUNTERS, "size": 32, "filename": str(dump)})
                count0, count1, status, context = struct.unpack("<4Q", dump.read_bytes())
                assert status == 0, f"PSCI CPU_ON failed: {status:#x}"
                assert context == CONTEXT, f"bad secondary context: {context:#x}"
                assert count0 > 0 and count1 > 0 and count1 % 3 == 0
                # Same execution identity remains after active execution and stop.
                after = execute("query-cpus-fast")["return"]
                assert {cpu["thread-id"] for cpu in after} == {tid}
                assert sorted(cpu["cpu-index"] for cpu in after) == [0, 1]
                assert "return" in execute("quit")
                assert process.wait(timeout=10) == 0
                log.seek(0)
                assert "paused-ready: CPUs 0,1" in log.read()
            finally:
                watchdog.cancel()
                connection.close()
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                log.seek(0)
                print(log.read(), file=sys.stderr)


if __name__ == "__main__":
    test(sys.argv[1])
