# SPDX-License-Identifier: Apache-2.0
"""F1-C CPU离线合同测试：不声称NPU/HCCL并行执行已获实测验证。"""
from __future__ import annotations
import ast
import json
import runpy
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "test/perf"
M = runpy.run_path(str(SCRIPTS / "f1c_analyze.py"))


def record(arm, rank, session, m=10, fast=400):
    return {
        "experiment": "F1C_SEPARATE_PROCESS_CROSS_ABBA", "arm": arm, "rank": rank,
        "session": session, "M": m, "K": 2048, "N": 2048, "TP": 2,
        "dtype": "fp16", "stock_collective_dtype": "fp16",
        "reference": "CPU_GLOO_FP32_SUM_TO_FP16",
        "communication_isolation": "HCCL group only Stock; MemFabric pool only T1",
        "host_f32_conversion_in_timed_stock": False, "iters": 50, "warmup": 5,
        "bit_exact": True, "mm_cores": 8 if arm == "memfabric_t1" else None,
        "add_cores": 4 if arm == "memfabric_t1" else None,
        "protocol_status": 0 if arm == "memfabric_t1" else None,
        "device_so_sha256": M["D4_DEVICE"],
        "plugin_sha256": M["GATE_C_PLUGIN"],
        "host_so_sha256": M["HOST_PATCH"],
        "kver11_sha256": M["KVER11"],
        "samples": [{"i": i, "input_variant": (i + 5) % 2,
                     "event_us": float(fast + rank + i % 3),
                     "wall_us": float(fast + rank + i % 3 + 50)}
                    for i in range(50)],
    }


def fixture(root, m=10, fused_us=345, stock_us=400):
    (root / "manifest.txt").write_text("\n".join((
        "experiment=F1C_SEPARATE_PROCESS_CROSS_ABBA",
        f"M={m}", "TP=2", "order=A_B_B_A__A_B_B_A",
        "device_so_sha256=" + M["D4_DEVICE"],
        "plugin_sha256=" + M["GATE_C_PLUGIN"],
        "memfabric_host_sha256=" + M["HOST_PATCH"],
        "kver11_sha256=" + M["KVER11"],
    )) + "\n")
    sessions = root / "sessions"
    sessions.mkdir(parents=True)
    for i, arm in enumerate(M["ORDER"]):
        name = f"{i+1:02d}_m{m}_{arm}"
        d = sessions / name
        d.mkdir()
        (d / "session.log").write_text("completed 50/50\n")
        for r in (0, 1):
            item = record(arm, r, name, m, fused_us if arm == "memfabric_t1" else stock_us)
            (d / f"{name}_rank{r}.json").write_text(json.dumps(item))
    return sessions


def test_full_8_sessions_and_no_unaudited_go(tmp_path):
    d = fixture(tmp_path)
    result = M["analyze"](d, 10)
    assert len(result["sessions"]) == 8
    assert all(x["sample_count"] == 50 for x in result["sessions"])
    assert result["event_speedup_pct"] > 5
    assert result["verdict"] == "PROVISIONAL_ONLY_ROUTE_AND_HCCL_EVENT_AUDIT_REQUIRED"
    assert all(x["speedup_pct"] > 5 for x in result["abba_windows"])


def test_valid_audit_only_opens_conditional_operator_gate(tmp_path):
    d = fixture(tmp_path)
    evidence = tmp_path / "evidence"
    evidence.mkdir()
    for n in ("hccl_trace.txt", "fused_trace.txt"):
        (evidence / n).write_text("manually independently validated\n")
    audit = tmp_path / "audit.json"
    audit.write_text(json.dumps({
        "stock_fp16_hccl_verified": True,
        "memfabric_t1_only_verified": True,
        "no_simultaneous_hccl_memfabric": True,
        "hccl_event_stream_bracketing_verified": True,
        "evidence_files": ["evidence/hccl_trace.txt", "evidence/fused_trace.txt"],
    }))
    good = M["analyze"](d, 10, audit)
    assert good["verdict"] == "F1C_OPERATOR_GO_FURTHER_CORRECTNESS_AND_QWEN_REQUIRED"
    audit.write_text(json.dumps({
        "stock_fp16_hccl_verified": True,
        "memfabric_t1_only_verified": True,
        "no_simultaneous_hccl_memfabric": True,
        "hccl_event_stream_bracketing_verified": False,
        "evidence_files": ["evidence/hccl_trace.txt", "evidence/fused_trace.txt"],
    }))
    with pytest.raises(ValueError, match="audit"):
        M["analyze"](d, 10, audit)


