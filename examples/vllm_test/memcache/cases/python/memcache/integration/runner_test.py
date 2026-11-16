from pathlib import Path
import subprocess

import pytest

from integration.vllm_integration_test import VLLMBenchmark
from integration.large_capacity_vllm_test import VLLMTTFTBenchmark


@pytest.mark.parametrize("with_memcache", [False, True])
@pytest.mark.parametrize("benchmark_class", [VLLMBenchmark, VLLMTTFTBenchmark])
def test_model_commands_have_bounded_cache(monkeypatch, with_memcache, benchmark_class):
    monkeypatch.setenv("MMC_TEST_KV_CACHE_MEMORY_BYTES", "1073741824")
    bench = benchmark_class("model", "name", 4077, "0,1,2,3", 4)
    command = bench._build_serve_cmd(with_memcache)
    assert command[command.index("--kv-cache-memory-bytes") + 1] == "1073741824"
    assert ("--kv-transfer-config" in command) == with_memcache
    assert "--prefix-match-unit" not in command
    assert "--enable-prefix-caching" in command


def _runner_function(name):
    source = (Path(__file__).resolve().parents[4] / "run_test.sh").read_text()
    start = source.index(name + "() {")
    return source[start : source.index("\n}\n", start) + 3]


def test_largecap_preserves_failure_after_cleanup():
    script = """
run_pytest() { return 7; }
ensure_meta_service() { :; }
python3() { return 0; }
pkill() { return 0; }
log() { :; }
TEST_FILTER=rank4
"""
    script += _runner_function("run_largecap_integration")
    script += "\nrun_largecap_integration || exit $?\n"
    result = subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=10)
    assert result.returncode == 7, result.stdout + result.stderr


