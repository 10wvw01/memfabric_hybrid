# SPDX-License-Identifier: Apache-2.0
"""U1纯CPU合成测试：必须在进入310P3前实际pytest PASS；合成数据不代表任何实机性能。"""
from __future__ import annotations

import ast
import json
import sys
from pathlib import Path

import pytest

PERF = Path(__file__).resolve().parents[1] / "perf"
sys.path.insert(0, str(PERF))
import u1_spec as sp  # noqa: E402
import u1_analyze as an  # noqa: E402


def synth_rank(p, rank, fast=400):
    readings = {}
    for m in sp.SHAPES[p["scope"]]:
        readings[str(m)] = {
            "M": m, "arm": p["arm"],
            "warmup": p["warmup"], "iters": p["iters"],
            "bit_exact": True,
            "status": None if p["arm"] == "stock_fp16" else 0,
            "samples": [
                {"i": i, "variant": (i + p["warmup"]) % 2,
                 "event_us": float(fast + rank + i % 3),
                 "wall_us": float(fast + rank + 30 + i % 3)}
                for i in range(p["iters"])
            ],
        }
        if p["arm"] == "stock_fp16" and p["phase"] == "screen":
            readings[str(m)]["native_mm_only"] = {
                "iters": p["iters"], "bit_exact": True,
                "samples": [
                    {"i": i, "variant": (i + p["warmup"]) % 2,
                     "event_us": float(fast / 2 + rank + i % 3),
                     "wall_us": float(fast / 2 + 30 + rank + i % 3)}
                    for i in range(p["iters"])
                ],
            }
    readings["_anchor_after"] = {
        "M": p["M_order"][0],
        "samples": [{"i": i, "variant": i % 2,
                     "event_us": float(fast + rank + i % 3),
                     "wall_us": float(fast + rank + 30 + i % 3)}
                    for i in range(8)]
    }
    return {
        "experiment": "U1_FULLCHAIN", "scope": p["scope"], "phase": p["phase"],
        "session": p["session"], "stage": p["stage"], "arm": p["arm"],
        "rank": rank, "M_order": p["M_order"],
        "TP": 2, "K": sp.K, "N": sp.N, "dtype": "fp16",
        "reference": "CPU_GLOO_FP32_SUM_TO_FP16",
        "stock_collective": "FP16_HCCL_no_FP32_conversion",
        "comm_isolation": "separate process; never HCCL with MemFabric pool",
        "device_so_sha256": sp.D4_DEVICE_SHA,
        "plugin_sha256": sp.PLUGIN_SHA,
        "host_so_sha256": sp.MEMFABRIC_HOST_SHA,
        "kver11_sha256": sp.KVER_SHA,
        "profile": False, "results": readings,
    }


def fixture(root, scope="small", phase="screen", stock=500, fused=400):
    specs = sp.plan(scope, phase)
    (root / "sessions").mkdir()
    (root / "manifest.json").write_text(json.dumps({
        "experiment": "U1_FULLCHAIN", "scope": scope, "phase": phase,
        "device_so_sha256": sp.D4_DEVICE_SHA,
        "plugin_sha256": sp.PLUGIN_SHA,
        "host_so_sha256": sp.MEMFABRIC_HOST_SHA,
        "kver11_sha256": sp.KVER_SHA,
        "session_ids": [s["session"] for s in specs],
    }))
    for p in specs:
        folder = root / "sessions" / p["session"]
        folder.mkdir()
        (folder / "session.log").write_text("SUCCESS\n")
        base = stock if p["arm"] == "stock_fp16" else fused
        for r in range(2):
            name = p["session"] + f"_rank{r}.json"
            (folder / name).write_text(json.dumps(synth_rank(p, r, base)))


