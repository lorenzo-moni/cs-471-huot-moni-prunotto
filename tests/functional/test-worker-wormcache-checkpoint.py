#!/usr/bin/env python3
"""Capture two real checkpoints, then load the first and capture one more.

Input is a JSON manifest with capture/reload objects: cwd, argv, snapshots.
Use private writable disk copies and existing real checkpoint/plugin assets.
argv must include -S, -loadvm, -quantum workers=4 and a warm-mode plugin.
Capture needs count=2; reload needs count=1. No synthetic guest is generated.
"""
import argparse
import csv
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import re


def option(command, name):
    return command[command.index(name) + 1]


def run_stage(stage, log_path, timeout, count):
    command = stage["argv"]
    cwd = Path(stage["cwd"]).resolve(strict=True)
    assert Path(command[0]).is_absolute()
    assert "-S" in command and "-qmp" not in command
    assert "workers=4" in option(command, "-quantum").split(",")
    assert "size=10000" in option(command, "-quantum").split(",")
    stats = [value[6:] for value in option(command, "-quantum").split(",")
             if value.startswith("stats=")]
    assert len(stats) == 1, "acceptance requires qstats for real instruction evidence"
    stats_dir = cwd / stats[0]
    assert not stats_dir.exists(), "use a fresh qstats directory"
    assert option(command, "-smp").split(",")[0] in ("8", "cpus=8")
    plugin = option(command, "-plugin").split(",")
    assert "mode=warm" in plugin and f"count={count}" in plugin
    names = stage["snapshots"]
    assert len(names) == count
    for name in names:
        assert not (cwd / (name + ".uarch")).exists(), "use fresh output names"
    with tempfile.TemporaryDirectory(prefix="nm-ckpt-") as tmp:
        address = str(Path(tmp) / "qmp.sock")
        with log_path.open("w") as log:
            process = subprocess.Popen(command + ["-qmp", f"unix:{address},server=on,wait=off"],
                                       cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
            connection = socket.socket(socket.AF_UNIX)
            connection.settimeout(timeout)
            deadline = time.monotonic() + timeout
            try:
                while True:
                    try:
                        connection.connect(address)
                        break
                    except (FileNotFoundError, ConnectionRefusedError):
                        assert process.poll() is None, f"startup failed: {log_path}"
                        assert time.monotonic() < deadline
                        time.sleep(0.1)
                stream = connection.makefile("rwb")
                assert "QMP" in json.loads(stream.readline())

                def execute(name):
                    stream.write(json.dumps({"execute": name}).encode() + b"\n")
                    stream.flush()
                    while True:
                        assert time.monotonic() < deadline
                        connection.settimeout(max(0.1, deadline - time.monotonic()))
                        response = json.loads(stream.readline())
                        if "event" not in response:
                            return response["return"]

                execute("qmp_capabilities")
                # QMP dispatch starts after CLI -loadvm has completed.
                assert not execute("query-status")["running"]
                cpus = execute("query-cpus-fast")
                assignment = {cpu["cpu-index"]: cpu["thread-id"] for cpu in cpus}
                assert set(assignment) == set(range(8))
                assert len(set(assignment.values())) == 4
                for cpu in range(0, 8, 2):
                    assert assignment[cpu] == assignment[cpu + 1]
                    assert Path(f"/proc/{process.pid}/task/{assignment[cpu]}").exists()
                execute("cont")
                assert process.wait(timeout=max(0.1, deadline - time.monotonic())) == 0
            finally:
                connection.close()
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
    text = log_path.read_text(errors="replace")
    assert f"Generate {count} snapshots. Quit." in text, log_path
    assert "unsupported" not in text.lower(), log_path
    completions = re.findall(r"N checkpoint complete generation=(\d+) name=(\S+)", text)
    assert [name for _, name in completions] == names
    for name in names:
        assert f"Snapshot request: {name}," in text
        assert (cwd / (name + ".uarch")).is_dir()
        assert (cwd / (name + ".uarch") / "timestamp").stat().st_size > 0
        assert (cwd / (name + ".state.zstd")).stat().st_size > 0
        assert any((cwd / (name + suffix)).exists() for suffix in
                   (".bxdb", ".bxdb-meta", ".rawmem", ".raw-meta"))
    totals = [0] * 8
    resumed = [0] * 8
    seen = set()
    assert len(list(stats_dir.glob("host_*.csv"))) == 4
    assert len(list(stats_dir.glob("vcpu_*.csv"))) == 4
    for worker in range(4):
        with (stats_dir / f"vcpu_{worker}.csv").open(newline="") as stream:
            for row in csv.DictReader(stream):
                cpu, generation, insns = (int(row[key]) for key in
                                          ("cpu_idx", "generation", "insns"))
                assert cpu in (worker * 2, worker * 2 + 1)
                assert (generation, cpu) not in seen
                seen.add((generation, cpu))
                totals[cpu] += insns
                if generation > int(completions[0][0]):
                    resumed[cpu] += insns
    assert all(totals), f"no instruction progress on some CPUs: {totals}"
    if count == 2:
        assert all(resumed), f"no post-checkpoint instruction progress: {resumed}"
    return cwd


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--logs", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    config = json.loads(args.manifest.read_text())
    assert option(config["capture"]["argv"], "-loadvm") == "init_warmed"
    assert option(config["reload"]["argv"], "-loadvm") == config["capture"]["snapshots"][0]
    assert Path(config["capture"]["cwd"]).resolve() == Path(config["reload"]["cwd"]).resolve()
    assert Path(config["capture"]["argv"][0]).resolve() == Path(config["reload"]["argv"][0]).resolve()
    args.logs.mkdir(parents=True, exist_ok=False)
    run_stage(config["capture"], args.logs / "capture.log", args.timeout, 2)
    run_stage(config["reload"], args.logs / "reload.log", args.timeout, 1)
    print("Real checkpoint capture and fresh-process reload completed")