def test_altered_shape_missing_or_slow_rank_fail_closed(tmp_path):
    d = fixture(tmp_path)
    js = d / "01_m10_stock_fp16/01_m10_stock_fp16_rank1.json"
    item = json.loads(js.read_text())
    item["M"] = 8
    js.write_text(json.dumps(item))
    with pytest.raises(ValueError, match="mismatch"):
        M["analyze"](d, 10)
    item["M"] = 10
    item["samples"][0]["event_us"] = float("nan")
    js.write_text(json.dumps(item))
    with pytest.raises(ValueError, match="invalid"):
        M["analyze"](d, 10)


def test_rank_pair_uses_max_per_sample_not_max_of_two_rank_p50():
    a = record("stock_fp16", 0, "demo")
    b = record("stock_fp16", 1, "demo")
    for i in range(50):
        a["samples"][i]["event_us"] = float(101 if i % 2 else 301)
        b["samples"][i]["event_us"] = float(301 if i % 2 else 101)
    assert M["validate_pair"](a, b, 10, "stock_fp16")["pair_event_p50_us"] == 301.0


def test_source_scope_and_isolation():
    bench = (SCRIPTS / "f1c_arm.py").read_text()
    runner = (SCRIPTS / "f1c_run_abba.sh").read_text()
    ast.parse(bench)
    for phrase in ("dist.init_process_group(backend=\"hccl\")",
                   "cpu_group = dist.new_group(backend=\"gloo\")",
                   "dist.init_process_group(backend=\"gloo\")",
                   "dist.barrier(group=cpu_group)",
                   "torch.npu.Event(enable_timing=True)",
                   "mf310p_v2_trial_run_nz_direct_small", "torch.equal(got, ref)",
                   "npu_format_cast(w, 29)"):
        assert phrase in bench
    assert "_launch_stock(x, w, hccl)" in bench
    assert "float().cpu()" in bench  # reference only, before actual benchmark.
    assert "for arm in stock_fp16 memfabric_t1 memfabric_t1 stock_fp16" in runner
    assert "timeout --signal=TERM --kill-after=30s" in runner
    assert "F1C_DEVICE_SHA256" in runner and "F1C_HOST_SHA256" in runner
    assert "MF310P_V2_NOWARMUP" in runner and "MF_KFC_BYPASS" in runner
    assert "f1c_provisional.json" in runner


def test_port_guard_is_fail_closed_proc_net_probe():
    runner = (SCRIPTS / "f1c_run_abba.sh").read_text()
    probe_src = (SCRIPTS / "f1c_port_probe.py").read_text()
    ast.parse(probe_src)
    # The host has no ss/netstat: the runner must use the /proc/net probe and
    # treat a busy required port as a hard failure instead of best-effort log.
    assert "f1c_port_probe.py" in runner
    assert " ss " not in runner and "ss -ltnp" not in runner
    assert "pre_port_listeners.txt" in runner
    assert '"$PORT_PROBE" > "$dir/pre_port_listeners.txt" 2>&1 ||' in runner
    assert 'echo "port probe failure or required port busy' in runner
    assert "/proc/net/tcp" in probe_src and "0A" in probe_src
    assert "REQUIRED_FREE_PORT" in probe_src and "return 1" in probe_src


def test_missing_session_cannot_report_a_win(tmp_path):
    d = fixture(tmp_path)
    (d / "08_m10_stock_fp16/08_m10_stock_fp16_rank1.json").unlink()
    with pytest.raises(ValueError, match="JSON"):
        M["analyze"](d, 10)

def test_manifest_wrong_binary_and_2ms_tail_cannot_be_operator_go(tmp_path):
    d = fixture(tmp_path)
    manifest = tmp_path / "manifest.txt"
    contents = manifest.read_text()
    manifest.write_text(contents.replace(M["D4_DEVICE"], "0" * 64))
    with pytest.raises(ValueError, match="manifest"):
        M["analyze"](d, 10)
    manifest.write_text(contents)
    slow = d / "02_m10_memfabric_t1/02_m10_memfabric_t1_rank0.json"
    item = json.loads(slow.read_text())
    item["samples"][0]["event_us"] = 2300.0
    item["samples"][0]["wall_us"] = 2400.0
    slow.write_text(json.dumps(item))
    result = M["analyze"](d, 10)
    assert result["sessions"][1]["samples_ge_2ms"] == 1
    # No audit => never GO, independent of impressive median.
    assert result["verdict"].startswith("PROVISIONAL_ONLY")
