#!/usr/bin/env python3
"""Bounded real-QEMU rejection tests for missing/invalid worker timing rows."""
import argparse
from pathlib import Path
import subprocess
import tempfile


HEADER = ("host_core_idx,model_type,ipns,bx_private_icache_miss_coeff,"
          "bx_private_dcache_miss_coeff,bx_shared_cache_miss_coeff,"
          "bx_bp_miss_coeff,bx_drain_store_buffer_coeff,bx_instruction_coeff\n")
VALID = "0,constant,1,0,0,0,0,0,0\n"


def test(qemu, timeout):
    cases = [
        ("missing-file", None, "worker CPU 0"),
        ("missing-second-row", HEADER + VALID, "worker CPU 1"),
        ("no-rows", HEADER, "worker CPU 0"),
        ("zero-ipns", HEADER + "0,constant,0\n", "positive ipns"),
        ("negative-ipns", HEADER + "0,constant,-1\n", "non-negative"),
        ("zero-after-conversion", HEADER + "0,constant,0.001\n", "worker CPU 0"),
        ("overflow-after-conversion", HEADER + "0,constant,1e300\n", "worker CPU 0"),
        ("uint64-boundary", HEADER + "0,constant,184467440737095516.16\n", "worker CPU 0"),
    ]
    for ipns in ("nan", "inf", "1junk", "junk", "   "):
        cases.append((f"bad-ipns-{ipns.strip() or 'blank'}",
                      HEADER + f"0,constant,{ipns}\n", "invalid ipns"))
    for coefficient in ("nan", "inf", "1e300", "1junk", "   "):
        cases.append((f"bad-coefficient-{coefficient.strip() or 'blank'}",
                      HEADER + f"0,ipc-model,1,{coefficient},0,0,0,0,1\n",
                      "invalid bx_private_icache_miss_coeff"))
    with tempfile.TemporaryDirectory(prefix="nm-invalid-timing-") as directory:
        root = Path(directory)
        for name, content, diagnostic in cases:
            core = root / f"{name}.csv"
            if content is not None:
                core.write_text(content)
            command = [str(qemu), "-machine", "virt", "-cpu", "cortex-a57",
                       "-accel", "tcg,thread=multi", "-smp", "2,maxcpus=2",
                       "-m", "128M", "-S", "-nodefaults", "-display", "none",
                       "-serial", "none", "-monitor", "none", "-quantum",
                       f"size=10000,workers=1,ipns_file={core}"]
            result = subprocess.run(command, cwd=root, capture_output=True,
                                    text=True, timeout=timeout)
            assert result.returncode == 1, (name, result.returncode, result.stderr)
            assert diagnostic in result.stderr, (name, result.stderr)
            assert "paused-ready" not in result.stderr, (name, result.stderr)
            print(f"PASS {name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu", type=Path)
    parser.add_argument("--timeout", type=float, default=10)
    args = parser.parse_args()
    test(args.qemu.resolve(strict=True), args.timeout)
