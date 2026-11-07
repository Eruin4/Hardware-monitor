"""Run launchers with fake desktop/process commands; no host processes are touched."""
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Launchers(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="launcher-test-", dir=ROOT)
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.project = self.work / "project [test]"
        shutil.copytree(ROOT / "scripts", self.project / "scripts")
        # Keep baseline launchers' old log redirections inside the fixture too.
        for script in (self.project / "scripts").glob("*.sh"):
            script.write_text(script.read_text().replace(
                "/tmp/hardware-dashboard.log", str(self.work / "legacy.log")))
        (self.project / "hardware-dashboard").write_text("#!/bin/bash\nexit 0\n")
        (self.project / "hardware-dashboard").chmod(0o700)
        self.runtime = self.work / "runtime"
        self.runtime.mkdir(mode=0o700)
        self.bin = self.work / "bin"
        self.bin.mkdir()
        self.calls = self.work / "calls.jsonl"
        stub = '''#!/usr/bin/python3
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
with open(os.environ["CALLS"], "a") as out:
    out.write(json.dumps([name, sys.argv[1:]]) + "\\n")
if name == "pgrep":
    if os.environ.get("FAIL_PGREP") == "1":
        sys.exit(2)
    sys.exit(0 if pathlib.Path(os.environ["RUNNING"]).exists() else 1)
if name == "pkill":
    pathlib.Path(os.environ["RUNNING"]).unlink(missing_ok=True)
if name in ("setsid", "nohup"):
    if os.environ.get("FAIL_LAUNCH") == "1":
        sys.exit(1)
    pathlib.Path(os.environ["RUNNING"]).touch()
if name in ("curl", "sleep"):
    if name == "curl" or sys.argv[1] == "30":
        counter = pathlib.Path(os.environ["CALLS"] + "." + name)
        count = int(counter.read_text()) + 1 if counter.exists() else 1
        counter.write_text(str(count))
        if name == "curl" and count <= int(os.environ.get("PROBE_FAILURES", "0")):
            sys.exit(1)
        if name == "sleep" and count >= int(os.environ.get("STOP_AFTER", "1")):
            os.kill(os.getppid(), 15)
if name == "logger" and os.environ.get("FAIL_LOGGER") == "1":
    sys.exit(1)
'''
        for name in ("curl", "logger", "pkill", "pgrep", "setsid", "nohup", "sleep"):
            path = self.bin / name
            path.write_text(stub)
            path.chmod(0o700)
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}",
                        XDG_RUNTIME_DIR=str(self.runtime), CALLS=str(self.calls),
                        RUNNING=str(self.work / "running"))

    def run_script(self, name):
        return subprocess.run(["bash", str(self.project / "scripts" / name)],
                              env=self.env, capture_output=True, text=True, timeout=10)

    def recorded(self):
        return [json.loads(line) for line in self.calls.read_text().splitlines()]

    def test_restart(self):
        result = self.run_script("restart-kiosk.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.recorded()
        launch = next(args for name, args in calls if name in ("nohup", "setsid"))
        self.assertEqual(launch[-1], str(self.project / "hardware-dashboard"))
        for name, args in calls:
            if name in ("pkill", "pgrep"):
                self.assertIn(str(os.getuid()), args)
                # The process pattern must match this literal path only.
                self.assertIsNotNone(re.fullmatch(args[-1], str(self.project / "hardware-dashboard")))
                self.assertIsNone(re.fullmatch(args[-1], str(self.project / "hardware-dashboard").replace("[test]", "t")))
        log = self.runtime / "rtk-hardware-monitor" / "hardware-dashboard.log"
        self.assertTrue(log.is_file())
        self.assertEqual(log.stat().st_mode & 0o077, 0)

    def test_launch_failure_is_failure(self):
        self.env["FAIL_LAUNCH"] = "1"
        result = self.run_script("restart-kiosk.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("done", result.stdout)

    def test_process_check_failure(self):
        self.env["FAIL_PGREP"] = "1"
        result = self.run_script("restart-kiosk.sh")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse(any(name == "setsid" for name, _ in self.recorded()))

    def test_shared_runtime_rejected(self):
        self.runtime.chmod(0o777)
        result = self.run_script("restart-kiosk.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.calls.exists())

    def test_state_symlink_rejected(self):
        target = self.work / "outside"
        target.mkdir()
        (self.runtime / "rtk-hardware-monitor").symlink_to(target)
        result = self.run_script("restart-kiosk.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(target.iterdir()), [])

    def test_log_symlink_rejected(self):
        state = self.runtime / "rtk-hardware-monitor"
        state.mkdir(mode=0o700)
        target = self.work / "valuable"
        target.write_text("keep this")
        (state / "hardware-dashboard.log").symlink_to(target)
        result = self.run_script("restart-kiosk.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(target.read_text(), "keep this")

    def test_missing_binary(self):
        (self.project / "hardware-dashboard").unlink()
        result = self.run_script("restart-kiosk.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.calls.exists())

    def test_watchdog_launch(self):
        result = self.run_script("kiosk-watchdog.sh")
        self.assertEqual(result.returncode, -15, result.stderr)
        calls = self.recorded()
        launch = next(args for name, args in calls if name == "setsid")
        self.assertEqual(launch[-1], str(self.project / "hardware-dashboard"))
        self.assertTrue((self.runtime / "rtk-hardware-monitor" / "hardware-dashboard.log").is_file())

    def test_watchdog_existing_process(self):
        Path(self.env["RUNNING"]).touch()
        result = self.run_script("kiosk-watchdog.sh")
        self.assertEqual(result.returncode, -15, result.stderr)
        self.assertFalse(any(name in ("setsid", "pkill") for name, _ in self.recorded()))

    def test_watchdog_recovery(self):
        Path(self.env["RUNNING"]).touch()
        self.env.update(PROBE_FAILURES="2", STOP_AFTER="3", FAIL_LOGGER="1")
        result = self.run_script("kiosk-watchdog.sh")
        self.assertEqual(result.returncode, -15, result.stderr)
        names = [name for name, _ in self.recorded()]
        self.assertEqual(names.count("pkill"), 1)
        self.assertEqual(names.count("setsid"), 1)

    def test_watchdog_retries_failed_launch(self):
        self.env.update(FAIL_LAUNCH="1", STOP_AFTER="2")
        result = self.run_script("kiosk-watchdog.sh")
        self.assertEqual(result.returncode, -15, result.stderr)
        self.assertEqual(sum(name == "setsid" for name, _ in self.recorded()), 2)

    def test_watchdog_singleton(self):
        import fcntl
        state = self.runtime / "rtk-hardware-monitor"
        state.mkdir(mode=0o700)
        with (state / "watchdog.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_script("kiosk-watchdog.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.calls.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
