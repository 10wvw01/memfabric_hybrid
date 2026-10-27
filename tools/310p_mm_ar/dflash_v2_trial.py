#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Stock+DFlash vs V2+DFlash, K=7 TP=2: gated, isolated, auditable trial.

plan = CPU only, execute = NPU work only with explicit operator approvals.
Production Serving, Kernel and runtime code remain unchanged.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
import urllib.request

SPEC_TOKENS = 7
VERIFY_WIDTH = SPEC_TOKENS + 1
TARGET = "/home/models/Qwen/Qwen3.6-35B-A3B-w8a8"
# 2026-10-25 actual on-device G0 discovered this directory, despite the
# earlier user-provided path lacking /Qwen/. No silent fallback is allowed.
DRAFT = "/home/models/Qwen/Qwen3.6-35B-A3B-DFlash"
GRAPH_SIZES = (8, 16, 32, 64, 80)
EXTRA = {
    "enable_flashcomm1": False,
    "enable_flashcomm2_parallel_size": 0,
    "enable_prefill_mc2": False,
    "enable_fused_mc2": 0,
    "ascend_compilation_config": {"fuse_norm_quant": False, "enable_npugraph_ex": False},
}
GUARD_VARS = ("MF_KFC_BYPASS", "MF310P_V2_NOWARMUP", "MF310P_V2_AR_ONLY_NOWARMUP")


def compact(obj) -> str:
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


def build_plan(*, arm: str, mode: str, phase: str, devices: str, port: int,
               store_port: int, orch_json: str | None, output_dir: Path) -> dict:
    if arm not in ("stock", "v2") or mode not in ("eager", "graph") or phase not in ("smoke", "full"):
        raise ValueError("unsupported arm/mode/phase")
    if len(devices.split(",")) != 2 or len(set(devices.split(","))) != 2:
        raise ValueError("TP=2 needs two distinct visible devices")
    if not all(s.strip().isdigit() for s in devices.split(",")):
        raise ValueError("devices must be two explicit integer IDs")
    if not (1 <= port <= 65535 and 1 <= store_port <= 65535 and port != store_port):
        raise ValueError("port and MemFabric store port must be different valid ports")
    if arm == "v2" and not (orch_json and Path(orch_json).is_absolute() and orch_json.endswith("libmf_sdma_orch_v11.json")):
        raise ValueError("V2 needs verified absolute KVER11 JSON path, never auto-select installed files")
    if not output_dir.is_absolute():
        raise ValueError("output directory must be absolute and unused")
    # Match the owner's speculative-config JSON exactly. For the Qwen3 DFlash
    # draft, vLLM v0.24.0 resolves an omitted draft TP to target TP=2.
    # G0 must still verify the effective runtime draft TP from worker logs.
    spec = {"method": "dflash", "model": DRAFT, "num_speculative_tokens": SPEC_TOKENS}
    env = {
        "ASCEND_RT_VISIBLE_DEVICES": devices,
        "VLLM_WORKER_MULTIPROC_METHOD": "spawn",
        "HF_HUB_OFFLINE": "1",
        "VLLM_LOGGING_LEVEL": "DEBUG",
        "VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR": "1" if arm == "v2" else "0",
        "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE": "1" if arm == "v2" else "0",
    }
    if arm == "v2":
        env.update({
            "VLLM_ASCEND_310P_MEMFABRIC_STORE_URL": f"tcp://127.0.0.1:{store_port}",
            "VLLM_ASCEND_310P_MEMFABRIC_LOCAL_BYTES": str(96 * 1024 * 1024),
            "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_BATCH_BASEM_COUNT": "1",
            "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_WARMUP_FALLBACK": "1",
            "VLLM_ASCEND_310P_MEMFABRIC_V2_CHUNK_ROWS": "1024",
            "VLLM_ASCEND_310P_MEMFABRIC_V2_OUTPUT_SLOTS": "4",
            "MF_SDMA_ORCH_JSON": orch_json,
        })
    compilation = {"cudagraph_mode": "NONE"} if mode == "eager" else {
        "cudagraph_mode": "FULL_DECODE_ONLY",
        "cudagraph_capture_sizes": list(GRAPH_SIZES),
        "cudagraph_num_of_warmups": 0,
    }
    server = [
        "vllm", "serve", TARGET, "--host", "0.0.0.0", "--port", str(port),
        "--served-model-name", "qwen3.6-35b-a3b-w8a8", "--dtype", "float16",
        "--tensor-parallel-size", "2", "--quantization", "ascend",
        "--gpu-memory-utilization", "0.90", "--max-num-seqs", "16",
        "--max-num-batched-tokens", "4096", "--max-model-len", "8192",
        "--trust-remote-code", "--no-enable-prefix-caching", "--enable-chunked-prefill",
        "--async-scheduling",
        "--mamba-ssm-cache-dtype", "float16", "--mamba-cache-mode", "align",
        "--speculative-config", compact(spec), "--additional-config", compact(EXTRA),
        "--compilation-config", compact(compilation),
    ]
    full = phase == "full"
    benchmark = [
        "vllm", "bench", "serve", "--backend", "vllm", "--endpoint", "/v1/completions",
        "--base-url", f"http://127.0.0.1:{port}", "--model", "qwen3.6-35b-a3b-w8a8",
        "--tokenizer", TARGET, "--dataset-name", "random",
        "--random-input-len", str(4096), "--random-output-len", str(2048 if full else 32),
        "--num-prompts", str(50 if full else 2),
        "--max-concurrency", str(10 if full else 1), "--ignore-eos",
        "--percentile-metrics", "ttft,tpot,itl", "--metric-percentiles", "50,90,99,99.9",
        "--save-result", "--save-detailed", "--result-dir", str(output_dir),
        "--result-filename", "bench.json",
    ]
    return {
        "contract": "R1_DFLASH_K7_TP2_STOCK_V2_20261025",
        "arm": arm, "mode": mode, "phase": phase, "TP": 2, "speculative": spec,
        "logical_verify_width_not_measured_physical_M": VERIFY_WIDTH,
        "graph_sizes": list(GRAPH_SIZES) if mode == "graph" else [],
        "expected_requests": 50 if full else 2, "output_dir": str(output_dir),
        "env_overrides": env, "server_command": server, "benchmark_command": benchmark,
        "kernel_evidence_contract": (
            "V2 route+first-call correctness+rank0/1 trace needed. Log-only is not Graph kernel proof."
        ),
        "status": "PREPARED_NOT_RUN",
    }


