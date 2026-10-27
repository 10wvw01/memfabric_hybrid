#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Stock+DFlash environment audit; no model load, package install or NPU work by default."""
from __future__ import annotations

import argparse
import ast
import importlib.metadata as metadata
import importlib.util
import json
import os
import re
from pathlib import Path
import subprocess
import sys

# Server-verified working baseline of the 310P3 R1 host, established by the
# 2026-10-25 on-device G0 (see csrc/_310P/memfabric_mm_ar_v2/docs/
# R1_G0_ONDEVICE_NOGO_REPORT_20261025.md): /usr/local/python3.12.13 runs the
# official vllm-project/vllm@6e448d0 (v0.27.1, DFlash files in-tree) and every
# recent on-device serving run (F1-C, SEL-01, F1/F2, D4) executed on it. This
# code line is adapted to vLLM v0.26/0.27 (commit 1b6abadc8; FLA alias and
# cp_utils shims). The previous 0.24.0-era values came from the upstream
# requirements audit written without server access; repo requirements.txt
# still carries that stale metadata (pip check conflicts originate there).
PINNED = {
    "vllm": "0.27.1",
    "torch": "2.10.0",
    "torch-npu": "2.10.0.post4",
    "transformers": "5.17.0",
}
# Observed, not all mandatory to initialize Stock. Upstream requirements.txt
# lists these but the 310P Dockerfile explicitly UNINSTALLS triton-ascend.
OBSERVE = (
    "vllm-ascend", "arctic-inference", "numba", "regex",
    "compressed-tensors", "memfabric-hybrid", "memcache-hybrid", "triton-ascend",
)
API_FILES = {
    "vllm": (
        "model_executor/models/qwen3_dflash.py",
        "v1/spec_decode/dflash.py",
        "v1/worker/cp_utils.py",
    ),
    "vllm_ascend": (
        "_310p/model_runner_310p.py",
        "_310p/spec_decode/dflash_proposer_310.py",
        "patch/worker/patch_idex_310.py",
    ),
}
STOCK_DISABLED_FLAGS = (
    "VLLM_ASCEND_310P_ENABLE_MEMFABRIC_MM_AR",
    "VLLM_ASCEND_310P_MEMFABRIC_MM_AR_V2_SERVE",
)


def active(value: str | None) -> bool:
    return str(value or "").strip().lower() in {"1", "true", "yes", "on"}


def verdict(versions: dict[str, str | None], files: dict[str, bool],
            flags: dict[str, str | None], model_ready: dict[str, bool]) -> dict:
    blockers, observations = [], []
    for name, wanted in PINNED.items():
        found = versions.get(name)
        if found is None:
            blockers.append("MISSING_DISTRIBUTION:" + name)
        elif found != wanted and not found.startswith(wanted + "+"):
            blockers.append("PIN_MISMATCH:" + name + ":" + found + ":expected=" + wanted)
    for name, present in files.items():
        if not present:
            blockers.append("MISSING_SOURCE_API:" + name)
    for flag in STOCK_DISABLED_FLAGS:
        if active(flags.get(flag)):
            blockers.append("STOCK_CONTAMINATED_BY_MEMFABRIC:" + flag)
    for name, ready in model_ready.items():
        if not ready:
            blockers.append("MODEL_CONFIG_MISSING:" + name)
    for name in OBSERVE:
        if versions.get(name) is None:
            observations.append("UPSTREAM_DECLARED_OR_OPTIONAL_NOT_INSTALLED:" + name)
    return {"static_go": not blockers, "blockers": blockers, "observations": observations}


def package_root(name: str) -> tuple[str | None, Path | None]:
    try:
        spec = importlib.util.find_spec(name)
        if spec is None:
            return None, None
        places = list(spec.submodule_search_locations or [])
        if places:
            return str(places[0]), Path(places[0])
        if spec.origin:
            return str(spec.origin), Path(spec.origin).parent
    except (ImportError, AttributeError, ValueError) as exc:
        return "FIND_SPEC_ERROR: " + repr(exc), None
    return None, None


