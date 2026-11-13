"""Record source revisions and verify that tests load the corresponding artifacts."""
import argparse
import hashlib
import importlib
import json
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def git(directory, *args):
    return subprocess.check_output(["git", "-C", str(directory), *args], text=True).strip()


def digest(path):
    with Path(path).open("rb") as stream:
        result = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
        return result.hexdigest()


def source_record(directory):
    path = Path(directory).resolve()
    dirty = git(path, "status", "--porcelain", "--untracked-files=no", "--ignore-submodules=all")
    if dirty:
        raise RuntimeError(f"Uncommitted source changes: {path}")
    return {
        "path": str(path),
        "branch": git(path, "branch", "--show-current"),
        "commit": git(path, "rev-parse", "HEAD"),
    }


def sources(args):
    cache = Path(args.memcache_dir).resolve()
    records = {"memcache": source_record(cache), "memfabric": source_record(cache / "3rdparty/memfabric_hybrid")}
    records["memcache"]["memfabric_gitlink"] = git(cache, "rev-parse", "HEAD:3rdparty/memfabric_hybrid")
    for name, directory in [("vllm_ascend", args.vllm_ascend_dir), ("vllm", args.vllm_dir)]:
        if directory:
            records[name] = source_record(directory)
    branches = {"memcache": "trunk_for_escc_v1.2.1", "memfabric": "trunk_for_escc_v1.2.0",
                "vllm_ascend": "trunk_for_escc_v24.0"}
    for name, branch in branches.items():
        if name in records and records[name]["branch"] != branch:
            raise RuntimeError(f"Unexpected branch for {name}: {records[name]['branch']}, expected {branch}")
    return records


def artifacts(args):
    cache = Path(args.memcache_dir).resolve()
    fabric = cache / "3rdparty/memfabric_hybrid"
    paths = {
        "libmf_hybm_core.so": fabric / "output/hybm/lib64/libmf_hybm_core.so",
        "libmf_smem.so": fabric / "output/smem/lib64/libmf_smem.so",
        "libmf_acc_offload.so": fabric / "output/acc_offload/lib64/libmf_acc_offload.so",
        "mmc_meta_service": cache / "output/memcache/bin/mmc_meta_service",
    }
    wheels = sorted((cache / "output/memcache/wheel").glob("memcache_hybrid-*.whl"))
    if len(wheels) == 1:
        paths["memcache_wheel"] = wheels[0]
    return {name: {"path": str(path), "sha256": digest(path)} for name, path in paths.items() if path.is_file()}


def verify_mapped_libraries(built):
    paths = {Path(line.split()[-1]) for line in Path("/proc/self/maps").read_text().splitlines()}
    for path in sorted(paths):
        if path.name in built and path.is_file():
            if digest(path) != built[path.name]["sha256"]:
                raise RuntimeError(f"Loaded runtime library differs from build: {path}")
            print(f"[LOADED] {path}")


def verify_runtime(records, built):
    for name in ["memcache_hybrid", "memfabric_hybrid"]:
        module = importlib.import_module(name)
        print(f"[IMPORT] {name}={module.__file__}")
        if name == "memcache_hybrid":
            if "memcache_wheel" not in built:
                raise RuntimeError("Expected exactly one MemCache build wheel")
            root = Path(module.__file__).parent.parent
            with zipfile.ZipFile(built["memcache_wheel"]["path"]) as wheel:
                for filename in wheel.namelist():
                    if filename.startswith("memcache_hybrid/") and filename.endswith(".so"):
                        expected = hashlib.sha256(wheel.read(filename)).hexdigest()
                        if digest(root / filename) != expected:
                            raise RuntimeError(f"Installed MemCache library differs from wheel: {filename}")
        if name == "memfabric_hybrid":
            lib = Path(module.__file__).parent / "lib"
            for filename in ["libmf_hybm_core.so", "libmf_smem.so", "libmf_acc_offload.so"]:
                if filename not in built or digest(lib / filename) != built[filename]["sha256"]:
                    raise RuntimeError(f"Installed library differs from build: {lib / filename}")
    for name in ["vllm", "vllm_ascend"]:
        if name not in records:
            continue
        module = importlib.import_module(name)
        expected = Path(records[name]["path"])
        actual = Path(module.__file__).resolve()
        print(f"[IMPORT] {name}={actual}")
        if not actual.is_relative_to(expected):
            raise RuntimeError(f"{name} loaded from {actual}, expected source {expected}")
        if name == "vllm" and not module.__version__.startswith("0.24."):
            raise RuntimeError(f"Expected vLLM 0.24, got {module.__version__}")
    verify_mapped_libraries(built)
    if "mmc_meta_service" not in built:
        raise RuntimeError("MetaService build artifact is missing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["write", "verify"])
    parser.add_argument("--memcache-dir", required=True)
    parser.add_argument("--vllm-ascend-dir", default="")
    parser.add_argument("--vllm-dir", default="")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--sources-only", action="store_true")
    parser.add_argument("--require-vllm", action="store_true")
    args = parser.parse_args()
    if args.output is None:
        filename = "source_revisions.json" if args.sources_only else "source_manifest.json"
        args.output = ROOT / "retest_logs" / filename
    previous = None
    if args.action == "verify":
        previous = json.loads(args.output.read_text())
        if not args.vllm_dir:
            args.vllm_dir = previous["sources"].get("vllm", {}).get("path", "")
        if not args.vllm_ascend_dir:
            args.vllm_ascend_dir = previous["sources"].get("vllm_ascend", {}).get("path", "")
    records = sources(args)
    built = {} if args.sources_only else artifacts(args)
    if args.require_vllm and not {"vllm", "vllm_ascend"}.issubset(records):
        raise RuntimeError("The deployment record must include vLLM and vLLM-Ascend")
    data = {"sources": records, "artifacts": built}
    if args.action == "write":
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(data, indent=2) + "\n")
        print(f"[MANIFEST] {args.output}")
    else:
        if data != previous:
            raise RuntimeError("Source revisions or artifacts changed since setup; run setup again")
        verify_runtime(records, built)
        print("[PASS] source revisions, package paths and MemFabric libraries match")
    for name, record in records.items():
        print(f"[SOURCE] {name} {record['branch']} {record['commit']}")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        sys.exit(1)