def classify(manifest: dict, server_log: str, benchmark: dict) -> dict:
    errors = re.findall(r"507014|507899|first fused call verification FAILED|bit-exact mismatch",
                        server_log, flags=re.IGNORECASE)
    expected = manifest["expected_requests"]
    completed = benchmark.get("completed")
    # vLLM's real benchmark json must expose completed. Missing is UNKNOWN, not PASS.
    matched = isinstance(completed, int) and completed == expected
    seen_fused = "[mf310p-v2]" in server_log and "V2 MM+MemFabric" in server_log
    first_verified = "first fused call verified bit-exact" in server_log
    selected = "MemFabricMmArV2LinearMethod310" in server_log
    replay = bool(re.search(r"event=replay\b|/replay\]", server_log))
    ret = {"completed": completed, "expected": expected, "completed_ok": matched,
           "v2_route_log": seen_fused, "v2_first_call_verified": first_verified,
           "v2_linear_method_selected": selected, "graph_replay_log_observed": replay,
           "error_signatures": errors,
           "actual_npu_fused_kernel_trace": "NOT_PROVIDED",
           "real_physical_M_distribution": "NOT_PROVIDED"}
    if errors or not matched:
        ret["verdict"] = "FAIL_OR_INCOMPLETE_PRESERVE_EVIDENCE"
    elif manifest["arm"] == "stock":
        ret["verdict"] = ("INVALID_STOCK_CONTAMINATED" if (seen_fused or selected)
                          else "STOCK_SERVING_ONLY_PASS_GRAPH_UNVERIFIED"
                          if manifest["mode"] == "graph"
                          else "STOCK_EAGER_SERVING_PASS")
    elif not (seen_fused and selected and first_verified):
        ret["verdict"] = "V2_NOT_PROVEN_STOCK_FALLBACK_POSSIBLE"
    elif manifest["mode"] == "graph":
        ret["verdict"] = "V2_PREFILL_HINT_ONLY_GRAPH_FUSED_REPLAY_NOT_PROVEN"
    else:
        ret["verdict"] = "V2_PREFILL_ROUTE_HINT_ONLY_REQUIRE_REAL_KERNEL_TRACE"
    return ret