def distribution_versions() -> dict[str, str | None]:
    versions = {}
    for name in (*PINNED, *OBSERVE):
        try:
            versions[name] = metadata.version(name)
        except metadata.PackageNotFoundError:
            versions[name] = None
    return versions


def check_api(package_roots: dict[str, Path | None]) -> dict[str, bool]:
    result = {}
    for package, rels in API_FILES.items():
        root = package_roots.get(package)
        for rel in rels:
            result[package + "/" + rel] = bool(root and (root / rel).is_file())
    root = package_roots.get("vllm")
    ascend_root = package_roots.get("vllm_ascend")
    cp = root / "v1/worker/cp_utils.py" if root else None
    if cp and cp.is_file():
        native = False
        dcp_lane = False
        try:
            text = cp.read_text(encoding="utf-8")
            tree = ast.parse(text)
            native = any(
                isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name == "get_total_cp_world_size" for node in tree.body)
            # vLLM v0.26/v0.27 lane: the helper moved behind get_dcp_group.
            dcp_lane = "get_dcp_group" in text
        except (OSError, SyntaxError, UnicodeError):
            pass
        # The dcp lane only counts when our runner carries the compat shim.
        shim = False
        runner = ascend_root / "_310p/model_runner_310p.py" if ascend_root else None
        if runner and runner.is_file():
            try:
                shim_text = runner.read_text(encoding="utf-8")
                shim = ("get_total_cp_world_size" in shim_text
                        and "get_dcp_group" in shim_text)
            except (OSError, UnicodeError):
                pass
        result["vllm/cp_utils.get_total_cp_world_size"] = bool(native or (dcp_lane and shim))
    else:
        result["vllm/cp_utils.get_total_cp_world_size"] = False
    return result


# Packages that the verified 310P server intentionally does not install even
# though some distribution metadata declares them: upstream vllm-ascend
# declares optional extras (arctic-inference, memcache-hybrid, pandas,
# pandas-stubs, quart; the 310P image also removes triton/triton-ascend) and
# CANN host tooling (ms-service-profiler, mindstudio-kpp, affinity-sched)
# declares desktop analysis deps. Every recent successful serving run shared
# this exact state; absence is recorded as an observation, never a blocker.
KNOWN_OPTIONAL_MISSING_BY_REPORTER = {
    "vllm-ascend": {
        "arctic-inference", "memcache-hybrid", "pandas",
        "pandas-stubs", "quart", "triton-ascend",
    },
    "ms-service-profiler": {
        "matplotlib", "msguard", "openpyxl", "pandas", "tzdata",
    },
    "mindstudio-kpp": {"pandas", "plotly"},
    "affinity-sched": {"argparse"},
}
# Version conflicts produced only by stale editable metadata that still pins
# the vllm-0.24.0 era (repo requirements.txt) or by CANN tooling's own pinned
# telemetry stack. Runtime versions are the verified server baseline. This
# table must be re-audited whenever requirements.txt is refreshed.
KNOWN_STALE_METADATA_CONFLICTS = {
    ("vllm-ascend", "torch-npu==2.10.0.post2"): ("torch-npu", "2.10.0.post4"),
    ("vllm-ascend", "transformers==5.13.0"): ("transformers", "5.17.0"),
    ("vllm-ascend", "fastapi<0.124.0"): ("fastapi", "0.136.3"),
    ("ms-service-profiler", "opentelemetry-exporter-otlp-proto-grpc==1.33.1"):
        ("opentelemetry-exporter-otlp-proto-grpc", "1.44.0"),
    ("ms-service-profiler", "opentelemetry-exporter-otlp-proto-http==1.33.1"):
        ("opentelemetry-exporter-otlp-proto-http", "1.44.0"),
}
KNOWN_PLATFORM_NOTICES = {
    "te 0.4.0 is not supported on this platform",
}


