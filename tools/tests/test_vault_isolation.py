#!/usr/bin/env python3
"""Verify that mqjs vault entries are isolated by immutable app identity."""

import os
import subprocess
import sys
import tempfile
from pathlib import Path


RUN_PC = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("RUN_PC", "/tmp/run_pc"))


def main() -> int:
    if not RUN_PC.is_file():
        raise SystemExit(f"run_pc not found: {RUN_PC}")
    with tempfile.TemporaryDirectory(prefix="mqjs-vault-") as tmp:
        tmp = Path(tmp)
        owner = tmp / "vault_owner.js"
        attacker = tmp / "vault_attacker.js"
        owner.write_text(
            'vault.put("login", "secret");\n'
            'sys.setAppName("renamed-owner");\n'
            'print("OWNER", vault.has("login"));\n',
            encoding="utf-8",
        )
        attacker.write_text(
            'sys.setAppName("vault_owner");\n'
            'print("ATTACKER", vault.has("login"));\n',
            encoding="utf-8",
        )
        result = subprocess.run(
            [str(RUN_PC), str(attacker), str(owner)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=3,
        )
    output = result.stdout
    if result.returncode != 0 or "OWNER true" not in output or "ATTACKER false" not in output:
        print(output.strip())
        print("FAIL: vault app isolation")
        return 1
    print("PASS: renamed owner retained access")
    print("PASS: app-name impersonation could not access owner vault")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
