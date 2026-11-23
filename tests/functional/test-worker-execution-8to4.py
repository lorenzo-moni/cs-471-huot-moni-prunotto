#!/usr/bin/env python3
"""Four or eight active CPUs via PSCI CPU_ON; no external guest required.

Source contracts: virt.c selects HVC with virtualization=off; boot.c leaves
that conduit enabled at EL1 and powers off secondary CPUs. tcg/psci.c reads
x0..x3 and arm-powerctl.c queues reset/entry setup on the target CPU. Affinity
1 identifies the second CPU for this two-CPU virt topology.
"""
import json
import os
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
COUNTERS = BASE + 0x8000
CPUS = 8
CONTEXT = 0x1234


def mov32(register, value):
    # MOVZ Xd,#lo16; MOVK Xd,#hi16,LSL #16.
    return [0xD2800000 | ((value & 0xFFFF) << 5) | register,
            0xF2A00000 | ((value >> 16) << 5) | register]


def store(register, base, offset=0):
    assert offset % 8 == 0 and 0 <= offset // 8 < 4096
    return 0xF9000000 | ((offset // 8) << 10) | (base << 5) | register


def workload():
    primary = []
    secondary_code = {}
    for cpu in range(1, CPUS):
        entry = BASE + cpu * 0x1000
        primary += (mov32(0, 0xC4000003) + mov32(1, cpu) +
                    mov32(2, entry) + mov32(3, cpu) + [0xD4000002] +
                    mov32(10, COUNTERS + 0x100 + cpu * 8) +
                    [store(0, 10), 0xB5000000])
        secondary_code[cpu] = (mov32(10, COUNTERS + cpu * 16) +
                               [store(0, 10, 8)] + mov32(9, 0) +
                               [0x91000129 | ((cpu + 1) << 10),
                                store(9, 10), 0x17FFFFFE])
    primary += mov32(10, COUNTERS) + mov32(9, 0)
    primary += [0x91000529, store(9, 10), 0x17FFFFFE]
    assert len(primary) * 4 < 0x1000
    blob = bytearray(CPUS * 0x1000)
    struct.pack_into('<' + 'I' * len(primary), blob, 0, *primary)
    for cpu, code in secondary_code.items():
        struct.pack_into('<' + 'I' * len(code), blob, cpu * 0x1000, *code)
    return bytes(blob)


def test(qemu):
    with tempfile.TemporaryDirectory(prefix="qw-multiworker-") as directory:
        root = pathlib.Path(directory)
        core = root / "core.csv"
        core.write_text(
            "host_core_idx,model_type,ipns,bx_private_icache_miss_coeff,"
            "bx_private_dcache_miss_coeff,bx_shared_cache_miss_coeff,"
            "bx_bp_miss_coeff,bx_drain_store_buffer_coeff,bx_instruction_coeff\n" +
            "".join(f"{cpu},constant,1,0,0,0,0,0,0\n" for cpu in range(CPUS)))
        guest = root / "two-active.bin"
        guest.write_bytes(workload())
        address = str(root / "qmp.sock")
        command = [str(pathlib.Path(qemu).resolve()), "-machine", "virt,secure=off,virtualization=off",
                   "-cpu", "cortex-a57", "-accel", "tcg,thread=multi",
                   "-smp", f"{CPUS},maxcpus={CPUS}", "-m", "128M", "-S",
                   "-nodefaults", "-display", "none", "-serial", "none",
                   "-monitor", "none",
                   "-device", f"loader,file={guest},addr={BASE:#x},force-raw=on,cpu-num=0",
                   "-quantum",
                   f"size=10000,ipns_file={core},workers={CPUS // 2}",
                   "-qmp", f"unix:{address},server=on,wait=off"]
        stats = os.environ.get("QFLEX_TEST_QSTATS_DIR")
        if stats:
            stats = str(pathlib.Path(stats).resolve())
            assert not pathlib.Path(stats).exists(), "use a fresh qstats directory"
            assert "," not in stats, "qstats path cannot contain a comma"
            option = command.index("-quantum") + 1
            command[option] += f",stats={stats}"
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
                assert sorted(cpu["cpu-index"] for cpu in cpus) == list(range(CPUS))
                tids = {cpu["thread-id"] for cpu in cpus}
                assert len(tids) == CPUS // 2
                assignment = {cpu["cpu-index"]: cpu["thread-id"] for cpu in cpus}
                for cpu in range(0, CPUS, 2):
                    assert assignment[cpu] == assignment[cpu + 1]
                for tid in tids:
                    assert tid != process.pid
                    assert pathlib.Path(f"/proc/{process.pid}/task/{tid}").exists()
                assert not execute("query-status")["return"]["running"]
                assert "return" in execute("cont")
                deadline = time.monotonic() + 10
                while True:
                    log.flush()
                    text = (root / "stderr").read_text()
                    if all(f"N worker={worker} generation=16 " in text
                           for worker in range(CPUS // 2)):
                        break
                    assert process.poll() is None
                    assert time.monotonic() < deadline, text
                    time.sleep(0.05)
                assert "return" in execute("stop")
                assert not execute("query-status")["return"]["running"]
                assert len(execute("query-cpus-fast")["return"]) == CPUS
                import re
                records = re.findall(r"N worker=(\d+) generation=(\d+) cpus=(\d+),(\d+) insns=(\d+),(\d+) idle=(\d),(\d)", text)
                for worker in range(CPUS // 2):
                    rows = [row for row in records if int(row[0]) == worker]
                    assert [int(row[1]) for row in rows] == list(range(1, 17)), rows
                    for row in rows[-8:]:
                        assert tuple(map(int, row[2:4])) == (worker * 2, worker * 2 + 1)
                        assert int(row[4]) > 0 and int(row[5]) > 0, row
                        assert row[6:] == ("0", "0"), row
                arrivals = re.findall(r"N arrival worker=(\d+) generation=(\d+)", text)
                for generation in range(1, 17):
                    assert sorted(int(row[0]) for row in arrivals
                                  if int(row[1]) == generation) == list(range(CPUS // 2))
                releases = re.findall(r"N release=(\d+) arrivals=(\d+)", text)
                assert [int(row[0]) for row in releases] == list(range(1, 17))
                assert all(int(row[1]) == CPUS // 2 for row in releases)
                dump = root / "progress.bin"
                assert "return" in execute("pmemsave", {
                    "val": COUNTERS, "size": 0x140, "filename": str(dump)})
                memory = dump.read_bytes()
                previous = []
                for cpu in range(CPUS):
                    count, context = struct.unpack_from("<QQ", memory, cpu * 16)
                    assert count > 0 and count % (cpu + 1) == 0, (cpu, count)
                    previous.append(count)
                    if cpu:
                        assert context == cpu
                        assert struct.unpack_from("<Q", memory, 0x100 + cpu * 8)[0] == 0
                after = execute("query-cpus-fast")["return"]
                assert {cpu["cpu-index"]: cpu["thread-id"] for cpu in after} == assignment
                assert "return" in execute("cont")
                deadline = time.monotonic() + 5
                while True:
                    assert "return" in execute("pmemsave", {
                        "val": COUNTERS, "size": CPUS * 16, "filename": str(dump)})
                    memory = dump.read_bytes()
                    if all(struct.unpack_from("<Q", memory, cpu * 16)[0] > previous[cpu]
                           for cpu in range(CPUS)):
                        break
                    assert time.monotonic() < deadline, "resume starved a CPU"
                    time.sleep(0.05)
                assert "return" in execute("stop")
                assert not execute("query-status")["return"]["running"]
                assert "return" in execute("quit")
                assert process.wait(timeout=10) == 0
                log.seek(0)
                assert log.read().count("paused-ready: CPUs") == CPUS // 2
            finally:
                watchdog.cancel()
                connection.close()
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                log.seek(0)
                print(log.read(), file=sys.stderr)


if __name__ == "__main__":
    CPUS = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    assert CPUS in (4, 8)
    test(sys.argv[1])