def clean_environment(override: dict[str, str]) -> dict[str, str]:
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("VLLM_ASCEND_310P_MEMFABRIC") or (
                key == "VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR") or key.startswith("MF_"):
            env.pop(key)
    env.update(override)
    return env


def wait_for_health(port: int, proc: subprocess.Popen, seconds: int) -> None:
    deadline = time.monotonic() + seconds
    url = f"http://127.0.0.1:{port}/health"
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"service exited before health, rc={proc.returncode}")
        try:
            with urllib.request.urlopen(url, timeout=2) as response:
                if response.status == 200:
                    return
        except OSError:
            pass
        time.sleep(3)
    raise TimeoutError("health check deadline reached")


def stop_own_process_group(proc: subprocess.Popen) -> None:
    # Never target other jobs: only the process group explicitly created below.
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait(timeout=10)


def validate_readonly_preflight(report: dict) -> None:
    if report.get("mode") != "STOCK_DFLASH_G0_READ_ONLY":
        raise RuntimeError("wrong G0 diagnostic report")
    v = report.get("verdict", {})
    if not v.get("static_go") or v.get("blockers"):
        raise RuntimeError("Stock+DFlash dependency G0 not passed; preserve diagnostic and STOP")
    pip_check = report.get("probes", {}).get("pip_check", {})
    if pip_check.get("classification") not in ("PASS", "KNOWN_SERVER_STATE_WAIVED"):
        raise RuntimeError("pip check contains unexpected or unreadable dependency failure")
    if pip_check.get("unexpected_lines"):
        raise RuntimeError("unexpected pip dependencies remain, preserve full pip check")
    if report.get("num_speculative_tokens") != 7:
        raise RuntimeError("wrong G0 K configuration")
    # Do not accept a static-only G0 to authorize the expensive model smoke.
    # This probe uses the verified actual serve import order, not cold Runner.
    if report.get("probes", {}).get("isolated_import_probe", {}).get("exit_code") != 0:
        raise RuntimeError("real serve-order import smoke not passed; G0 STOP")
    paths = report.get("model_paths") or {}
    if (paths.get("target") != TARGET or paths.get("draft") != DRAFT):
        raise RuntimeError("preflight model paths differ from the frozen trial contract")



def vllm_ascend_outside_this_worktree(tool_file: str, package_root: Path | None) -> bool:
    """Prevent Python editable installs from silently resolving another worktree."""
    if package_root is None:
        return True
    worktree = Path(tool_file).resolve().parents[2]
    return worktree not in package_root.resolve().parents


def resolve_vllm_ascend_root() -> Path | None:
    try:
        import importlib.util

        spec = importlib.util.find_spec("vllm_ascend")
    except (ImportError, AttributeError, ValueError):
        return None
    if spec is None:
        return None
    places = list(spec.submodule_search_locations or [])
    if places:
        return Path(places[0])
    return Path(spec.origin).parent if spec.origin else None


def missing_v2_artifacts(worktree: Path) -> list[str]:
    """Require build outputs inside the isolated code line, not another tree."""
    package = worktree / "vllm_ascend"
    expected = ("libmf310p_v2_trial_device.so", "libmf310p_device.so")
    missing = [name for name in expected if not (package / name).is_file()]
    if not list(package.glob("vllm_ascend_C*.so")):
        missing.append("vllm_ascend_C*.so")
    return missing



def console_script_matches_python(script_path: str, python_executable: str) -> bool:
    """A trusted vllm console script must not use another Python environment."""
    try:
        with Path(script_path).open("rb") as fh:
            first = fh.readline().decode("utf-8").strip()
    except (OSError, UnicodeError):
        return False
    if not first.startswith("#!"):
        return False
    shebang = first[2:].split()
    if not shebang:
        return False
    if Path(shebang[0]).name == "env":
        # Accept /usr/bin/env python3 only when that interpreter on PATH
        # resolves to the exact same Python executing this tool.
        if len(shebang) != 2 or shebang[1] not in ("python", "python3"):
            return False
        target = shutil.which(shebang[1])
    else:
        target = shebang[0]
    try:
        return bool(target) and Path(target).samefile(python_executable)
    except (OSError, ValueError):
        return False


