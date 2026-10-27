# SPDX-License-Identifier: Apache-2.0
"""D1-A CPU-only 源码合同测试：不需要 CANN / torch_npu。"""

import json
import runpy
from pathlib import Path

V2 = Path(__file__).resolve().parents[2]
PERF = V2 / "test/perf"


def test_driver_mode_matrix_and_gates():
    runner = (PERF / "gate_d1a_perturbation.py").read_text()
    assert 'MODES = ("external", "profile", "aic")' in runner
    assert 'CASES = ("formal", "loop4")' in runner
    assert "M = 4096" in runner
    assert "CHUNK = 1024" in runner
    # KVER11 与首写保护 fail-closed，与 Gate C 同纪律。
    assert "must explicitly select an absolute KVER11 JSON path" in runner
    assert "MF310P_V2_NOWARMUP must be unset" in runner
    assert "WORLD_SIZE=2 is required" in runner
    # 每样本逐次位精确 + status 校验仍在计时窗口外。
    assert "torch.equal(y, ref)" in runner
    assert "mf310p_v2_trial_check_status" in runner
    assert "must(lib.mf310p_v2_trial_ib_probe" in runner
    assert "FileExistsError" in runner
    # AIC 指标参数与 Gate C 完全一致（PipeUtilization + Level1）。
    assert "AiCMetrics.PipeUtilization" in runner
    assert "ProfilerLevel.Level1" in runner
    assert "ASCEND_WORK_PATH" in runner


def test_driver_external_timing_semantics_documented():
    runner = (PERF / "gate_d1a_perturbation.py").read_text()
    # 双口径外部计时必须在产物里声明语义，禁止与设备 Kernel Duration 混同。
    assert '"wall_us":' in runner
    assert '"event_us":' in runner
    assert "timing_semantics" in runner
    assert "NOT identical to msprof Task Duration" in runner
    assert "perf_counter_ns" in runner
    assert "elapsed_time" in runner


def test_analyzer_fail_closed_contracts(tmp_path):
    mod = runpy.run_path(str(PERF / "analyze_gate_d1a.py"))
    # 造一个最小合法会话：双 Rank、iters 一致、位精确、trace 核数精确。
    session = tmp_path / "formal_profile_rX"
    session.mkdir()
    for rank in (0, 1):
        samples = [{"i": i, "wall_us": 2900.0 + i, "event_us": 2700.0 + i} for i in range(4)]
        meta = {
            "case": "formal",
            "mode": "profile",
            "rank": rank,
            "iters": 4,
            "bit_exact": True,
            "status": 0,
            "samples": samples,
            "trace": str(session / f"t{rank}.json"),
            "prof_dir": None,
        }
        (session / f"formal_profile_rank{rank}_d1a.json").write_text(json.dumps(meta))
        (session / f"t{rank}.json").write_text(
            json.dumps({"traceEvents": [{"ph": "X", "name": "V2TrialKernel", "dur": 2600 + i} for i in range(4)]})
        )
    out = mod["analyze_session"](session)
    assert out["case"] == "formal" and out["mode"] == "profile"
    r0 = out["ranks"]["0"]
    assert r0["kernel"]["n"] == 4
    assert r0["kernel"]["p50"] == 2601.5
    assert out["pairing"]["wall"]["both_stall"] == []
    # 样本数与 iters 不符必须拒绝。
    bad = json.loads((session / "formal_profile_rank0_d1a.json").read_text())
    bad["samples"] = bad["samples"][:3]
    (session / "formal_profile_rank0_d1a.json").write_text(json.dumps(bad))
    try:
        mod["analyze_session"](session)
        raise AssertionError("expected fail-closed ValueError")
    except ValueError as exc:
        assert "samples !=" in str(exc)


def test_launcher_plan_is_abba_and_idle_gated():
    mod = runpy.run_path(str(PERF / "run_gate_d1a_sessions.py"))
    plan = mod["PLAN"]
    assert len(plan) == 12
    # ABBA：前半与后半（去掉 case 交错后）模式序列互为镜像。
    modes = [(c, m) for c, m, _ in plan]
    half = len(plan) // 2
    assert modes[:half] == list(reversed(modes[half:]))
    # 每种 case×mode 恰好两次重复采样。
    from collections import Counter

    counts = Counter((c, m) for c, m, _ in plan)
    assert set(counts.values()) == {2}
    # 空闲检查按全局 device id 0/1 选择 5664 双 die，而非按行序。
    src = (PERF / "run_gate_d1a_sessions.py").read_text()
    assert 'd["chip"] in (0, 1)' in src
    assert "aicore_pct" in src
    # 失败即停：会话非零返回码直接抛错终止整批。
    assert "torchrun rc=" in src
