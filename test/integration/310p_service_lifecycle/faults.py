"""Run only against an explicitly selected unused test device."""

import os
import pathlib
import signal
import re
import subprocess
import sys
import time

if "TEST_DEVICE_BASE" not in os.environ or "TEST_LOG_DIR" not in os.environ:
    sys.exit("Set TEST_DEVICE_BASE and TEST_LOG_DIR")
root = pathlib.Path(__file__).resolve().parent
logs = pathlib.Path(os.environ["TEST_LOG_DIR"])
logs.mkdir(parents=True, exist_ok=True)


def query(label):
    started = time.monotonic()
    result = subprocess.run(
        ["npu-smi", "info"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=5, check=True
    )
    (logs / (label + "-smi.log")).write_bytes(result.stdout)
    print(label, "npu-smi seconds", time.monotonic() - started, flush=True)


def probe(mode):
    output = logs / (mode + ".log")
    stream = output.open("w")
    child = subprocess.Popen(["bash", str(root / "run.sh"), mode], stdout=stream, stderr=subprocess.STDOUT, cwd=root)
    return child, output, stream


def ready(child, output, cycle):
    deadline = time.monotonic() + 40
    pattern = r"READY pid=(\d+) cycle=" + str(cycle) + r" "
    while time.monotonic() < deadline:
        pids = {int(pid) for pid in re.findall(pattern, output.read_text())}
        if len(pids) == 2:
            expected = (root / "build/lifecycle").resolve()
            for pid in pids:
                if pathlib.Path("/proc", str(pid), "exe").resolve() != expected:
                    raise RuntimeError("unexpected probe executable")
            return pids
        if child.poll() is not None:
            raise RuntimeError("probe failed: " + output.read_text())
        time.sleep(0.1)
    raise TimeoutError("probe startup")


query("before")
child, output, stream = probe("lease")
try:
    for cycle in range(3):
        pids = ready(child, output, cycle)
        time.sleep(16)
        query("lease-" + str(cycle))
        for pid in pids:
            os.kill(pid, signal.SIGCONT)
    if child.wait(timeout=20) != 0:
        raise RuntimeError(output.read_text())
finally:
    if child.poll() is None:
        child.kill()
        child.wait(timeout=10)
    stream.close()

child, output, stream = probe("hold")
try:
    pids = ready(child, output, 0)
    for pid in pids:
        os.kill(pid, signal.SIGKILL)
    if child.wait(timeout=20) == 0:
        raise RuntimeError("unexpected successful SIGKILL exit")
finally:
    if child.poll() is None:
        child.kill()
        child.wait(timeout=10)
    stream.close()
query("killed")
time.sleep(16)
query("after-lease")
subprocess.run(["bash", str(root / "run.sh"), "normal"], cwd=root, check=True, timeout=45)
query("after-reinitialize")