def validate_runtime_identity(report: dict, module_root: Path | None,
                              vllm_cmd: str | None) -> None:
    """Bind G0 to actual Python, installed distributions, import paths, CLI."""
    if report.get("python") != sys.executable:
        raise RuntimeError("G0 Python executable changed since preflight")
    if vllm_cmd is None or not console_script_matches_python(vllm_cmd, sys.executable):
        raise RuntimeError("vllm executable does not use the G0 Python; STOP")
    expected = (report.get("package_roots") or {}).get("vllm_ascend")
    if not expected or module_root is None or Path(expected).resolve() != module_root.resolve():
        raise RuntimeError("vllm_ascend import root changed since G0; STOP")
    import importlib.metadata as metadata
    import importlib.util

    for package in ("vllm", "torch", "torch-npu", "transformers"):
        wanted = (report.get("versions") or {}).get(package)
        try:
            present = metadata.version(package)
        except metadata.PackageNotFoundError:
            present = None
        if not wanted or present != wanted:
            raise RuntimeError("distribution changed since G0: " + package)
    vllm_root = (report.get("package_roots") or {}).get("vllm")
    try:
        spec = importlib.util.find_spec("vllm")
    except (ImportError, ValueError, AttributeError):
        spec = None
    locations = list(spec.submodule_search_locations or []) if spec else []
    if not vllm_root or not locations or Path(vllm_root).resolve() != Path(locations[0]).resolve():
        raise RuntimeError("vllm source root changed since G0; STOP")


