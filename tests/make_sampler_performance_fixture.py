#!/usr/bin/env python3
"""Create a bounded, ordinary-user proc fixture outside benchmark timing."""
import os
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
root.mkdir(parents=True, exist_ok=False)
proc = root / "proc"
project = root / "project"
project.mkdir()
(project / "package.json").write_text("{}")
(project / "compiler").write_text("fixture")
(root / "sys/class/power_supply").mkdir(parents=True)
proc.mkdir()
(proc / "stat").write_text("cpu 100 0 50 850 0 0 0 0\ncpu0 100 0 50 850 0 0 0 0\n")
(proc / "meminfo").write_text("MemTotal: 16000000 kB\nMemAvailable: 12000000 kB\n")
(proc / "loadavg").write_text("0.1 0.2 0.3 1/1600 2600\n")
(proc / "uptime").write_text("1000 900\n")
command = b"compiler\0" + b"x" * 8192 + b"\0" + os.fsencode(project) + b"\0"
for offset in range(1600):
    pid = 1000 + offset
    directory = proc / str(pid)
    directory.mkdir()
    fields = ["0"] * 22
    fields[0], fields[1], fields[9], fields[11], fields[19], fields[21] = "S", "1", "4", "100", str(10000 + offset), str(100 + offset)
    (directory / "stat").write_text(f"{pid} (compiler) " + " ".join(fields) + "\n")
    (directory / "cmdline").write_bytes(command)
    (directory / "io").write_text("read_bytes: 4096\nwrite_bytes: 8192\n")
    (directory / "cwd").symlink_to(project)
    (directory / "exe").symlink_to(project / "compiler")
print(f"Created {1600} processes with {len(command)} argv bytes each at {root}")