def test_benchmark_passes_selected_config(tmp_path):
    args = tmp_path / "args"
    script = """
bash() { printf '%s\\n' "$@" > "$ARGS"; return 9; }
log() { :; }
MMC_TEST_TENSOR_PARALLEL_SIZE=4
MMC_LOCAL_CONFIG_PATH=/selected/four-rank.conf
"""
    script += _runner_function("run_bench")
    script += '\nrun_bench || exit $?\n'
    import os

    result = subprocess.run(
        ["bash", "-c", script],
        env={**os.environ, "ARGS": str(args)},
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 9, result.stdout + result.stderr
    values = args.read_text().splitlines()
    assert values[values.index("--config") + 1] == "/selected/four-rank.conf"


def test_service_start_requires_stable_idle_memory(monkeypatch):
    from types import SimpleNamespace
    from unittest.mock import Mock

    bench = VLLMBenchmark("model", "name", 4077, "0", 1)
    memory_rows = [10, 10, 80, 10, 10, 10]
    probe = Mock(side_effect=[SimpleNamespace(stdout=f"| 0 0 | bus | 0 {used} / 100 |\n") for used in memory_rows])
    monkeypatch.setattr("integration.vllm_integration_test.subprocess.run", probe)
    monkeypatch.setattr("integration.vllm_integration_test.time.sleep", lambda _: None)
    bench._wait_for_device_memory()
    assert probe.call_count == 6


@pytest.mark.parametrize("tp,suffix", [(1, "-1rank"), (2, ""), (4, "-4rank")])
def test_service_selects_config_for_tp(tp, suffix):
    import os

    root = Path(__file__).resolve().parents[4]
    source = (root / "start_service.sh").read_text().split("# Environment setup")[0]
    source += '\nprintf "%s" "$LOCAL_CONFIG"\n'
    env = dict(os.environ)
    env.pop("MMC_LOCAL_CONFIG_PATH", None)
    result = subprocess.run(
        ["bash", "-c", source, "start_service.sh", "--tp", str(tp)],
        cwd=root,
        env=env,
        text=True,
        capture_output=True,
        timeout=10,
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.endswith(f"mmc-local-integration{suffix}.conf")


def test_service_rejects_mismatched_config_before_start():
    import os

    root = Path(__file__).resolve().parents[4]
    source = (root / "start_service.sh").read_text().split("# Environment setup")[0]
    config = root / "cases/config/mmc-local-integration.conf"
    result = subprocess.run(
        ["bash", "-c", source, "start_service.sh", "--tp", "4", "--config", str(config)],
        cwd=root,
        env={**os.environ, "MMC_LOCAL_CONFIG_PATH": ""},
        text=True,
        capture_output=True,
        timeout=10,
    )
    assert result.returncode == 1
    assert "world_size=2" in result.stdout


def test_largecap_full_miss_uses_separate_warmup_and_checks_metrics():
    from unittest.mock import Mock

    bench = VLLMTTFTBenchmark.__new__(VLLMTTFTBenchmark)
    bench.measure_e2e_ttft_ms = Mock(return_value=1.0)
    bench.measure_request_with_cache_source = Mock(return_value=2.0)
    result = bench.measure_ttft_batch(num_requests=3, prompt_tokens=32, warmup=2)
    warmup = {tuple(call.args[0]) for call in bench.measure_e2e_ttft_ms.call_args_list}
    measured = {tuple(call.args[0]) for call in bench.measure_request_with_cache_source.call_args_list}
    assert len(warmup) == 2
    assert len(measured) == 3
    assert warmup.isdisjoint(measured)
    assert all(call.args[1] is False for call in bench.measure_request_with_cache_source.call_args_list)
    assert result["count"] == 3


def test_prefix_reset_retries_when_engine_has_not_freed_blocks(tmp_path, monkeypatch):
    from unittest.mock import Mock

    log = tmp_path / "service.log"
    log.write_text("Successfully reset prefix cache\n")
    bench = VLLMBenchmark.__new__(VLLMBenchmark)
    bench._port, bench._log_path = 1, str(log)
    connection = Mock()
    connection.getresponse.return_value.status = 200
    count = 0

    def reset(*_args):
        nonlocal count
        count += 1
        with log.open("a") as output:
            output.write(
                "Failed to reset: blocks are not freed\n" if count == 1 else "Successfully reset prefix cache\n"
            )

    connection.request.side_effect = reset
    monkeypatch.setattr("integration.vllm_integration_test.http.client.HTTPConnection", lambda *_a, **_kw: connection)
    monkeypatch.setattr("integration.vllm_integration_test.time.sleep", lambda _: None)
    bench.reset_local_prefix_cache()
    assert connection.request.call_count == 2
    assert connection.close.call_count == 2


def test_prefix_reset_rejects_http_success_without_engine_confirmation(tmp_path, monkeypatch):
    from unittest.mock import Mock

    log = tmp_path / "service.log"
    log.write_text("Successfully reset prefix cache\n")
    bench = VLLMBenchmark.__new__(VLLMBenchmark)
    bench._port, bench._log_path = 1, str(log)
    connection = Mock()
    connection.getresponse.return_value.status = 200
    clock = iter([0, 0, 31])
    monkeypatch.setattr("integration.vllm_integration_test.http.client.HTTPConnection", lambda *_a, **_kw: connection)
    monkeypatch.setattr("integration.vllm_integration_test.time.monotonic", lambda: next(clock))
    monkeypatch.setattr("integration.vllm_integration_test.time.sleep", lambda _: None)
    with pytest.raises(RuntimeError, match="did not confirm"):
        bench.reset_local_prefix_cache()
    connection.close.assert_called_once()


@pytest.mark.parametrize("enabled", [None, "0", "1"])
def test_workload_latency_limits_are_opt_in(monkeypatch, enabled):
    from largecap.large_capacity_test import _check_latency

    if enabled is None:
        monkeypatch.delenv("MMC_TEST_CHECK_PERFORMANCE", raising=False)
    else:
        monkeypatch.setenv("MMC_TEST_CHECK_PERFORMANCE", enabled)
    if enabled == "1":
        with pytest.raises(AssertionError, match="exceeds"):
            _check_latency("Read", 200.0, 150.0)
    else:
        _check_latency("Read", 200.0, 150.0)
