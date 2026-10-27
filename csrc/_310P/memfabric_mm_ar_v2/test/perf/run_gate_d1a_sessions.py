#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""D1-A 会话批调度器：ABBA/交叉顺序执行 12 个短窗口会话。

每个会话 = 独立 torchrun（Gate C 纪律：一会话一 case 一 mode）。
每会话前后核验 5664 双 die AICore 空闲；任何失败立即停止整批（强制停机条件）。
顺序（块 1 升序 / 块 2 反序 = ABBA，case 交错）：
  formal-ext, loop4-ext, formal-prof, loop4-prof, formal-aic, loop4-aic,
  loop4-aic, formal-aic, loop4-prof, formal-prof, loop4-ext, formal-ext
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path("/home/USER/310P/vllm-ascend")
DRIVER = "csrc/_310P/memfabric_mm_ar_v2/test/perf/gate_d1a_perturbation.py"
OUT_ROOT = Path("/tmp/opencode/gate_d1a/sessions")
ORCH_JSON = "/usr/local/memfabric_hybrid/1.2.0/aarch64-linux/hybm/aicpu_kernel/libmf_sdma_orch_v11.json"
WARMUP = 3
ITERS = 16

PLAN = [
    ("formal", "external", 1),
    ("loop4", "external", 1),
    ("formal", "profile", 1),
    ("loop4", "profile", 1),
    ("formal", "aic", 1),
    ("loop4", "aic", 1),
    ("loop4", "aic", 2),
    ("formal", "aic", 2),
    ("loop4", "profile", 2),
    ("formal", "profile", 2),
    ("loop4", "external", 2),
    ("formal", "external", 2),
]


def npu_state():
    out = subprocess.run(["npu-smi", "info"], capture_output=True, text=True).stdout
    rows = []
    for line in out.splitlines():
        m = re.match(r"\|\s*(\d+)\s+(\d+)\s+\|\s*([0-9:.]+)\s+\|\s*(\d+)\s+(\d+)\s*/\s*(\d+)", line)
        if m:
            rows.append(
                {
                    "npu": int(m.group(1)),
                    "chip": int(m.group(2)),
                    "bus": m.group(3),
                    "aicore_pct": int(m.group(4)),
                    "mem_used_mb": int(m.group(5)),
                    "mem_total_mb": int(m.group(6)),
                }
            )
    return rows, out


def check_idle(tag: str):
    rows, raw = npu_state()
    (OUT_ROOT / f"npu_smi_{tag}.txt").write_text(raw)
    # ASCEND_RT_VISIBLE_DEVICES=0,1 -> 全局 device id 0/1 = 5664 双 die
    dies = [d for d in rows if d["chip"] in (0, 1)]
    if len(dies) != 2:
        raise RuntimeError(f"{tag}: expected 2 dies for device 0/1, got {dies}")
    bad = [d for d in dies if d["aicore_pct"] != 0]
    if bad:
        raise RuntimeError(f"{tag}: 5664 not idle: {bad}")
    print(f"[{tag}] idle OK: {json.dumps(dies)}", flush=True)
    return dies