def test_scope_no_unverified_geometry_and_rotate():
    assert sp.SMALL_MS == (1, 8, 10, 16, 17, 64, 255, 256)
    assert sp.LARGE_MS == (1024, 2048, 4096)
    assert all(sp.eligible("small", x) for x in sp.SMALL_MS)
    assert all(sp.eligible("large", x) for x in sp.LARGE_MS)
    assert not sp.eligible("large", 512)
    assert not sp.eligible("small", 4096)
    assert sp.SCHEDULE == ("stock_fp16", "fused", "fused", "stock_fp16") * 2
    for scope in sp.SHAPES:
        for phase in sp.ITERS:
            p = sp.plan(scope, phase)
            assert len(p) == (2 if phase in ("smoke", "screen") else 8)
            for item in p:
                if phase == "smoke":
                    assert item["M_order"] == ((10,) if scope == "small" else (2048,))
                else:
                    assert set(item["M_order"]) == set(sp.SHAPES[scope])
                    assert len(item["M_order"]) == len(sp.SHAPES[scope])


@pytest.mark.parametrize("scope", ["small", "large"])
def test_screen_never_go(tmp_path, scope):
    fixture(tmp_path, scope=scope)
    result = an.evaluate(tmp_path, scope, "screen")
    assert len(result["performance_map"]) == len(sp.SHAPES[scope])
    assert all(x["speedup_pct"] > 10 for x in result["performance_map"])
    assert all(x["decision"].startswith("SCREEN_ONLY") for x in result["performance_map"])


def test_confirm_requires_independent_audit_to_promote(tmp_path):
    fixture(tmp_path, phase="confirm", stock=500, fused=400)
    a = an.evaluate(tmp_path, "small", "confirm")
    assert a["within_worker_anchor_drift_exceeds_5pct"] is False
    assert all(x["decision"] == "CANDIDATE_REQUIRES_ROUTE_AUDIT"
               for x in a["performance_map"])
    audit = tmp_path / "audit.json"
    evidence = tmp_path / "route_proof"
    evidence.mkdir()
    for name in ("stock_trace.json", "t1_trace.json"):
        (evidence / name).write_text("{}")
    audit.write_text(json.dumps({
        "stock_hccl_fp16_verified": True,
        "memfabric_route_verified": True,
        "hccl_stream_end_to_end_verified": True,
        "no_backend_coexistence": True,
        "evidence_files": ["route_proof/stock_trace.json", "route_proof/t1_trace.json"],
    }))
    b = an.evaluate(tmp_path, "small", "confirm", audit)
    assert all(x["decision"] == "OPERATOR_CANDIDATE_ONLY_MODEL_GATE_REMAINS"
               for x in b["performance_map"])
    bad = json.loads(audit.read_text())
    bad["hccl_stream_end_to_end_verified"] = False
    audit.write_text(json.dumps(bad))
    with pytest.raises(ValueError, match="audit"):
        an.evaluate(tmp_path, "small", "confirm", audit)


def test_missing_rank_raw_log_and_nan_fail_closed(tmp_path):
    fixture(tmp_path)
    directory = tmp_path / "sessions/01_small_stock_fp16"
    f = directory / "01_small_stock_fp16_rank0.json"
    x = json.loads(f.read_text())
    x["results"]["10"]["samples"][0]["event_us"] = "NaN"
    f.write_text(json.dumps(x))
    with pytest.raises(ValueError, match="invalid timing"):
        an.evaluate(tmp_path, "small", "screen")
    x["results"]["10"]["samples"][0]["event_us"] = 501.0
    f.write_text(json.dumps(x))
    (directory / "session.log").unlink()
    with pytest.raises(ValueError, match="raw log"):
        an.evaluate(tmp_path, "small", "screen")


def test_incorrect_protocol_status_and_missing_anchor_rejected(tmp_path):
    fixture(tmp_path)
    folder = tmp_path / "sessions/02_small_memfabric_t1"
    f = folder / "02_small_memfabric_t1_rank1.json"
    original = json.loads(f.read_text())
    x = json.loads(f.read_text())
    x["results"]["10"]["status"] = 507014
    f.write_text(json.dumps(x))
    with pytest.raises(ValueError, match="status"):
        an.evaluate(tmp_path, "small", "screen")
    x = original
    del x["results"]["_anchor_after"]
    f.write_text(json.dumps(x))
    with pytest.raises(ValueError, match="late"):
        an.evaluate(tmp_path, "small", "screen")


