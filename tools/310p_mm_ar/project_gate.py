#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""310P MM+AR project-control contract checker; standard library only.

Static validation is not CANN compilation, NPU correctness, or a release gate.
"""
from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROJECT = "csrc/_310P/memfabric_mm_ar_v2/"
DOCS = PROJECT + "docs/"
CONTROL = DOCS + "PROJECT_CONTROL.md"
PERF = DOCS + "PERFORMANCE_BASELINES.md"
DECISIONS = DOCS + "DECISIONS.md"

REQUIRED = (
    "PROJECT_310P_MEMFABRIC.md", "AGENTS.md",
    PROJECT + "AGENTS.md", CONTROL, PERF, DECISIONS,
    DOCS + "BRANCH_GOVERNANCE.md",
    PROJECT + "src/op_kernel/mf310p_v2_kernel.asc",
    "vllm_ascend/_310p/ops/memfabric_mm_ar_v2_serve.py",
)
ENTRY_LINKS = (
    CONTROL, PERF, DECISIONS, DOCS + "BRANCH_GOVERNANCE.md",
    PROJECT + "AGENTS.md",
)


def check_static(root: Path) -> list[str]:
    errors = [f"missing: {path}" for path in REQUIRED if not (root / path).is_file()]
    entry = root / "PROJECT_310P_MEMFABRIC.md"
    if entry.is_file():
        text = entry.read_text(encoding="utf-8")
        if "dev/mainline" not in text:
            errors.append("project entry missing canonical development branch")
        for path in ENTRY_LINKS:
            if path not in text:
                errors.append(f"project entry missing path link: {path}")
    if (root / CONTROL).is_file():
        content = (root / CONTROL).read_text(encoding="utf-8")
        for term in ("Stock", "4096", "2048", "TP", "Gate E"):
            if term not in content:
                errors.append(f"project control missing required contract marker: {term}")
    return errors


def check_changes(paths: list[str]) -> list[str]:
    changed = {path.replace("\\", "/").removeprefix("./") for path in paths if path}
    if not changed:
        return []
    formal_prefixes = (
        PROJECT + "src/",
        "vllm_ascend/_310p/ops/memfabric_mm_ar",
        "vllm_ascend/_310p/quantization/",
    )
    formal_exact = {
        "vllm_ascend/_310p/model_runner_310p.py",
        "vllm_ascend/envs.py",
        "cmake/memfabric_310p_v2.cmake",
    }
    affected_code = any(p.startswith(formal_prefixes) or p in formal_exact for p in changed)
    affected_trials = any(p.startswith(PROJECT + "test/") for p in changed)
    affected_evidence = any(p.startswith(PROJECT + "evidence/") for p in changed)
    performance_reports = any(
        p.startswith(DOCS) and ("REPORT" in Path(p).name.upper() or "PERF" in Path(p).name.upper())
        and p not in {CONTROL, PERF, DECISIONS}
        for p in changed
    )
    errs = []
    if (affected_code or affected_trials or affected_evidence or DECISIONS in changed or PERF in changed) and CONTROL not in changed:
        errs.append("project source/test/evidence/decision changed without PROJECT_CONTROL.md update")
    if (affected_evidence or performance_reports) and PERF not in changed:
        errs.append("performance evidence/report changed without PERFORMANCE_BASELINES.md update")
    # Human review still has to validate real routing, device/SO/KVER provenance,
    # regression coverage and whether a change needs a new ADR.
    return errs


def git_changed_paths(root: Path, base: str, head: str) -> list[str]:
    for name, sha in (("base", base), ("head", head)):
        if not re.fullmatch(r"[0-9a-fA-F]{7,40}", sha):
            raise ValueError(f"invalid {name} SHA")
    completed = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=ACMRT", base, head],
        cwd=root, capture_output=True, text=True, check=True,
    )
    return completed.stdout.splitlines()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--static", action="store_true", help="check entry and source-of-truth files")
    parser.add_argument("--base", help="base commit SHA of PR/push")
    parser.add_argument("--head", help="head commit SHA of PR/push")
    args = parser.parse_args()
    if bool(args.base) != bool(args.head):
        parser.error("--base and --head must be given together")
    if not (args.static or args.base):
        parser.error("specify --static and/or --base/--head")
    errors = check_static(ROOT) if args.static else []
    if args.base:
        errors.extend(check_changes(git_changed_paths(ROOT, args.base, args.head)))
    for error in errors:
        print(f"ERROR: {error}")
    if errors:
        return 1
    print("PASS: project metadata contract (NOT an NPU/Serving verification)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