def split_pip_check_lines(output: str) -> tuple[list[str], list[str]]:
    """Split pip check output into (waived known-state, unexpected) lines."""
    waived, unexpected = [], []
    for raw in output.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.lower() in KNOWN_PLATFORM_NOTICES:
            waived.append(line)
            continue
        # pip check prints "<name> <version> requires <pkg>, which is not installed."
        match = re.match(r"([A-Za-z0-9_.\-]+) \S+ requires ([A-Za-z0-9_.\-]+),"
                         r" which is not installed\.", line)
        if (match and match.group(2).lower() in
                KNOWN_OPTIONAL_MISSING_BY_REPORTER.get(match.group(1).lower(), set())):
            waived.append(line)
            continue
        # pip check prints "<name> <version> has requirement <req>, but you have ..."
        match = re.fullmatch(r"([A-Za-z0-9_.\-]+) \S+ has requirement ([^,]+),"
                             r" but you have ([A-Za-z0-9_.\-]+) ([A-Za-z0-9_.+\-]+)\.", line)
        if (match and KNOWN_STALE_METADATA_CONFLICTS.get(
                (match.group(1).lower(), match.group(2))) ==
                (match.group(3).lower(), match.group(4))):
            waived.append(line)
            continue
        unexpected.append(line)
    return waived, unexpected


def classify_pip_check(exit_code: int | None, output: str) -> str:
    """Distinguish the verified server's known pip state from real breakage.

    Only the exact known-state lines above are waived: any other missing
    package, any version conflict (including triton conflicts), or any
    unreadable output remains a hard blocker.
    """
    if exit_code == 0:
        return "PASS"
    if exit_code is None:
        return "UNREADABLE"
    waived, unexpected = split_pip_check_lines(output)
    # exit!=0 with no parseable diagnostic is NOT evidence of a healthy env.
    if not output.strip():
        return "UNREADABLE"
    if not unexpected:
        return "KNOWN_SERVER_STATE_WAIVED" if waived else "UNREADABLE"
    return "UNEXPECTED_DEPENDENCY_BREAKAGE"


# Import probe that mirrors the REAL vllm serve order, established by the
# 2026-10-25 on-device G0 forensics: plugin activation happens at the first
# vllm import; NPUPlatform.pre_register_and_update then runs adapt_patch
# (FLA alias + full vllm_ascend.ops import) BEFORE the executor imports the
# worker/model_runner. Importing model_runner_310p in any colder order hits a
# device_op -> ops -> fused_moe -> experts_selector -> device_op cycle and a
# missing FLA alias; those are probe artifacts, not serve defects.
PROBE_IMPORTS = "\n".join([
    "from vllm.model_executor.models.qwen3_dflash import DFlashQwen3ForCausalLM",
    "from vllm.v1.spec_decode.dflash import DFlashProposer",
    "import vllm_ascend.platform",
    "vllm_ascend.platform.NPUPlatform.pre_register_and_update(None)",
    "import vllm_ascend._310p.model_runner_310p",
    "import vllm_ascend.patch.worker.patch_idex_310",
])


