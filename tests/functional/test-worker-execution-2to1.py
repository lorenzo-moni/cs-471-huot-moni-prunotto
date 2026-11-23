#!/usr/bin/env python3
"""Linux real-QEMU smoke test; invoke with the qemu-system-aarch64 path."""
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import time


def test(qemu):
    with tempfile.TemporaryDirectory(prefix="qw-2to1-") as directory:
        root = pathlib.Path(directory)
        core = root / "core.csv"
        core.write_text(
            "host_core_idx,model_type,ipns,bx_private_icache_miss_coeff,"
            "bx_private_dcache_miss_coeff,bx_shared_cache_miss_coeff,"
            "bx_bp_miss_coeff,bx_drain_store_buffer_coeff,bx_instruction_coeff\n"
            "0,constant,1,0,0,0,0,0,0\n"
            "1,constant,1,0,0,0,0,0,0\n")
        address = str(root / "qmp.sock")
        command = [str(pathlib.Path(qemu).resolve()), "-machine", "virt",
                   "-cpu", "cortex-a57", "-accel", "tcg,thread=multi",
                   "-smp", "2,maxcpus=2", "-m", "128M", "-S",
                   "-nodefaults", "-display", "none", "-serial", "none",
                   "-monitor", "none",
                   "-device", "loader,addr=0x40000000,data=0x14000000,data-len=4,cpu-num=0",
                   "-quantum",
                   f"size=10000,ipns_file={core},workers=1",
                   "-qmp", f"unix:{address},server=on,wait=off"]
        with (root / "stderr").open("w+") as log:
            process = subprocess.Popen(command, stderr=log)
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

                def execute(name):
                    stream.write(json.dumps({"execute": name}).encode() + b"\n")
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
                assert any(int(row[2]) > 0 for row in records)
                assert any(row[5] == "1" for row in records)
                assert "return" in execute("quit")
                assert process.wait(timeout=10) == 0
                log.seek(0)
                assert "paused-ready: CPUs 0,1" in log.read()
            finally:
                connection.close()
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                log.seek(0)
                print(log.read(), file=sys.stderr)


if __name__ == "__main__":
    test(sys.argv[1])