def execute(manifest: dict, approved: bool, exclusive: bool, restore: bool,
            graph_authorized: bool = False, eager_reviewed: bool = False,
            readonly_preflight: dict | None = None) -> int:
    if not (approved and exclusive and restore):
        raise RuntimeError("separate G0, device exclusive, backup/restore evidence needed")
    validate_readonly_preflight(readonly_preflight or {})
    if manifest.get("mode") == "graph" and not (graph_authorized and eager_reviewed):
        raise RuntimeError("Graph research is gated: review BOTH Eager baselines and authorize it explicitly")
    if manifest.get("contract") != "R1_DFLASH_K7_TP2_STOCK_V2_20261025":
        raise ValueError("unrecognized manifest")
    directory = Path(manifest["output_dir"])
    for entry in ("server.log", "bench.stdout.log", "run.outcome.json"):
        if (directory / entry).exists():
            raise FileExistsError("refuse to reuse previously executed trial: " + entry)
    if not (Path(TARGET) / "config.json").is_file() or not (Path(DRAFT) / "config.json").is_file():
        raise RuntimeError("target or DFlash draft model config missing")
    if manifest["arm"] == "v2":
        orchard = manifest["env_overrides"].get("MF_SDMA_ORCH_JSON", "")
        if not Path(orchard).is_file():
            raise RuntimeError("KVER11 file unavailable; G0 NO-GO")
        if any(os.environ.get(k) for k in GUARD_VARS):
            raise RuntimeError("unsafe MF first-write/KFC bypass found")
    cli_path = shutil.which("vllm")
    if cli_path is None:
        raise RuntimeError("vllm executable not found in current Python environment")
    module_root = resolve_vllm_ascend_root()
    validate_runtime_identity(readonly_preflight or {}, module_root, cli_path)
    if vllm_ascend_outside_this_worktree(__file__, module_root):
        raise RuntimeError(
            "vllm_ascend resolves to another worktree; prepend the isolated "
            "worktree to PYTHONPATH (preserving existing CANN site-packages)")
    if manifest["arm"] == "v2":
        missing = missing_v2_artifacts(Path(__file__).resolve().parents[2])
        if missing:
            raise RuntimeError(
                "V2 artifacts missing in this worktree: " + ", ".join(missing)
                + "; build in isolated worktree or ask approval to copy "
                "matching SHA256 artifacts; never import another worktree SO")
    port = int(manifest["server_command"][manifest["server_command"].index("--port") + 1])
    with socket.socket() as sock:
        if sock.connect_ex(("127.0.0.1", port)) == 0:
            raise RuntimeError("port already in use; refuse to touch existing service")
    env = clean_environment(manifest["env_overrides"])
    failure = None
    with (directory / "server.log").open("x", encoding="utf-8") as server_log:
        proc = subprocess.Popen(manifest["server_command"], env=env, cwd=directory,
                                stdout=server_log, stderr=subprocess.STDOUT,
                                text=True, start_new_session=True)
        (directory / "server.pid").write_text(str(proc.pid), encoding="utf-8")
        try:
            wait_for_health(port, proc, 2400)
            with (directory / "bench.stdout.log").open("x", encoding="utf-8") as bench_log:
                subprocess.run(manifest["benchmark_command"], env=env, cwd=directory,
                               stdout=bench_log, stderr=subprocess.STDOUT,
                               timeout=7200, check=True, text=True)
        except (Exception, KeyboardInterrupt) as exc:
            failure = repr(exc)
        finally:
            stop_own_process_group(proc)
    outcome = {"execution_finished": failure is None, "error": failure,
               "benchmark_exists": (directory / "bench.json").is_file(),
               "note": "not a pass verdict; run verify separately and inspect both TP rank traces"}
    (directory / "run.outcome.json").write_text(compact(outcome) + "\n", encoding="utf-8")
    print(compact(outcome))
    return 1 if failure else 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="command", required=True)
    plan = sub.add_parser("plan")
    plan.add_argument("--arm", required=True, choices=("stock", "v2"))
    plan.add_argument("--mode", required=True, choices=("eager", "graph"))
    plan.add_argument("--phase", required=True, choices=("smoke", "full"))
    plan.add_argument("--devices", required=True)
    plan.add_argument("--port", type=int, required=True)
    plan.add_argument("--store-port", type=int, default=8581)
    plan.add_argument("--orch-json")
    plan.add_argument("--dir", required=True, type=Path)
    run = sub.add_parser("execute")
    run.add_argument("--manifest", type=Path, required=True)
    run.add_argument("--preflight", type=Path, required=True,
                     help="G0 readonly JSON from dflash_stock_preflight.py")
    run.add_argument("--g0-approved", action="store_true")
    run.add_argument("--exclusive-approved", action="store_true")
    run.add_argument("--restore-reviewed", action="store_true")
    run.add_argument("--graph-research-authorized", action="store_true")
    run.add_argument("--eager-baselines-reviewed", action="store_true")
    check = sub.add_parser("verify")
    check.add_argument("--manifest", type=Path, required=True)
    args = p.parse_args()
    if args.command == "plan":
        obj = build_plan(arm=args.arm, mode=args.mode, phase=args.phase,
                         devices=args.devices, port=args.port, store_port=args.store_port,
                         orch_json=args.orch_json, output_dir=args.dir)
        if args.dir.exists():
            raise FileExistsError("output directory already exists: " + str(args.dir))
        args.dir.mkdir(parents=True, exist_ok=False)
        (args.dir / "manifest.json").write_text(
            json.dumps(obj, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(str(args.dir / "manifest.json"))
        return 0
    obj = json.loads(args.manifest.read_text(encoding="utf-8"))
    if args.command == "execute":
        preflight = json.loads(args.preflight.read_text(encoding="utf-8"))
        return execute(obj, args.g0_approved, args.exclusive_approved, args.restore_reviewed,
                       args.graph_research_authorized, args.eager_baselines_reviewed, preflight)
    location = Path(obj["output_dir"])
    log = location / "server.log"
    bench = location / "bench.json"
    if not log.is_file() or not bench.is_file():
        raise FileNotFoundError("server.log and bench.json are both required; failed run remains evidence")
    assessment = classify(obj, log.read_text(encoding="utf-8", errors="replace"),
                          json.loads(bench.read_text(encoding="utf-8")))
    with (location / "verdict.json").open("x", encoding="utf-8") as fh:
        json.dump(assessment, fh, ensure_ascii=False, indent=2)
    print(json.dumps(assessment, ensure_ascii=False, indent=2))
    return 1 if assessment["verdict"].startswith(("FAIL", "INVALID", "V2_NOT")) else 0


if __name__ == "__main__":
    raise SystemExit(main())