def optional_probes(enable_imports: bool) -> dict:
    cmd = [sys.executable, "-m", "pip", "check"]
    try:
        p = subprocess.run(cmd, text=True, capture_output=True, timeout=35, check=False)
        # Classify FULL pip output; trimming can hide a real conflicting line.
        output = {"exit_code": p.returncode, "output": p.stdout + p.stderr}
        output["classification"] = classify_pip_check(output["exit_code"], output["output"])
        waived, unexpected = split_pip_check_lines(output["output"])
        output["waived_known_state_lines"] = waived
        output["unexpected_lines"] = unexpected
    except (OSError, subprocess.TimeoutExpired) as exc:
        output = {"error": repr(exc), "classification": "UNREADABLE"}
    result = {"pip_check": output}
    if enable_imports:
        # Runs in a CHILD process only. This may load torch-npu native libraries;
        # do not enable until the server's read-only G0 environment check is safe.
        try:
            p = subprocess.run([sys.executable, "-c", PROBE_IMPORTS], capture_output=True,
                               text=True, timeout=90, check=False)
            result["isolated_import_probe"] = {
                "exit_code": p.returncode, "traceback_tail": (p.stderr + p.stdout)[-12000:]
            }
        except (OSError, subprocess.TimeoutExpired) as exc:
            result["isolated_import_probe"] = {"error": repr(exc)}
    else:
        result["isolated_import_probe"] = {"status": "NOT_RUN"}
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", default="/home/models/Qwen/Qwen3.6-35B-A3B-w8a8")
    parser.add_argument("--draft", default="/home/models/Qwen/Qwen3.6-35B-A3B-DFlash")
    parser.add_argument("--num-speculative-tokens", type=int, default=7)
    parser.add_argument("--import-smoke", action="store_true",
                        help="opt-in child-process imports; only after G0")
    parser.add_argument("--out", type=Path, help="new output JSON file (never overwrite)")
    args = parser.parse_args()
    if args.num_speculative_tokens != 7:
        parser.error("current project Stock+DFlash baseline freezes num_speculative_tokens=7")
    modules = {}
    roots = {}
    for name in API_FILES:
        origin, root = package_root(name)
        modules[name] = origin
        roots[name] = root
    versions = distribution_versions()
    files = check_api(roots)
    flags = {name: os.environ.get(name) for name in STOCK_DISABLED_FLAGS}
    models = {"target": (Path(args.target) / "config.json").is_file(),
              "draft": (Path(args.draft) / "config.json").is_file()}
    decision = verdict(versions, files, flags, models)
    probes = optional_probes(args.import_smoke)
    result = {
        "mode": "STOCK_DFLASH_G0_READ_ONLY",
        "num_speculative_tokens": args.num_speculative_tokens,
        "logical_verify_width_example": args.num_speculative_tokens + 1,
        "note": "logical width is not measured physical/padded M",
        "python": sys.executable, "python_version": sys.version.split()[0],
        "cwd": os.getcwd(), "pythonpath": os.environ.get("PYTHONPATH"),
        "sys_path": sys.path, "package_roots": modules,
        "model_paths": {"target": str(Path(args.target).resolve()),
                        "draft": str(Path(args.draft).resolve())},
        "versions": versions,
        "api_files": files, "memfabric_flags": flags, "model_config_present": models,
        "verdict": decision, "probes": probes,
        "limitations": "No vllm serve, CANN/NPU kernel, Graph, or actual model inference executed",
    }
    # The verified 310P server intentionally omits several declared optional
    # packages and carries stale editable metadata (see classify_pip_check);
    # only those exact known-state lines are waived, everything else blocks.
    pip_state = probes.get("pip_check", {}).get("classification")
    if pip_state not in ("PASS", "KNOWN_SERVER_STATE_WAIVED"):
        decision["static_go"] = False
        decision["blockers"].append("PIP_CHECK_UNEXPECTED_FAILURE_OR_UNREADABLE")
    elif pip_state != "PASS":
        decision["observations"].append(
            "WAIVED_KNOWN_SERVER_STATE:" + "|".join(
                probes["pip_check"].get("waived_known_state_lines", [])))
    smoke = probes.get("isolated_import_probe", {})
    if args.import_smoke and smoke.get("exit_code", 1) != 0:
        decision["static_go"] = False
        decision["blockers"].append("ISOLATED_IMPORT_FAILED_OR_TIMED_OUT")
    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open("x", encoding="utf-8") as handle:
            handle.write(rendered + "\n")
    print(rendered)
    return 0 if decision["static_go"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
