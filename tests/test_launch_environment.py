#!/usr/bin/env python3
"""Probe private-runtime restoration with a native or wrapped ARM64 test binary."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

command = sys.argv[1:]
if not command:
    raise SystemExit("Usage: test_launch_environment.py [RUNNER] PROBE_BINARY")
binary = Path(command[-1]).resolve()
with tempfile.TemporaryDirectory(prefix="devstats-launch-env-") as temporary:
    payload = Path(temporary) / '普通 用户 $`"\\%'
    (payload / "libexec").mkdir(parents=True)
    (payload / "libexec/devstats").symlink_to(binary)
    wrapper = payload / "devstats"
    wrapper.write_text("#!/bin/sh\nexit 0\n")
    wrapper.chmod(0o755)
    expected = {"LD_LIBRARY_PATH": None, "QT_PLUGIN_PATH": "",
                "QT_QPA_PLATFORM_PLUGIN_PATH": 'original 中文 $`"\\%',
                "QT_QPA_PLATFORMTHEME": None, "QML2_IMPORT_PATH": "user qml"}
    environment = dict(os.environ, DEVSTATS_LAUNCHER_PATH=str(wrapper))
    for key, value in expected.items():
        environment[key] = "private-runtime"
        backup = "DEVSTATS_ORIGINAL_" + key
        environment[backup + "_SET"] = "0" if value is None else "1"
        if value is not None:
            environment[backup] = value
    def probe():
        result = subprocess.run(command, env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, timeout=20)
        assert result.returncode == 0, result.stderr[-500:]
        return json.loads(result.stdout)
    valid = probe()
    assert all(valid[key] == value for key, value in expected.items()), valid
    assert valid["launcher"] == str(wrapper) and valid["launcherStable"]
    assert not valid["backupLeaked"]
    wrapper.chmod(0o777)
    untrusted = probe()
    assert untrusted["launcher"] == str(binary), untrusted["launcher"]
    assert not untrusted["backupLeaked"]
    assert untrusted["QML2_IMPORT_PATH"] == "private-runtime"
    print("PASS launch environment: trusted ELF identity, original/unset/empty values, idempotence, unsafe-wrapper rejection, no backup leak")