def test_rank_samples_must_pair_before_percentiles():
    p = sp.plan("small", "screen")[0]
    x = synth_rank(p, 0)
    y = synth_rank(p, 1)
    # 两Rank交替100us/500us：先各自p50再max会错误；成对max每次都500us。
    for i in range(p["iters"]):
        x["results"]["10"]["samples"][i]["event_us"] = 500.0 if i % 2 else 100.0
        y["results"]["10"]["samples"][i]["event_us"] = 100.0 if i % 2 else 500.0
    outcome = an.paired([x, y], p)
    assert outcome["10"]["event_p50_us"] == 500.0


def test_python_static_and_bash_g0_guards():
    for p in ("u1_spec.py", "u1_arm.py", "u1_analyze.py", "u1_diag_mm.py",
              "u1_diag_large.py", "u1_e2e_budget.py"):
        ast.parse((PERF / p).read_text())
    runner = (PERF / "u1_run.sh").read_text()
    assert "MF310P_V2_NOWARMUP" in runner and "MF_KFC_BYPASS" in runner
    assert "U1_CONFIRM_APPROVED" in runner and "U1_G0_APPROVED" in runner
    assert "timeout --signal=TERM --kill-after=30s" in runner
    assert "session.log" in runner and "U1_DEVICE_SHA256" in runner
    driver = (PERF / "u1_arm.py").read_text()
    assert 'dist.init_process_group(backend="hccl")' in driver
    assert 'dist.init_process_group(backend="gloo")' in driver
    assert "mf310p_v2_trial_run_nz_direct_small" in driver
    assert "mf310p_v2_trial_run" in driver
    assert "torch.equal(y, ref)" in driver
    assert "results[\"_anchor_after\"]" in driver


def test_smoke_does_not_authorize_performance_analysis(tmp_path):
    assert [p["arm"] for p in sp.plan("small", "smoke")] == [
        "stock_fp16", "memfabric_t1"
    ]
    assert [p["arm"] for p in sp.plan("large", "smoke")] == [
        "stock_fp16", "memfabric_d4"
    ]
    assert all(len(p["M_order"]) == 1 for p in sp.plan("small", "smoke"))
    with pytest.raises(ValueError, match="smoke"):
        an.evaluate(tmp_path, "small", "smoke")
    campaign = (PERF / "u1_campaign.sh").read_text()
    assert campaign.index('"$OUT/small_smoke"') < campaign.index('"$OUT/small_screen"')
    assert campaign.index('"$OUT/large_smoke"') < campaign.index('"$OUT/large_screen"')


def test_serving_budget_requires_observed_real_shapes():
    import u1_e2e_budget as budget
    measured = {"experiment": "U1_FULLCHAIN", "performance_map": [
        {"M": 10, "stock_p50_us": 400, "fused_p50_us": 350, "decision": "NO_GO"}]}
    evidence = {"input_tokens": 4096, "output_tokens": 2048,
                "tp": 2, "batch_max": 10, "eager": True,
                "elapsed_seconds": 100.0, "measurement_id": "unit-test-synthetic",
                "calls": [{"M": 10, "count": 100}, {"M": 2048, "count": 7}]}
    z = budget.build_budget([measured], evidence)
    assert z["optimistic_time_saving_seconds"] == 0.005
    assert z["observed_model_speedup_pct"] is None
    assert z["calls"][1]["status"] == "UNKNOWN_MEASURED_SHAPE_OR_DRIFT"
    bad = dict(evidence, output_tokens=512)
    with pytest.raises(ValueError, match="workload mismatch"):
        budget.build_budget([measured], bad)


def test_stock_mm_only_must_have_full_paired_samples(tmp_path):
    fixture(tmp_path)
    path = tmp_path / "sessions/01_small_stock_fp16/01_small_stock_fp16_rank0.json"
    obj = json.loads(path.read_text())
    assert "native_mm_only" in obj["results"]["10"]
    del obj["results"]["10"]["native_mm_only"]["samples"][0]
    path.write_text(json.dumps(obj))
    with pytest.raises(ValueError, match="MM-only samples missing"):
        an.evaluate(tmp_path, "small", "screen")