def run_session(case: str, mode: str, rep: int):
    name = f"{case}_{mode}_r{rep}"
    out_dir = OUT_ROOT / name
    if out_dir.exists() and any(out_dir.glob("*_d1a.json")):
        raise RuntimeError(f"{name}: prior evidence exists, refusing to overwrite")
    out_dir.mkdir(parents=True, exist_ok=True)
    before = check_idle(f"{name}_before")
    cmd = [
        sys.executable,
        "-m",
        "torch.distributed.run",
        "--standalone",
        "--nproc-per-node=2",
        DRIVER,
        "--case",
        case,
        "--mode",
        mode,
        "--out",
        str(out_dir),
        "--warmup",
        str(WARMUP),
        "--iters",
        str(ITERS),
    ]
    env = {
        **{k: v for k, v in os.environ.items() if not k.startswith(("MF_", "VLLM_", "ASCEND_WORK"))},
        "ASCEND_RT_VISIBLE_DEVICES": "0,1",
        "MF_SDMA_ORCH_JSON": ORCH_JSON,
        # MemFabric set_env.sh 等效项（Host SO 所在 lib64），与 Gate C 会话一致
        "LD_LIBRARY_PATH": "/usr/local/memfabric_hybrid/latest/aarch64-linux/lib64:"
        + os.environ.get("LD_LIBRARY_PATH", ""),
    }
    t0 = time.time()
    log = out_dir / "session.log"
    with open(log, "w") as fh:
        proc = subprocess.run(cmd, cwd=ROOT, env=env, stdout=fh, stderr=subprocess.STDOUT, timeout=1500)
    elapsed = time.time() - t0
    if proc.returncode != 0:
        print(log.read_text()[-3000:])
        raise RuntimeError(f"{name}: torchrun rc={proc.returncode} (log: {log})")
    results = sorted(out_dir.glob("*_rank*_d1a.json"))
    if len(results) != 2:
        raise RuntimeError(f"{name}: expected 2 rank results, got {len(results)}")
    for rp in results:
        meta = json.loads(rp.read_text())
        if not (meta["bit_exact"] and meta["status"] == 0):
            raise RuntimeError(f"{name}: correctness gate failed in {rp}")
    after = check_idle(f"{name}_after")
    mem_shift = [a["mem_used_mb"] - b["mem_used_mb"] for a, b in zip(after, before)]
    print(f"[{name}] OK in {elapsed:.0f}s mem_shift={mem_shift}", flush=True)
    return name


def is_idle():
    rows, _ = npu_state()
    dies = [d for d in rows if d["chip"] in (0, 1)]
    return len(dies) == 2 and all(d["aicore_pct"] == 0 for d in dies)


def wait_idle_window(confirm_s: float = 45.0, max_wait_s: float = 3600.0):
    """等待出现持续空闲窗口：连续两次（间隔 confirm_s 秒）双 die AICore=0。

    共享宿主存在分钟级瞬态外部负载（Gate C 记录同类现象，仅打 die1）；
    测量窗口约 60s，须在空闲窗口内启动；msprof 解析阶段不接触设备。
    """
    t0 = time.time()
    while time.time() - t0 < max_wait_s:
        if is_idle():
            time.sleep(confirm_s)
            if is_idle():
                return True
        time.sleep(15)
    return False


def run_session_with_idle_retry(case: str, mode: str, rep: int, max_attempts: int = 8):
    """空闲门失败时等待而非放弃；torchrun 产物存在则先视为已完成。

    若会话已完成但 after 门发现不空闲（中途污染候选），将整目录移入
    _contaminated_<name>_<n> 保全后重跑，不销毁证据。
    """
    name = f"{case}_{mode}_r{rep}"
    out_dir = OUT_ROOT / name
    if out_dir.exists() and any(out_dir.glob("*_d1a.json")):
        print(f"[{name}] prior evidence exists, skipping", flush=True)
        return name
    if out_dir.exists():
        shutil.rmtree(out_dir)
    for attempt in range(1, max_attempts + 1):
        try:
            run_session(case, mode, rep)
            return name
        except RuntimeError as exc:
            if "not idle" in str(exc):
                print(f"[{name}] attempt {attempt}: idle gate blocked, waiting ({exc})", flush=True)
                if out_dir.exists():
                    quarantine = OUT_ROOT / f"_contaminated_{name}_a{attempt}"
                    shutil.move(str(out_dir), str(quarantine))
                wait_idle_window()
                continue
            raise
    raise RuntimeError(f"{name}: exhausted {max_attempts} idle-gated attempts")


def main():
    only = sys.argv[1:] if len(sys.argv) > 1 else None
    done = []
    for case, mode, rep in PLAN:
        name = f"{case}_{mode}_r{rep}"
        if only and name not in only:
            continue
        run_session_with_idle_retry(case, mode, rep)
        done.append(name)
    print(json.dumps({"completed": done}, indent=2))


if __name__ == "__main__":
    main()
