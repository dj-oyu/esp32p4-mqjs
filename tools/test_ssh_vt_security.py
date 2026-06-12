#!/usr/bin/env python3
"""Adversarial PC tests for examples/ssh_vt.js.

The test builds temporary SELFTEST variants of ssh_vt.js and feeds terminal
escape sequences that an untrusted SSH server can send. A timeout, exception,
or non-zero exit means the server was able to disrupt the terminal app.

Usage:
    python3 tools/test_ssh_vt_security.py [/path/to/run_pc]
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "examples" / "ssh_vt.js"
RUN_PC = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("RUN_PC", "/tmp/run_pc"))

SELFTEST_FLAG = "var SELFTEST = false;"
INJECT_POINT = "    var lines = gridLines(t);"

CASES = [
    (
        "huge-insert-lines",
        '    t.feed("\\x1b[100000000L");\n',
        "unbounded CSI L count",
    ),
    (
        "huge-delete-chars",
        '    t.feed("\\x1b[100000000P");\n',
        "attacker-sized String.repeat in CSI P",
    ),
    (
        "oversized-csi",
        '    var atk = "1".repeat(1024);\n'
        '    t.feed("\\x1b[" + atk);\n'
        '    for (var ai = 0; ai < 200; ai++) t.feed(atk);\n',
        "unbounded incomplete CSI buffer",
    ),
]


def make_case(source: str, injection: str) -> str:
    if SELFTEST_FLAG not in source or INJECT_POINT not in source:
        raise SystemExit("ssh_vt.js self-test markers changed")
    source = source.replace(SELFTEST_FLAG, "var SELFTEST = true;", 1)
    return source.replace(INJECT_POINT, injection + INJECT_POINT, 1)


def main() -> int:
    if not RUN_PC.is_file():
        raise SystemExit(f"run_pc not found: {RUN_PC}")

    source = SOURCE.read_text(encoding="utf-8")
    vulnerable = 0
    with tempfile.TemporaryDirectory(prefix="ssh-vt-security-") as tmp:
        baseline = Path(tmp) / "baseline.js"
        baseline.write_text(
            source.replace(SELFTEST_FLAG, "var SELFTEST = true;", 1),
            encoding="utf-8",
        )
        try:
            result = subprocess.run(
                [str(RUN_PC), str(baseline)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=2,
            )
        except subprocess.TimeoutExpired:
            print("HARNESS FAIL: baseline SELFTEST timed out")
            return 2
        if result.returncode != 0 or "END cursor=" not in result.stdout:
            print("HARNESS FAIL: baseline SELFTEST did not complete")
            print(result.stdout.strip())
            return 2
        print("PASS baseline-selftest")

        for name, injection, reason in CASES:
            path = Path(tmp) / f"{name}.js"
            path.write_text(make_case(source, injection), encoding="utf-8")
            try:
                result = subprocess.run(
                    [str(RUN_PC), str(path)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    timeout=2,
                )
            except subprocess.TimeoutExpired:
                print(f"VULNERABLE {name}: timed out ({reason})")
                vulnerable += 1
                continue

            output = result.stdout
            bad_output = any(
                marker.lower() in output.lower()
                for marker in ("exception", "out of memory", "rangeerror", "typeerror")
            )
            if result.returncode != 0 or bad_output:
                first = output.strip().splitlines()[:2]
                detail = " | ".join(first) if first else f"exit={result.returncode}"
                print(f"VULNERABLE {name}: {detail} ({reason})")
                vulnerable += 1
            else:
                print(f"PASS {name}")

    if vulnerable:
        print(f"{vulnerable}/{len(CASES)} adversarial cases disrupted ssh_vt.js")
        return 1
    print(f"PASS: all {len(CASES)} adversarial cases were bounded")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
