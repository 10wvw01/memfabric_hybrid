# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Do not report failed or empty SSE completions as successful TTFT samples."""

from unittest.mock import MagicMock, patch

import pytest

from integration.vllm_integration_test import VLLMBenchmark


@pytest.mark.parametrize(
    ('status', 'events', 'message'),
    [
        (500, [], 'HTTP 500'),
        (200, [b'data: {"error":{"message":"engine failed"}}\n'], 'stream error'),
        (200, [b'data: {"choices":[{"text":""}]}\n', b'data: [DONE]\n'], 'without a token'),
    ],
)
def test_invalid_completions_are_rejected(status, events, message):
    benchmark = VLLMBenchmark.__new__(VLLMBenchmark)
    benchmark._served_model_name = 'test-model'
    benchmark._port = 1
    benchmark.reset_local_prefix_cache = MagicMock()
    response = MagicMock()
    response.status = status
    response.read.return_value = b'failed'
    response.__iter__.return_value = iter(events)
    connection = MagicMock()
    connection.getresponse.return_value = response
    with patch('http.client.HTTPConnection', return_value=connection):
        with pytest.raises(RuntimeError, match=message):
            benchmark.measure_e2e_ttft_ms([1])
    benchmark.reset_local_prefix_cache.assert_not_called()
    connection.close.assert_called_once()


@pytest.mark.parametrize(
    'token_event',
    [
        b'data: {"choices":[{"text":"hello"}]}\n',
        b'data: {"choices":[{"text":"","token_ids":[151645]}]}\n',
    ],
)
@pytest.mark.parametrize("reset_local", [True, False])
def test_ttft_waits_for_generated_token_after_empty_metadata(token_event, reset_local):
    benchmark = VLLMBenchmark.__new__(VLLMBenchmark)
    benchmark._served_model_name = 'test-model'
    benchmark._port = 1
    benchmark.reset_local_prefix_cache = MagicMock()
    response = MagicMock()
    response.status = 200
    response.__iter__.return_value = iter(
        [
            b'data: {"choices":[{"text":""}]}\n',
            token_event,
        ]
    )
    connection = MagicMock()
    connection.getresponse.return_value = response
    with patch('http.client.HTTPConnection', return_value=connection):
        assert benchmark.measure_e2e_ttft_ms([1], reset_local=reset_local) >= 0
    assert benchmark.reset_local_prefix_cache.call_count == int(reset_local)


@pytest.mark.parametrize("ttft,external_hits,expected_external", [(1.0, 0, False), (10000.0, 128, True)])
def test_cache_hit_validation_uses_metrics_instead_of_latency(ttft, external_hits, expected_external):
    benchmark = VLLMBenchmark.__new__(VLLMBenchmark)
    benchmark.cache_hit_counts = MagicMock(side_effect=[(0, 0), (0, external_hits)])
    benchmark.measure_e2e_ttft_ms = MagicMock(return_value=ttft)
    assert benchmark.measure_request_with_cache_source([1, 2], expected_external) == ttft


def test_fast_response_without_external_hits_is_rejected():
    benchmark = VLLMBenchmark.__new__(VLLMBenchmark)
    benchmark.cache_hit_counts = MagicMock(return_value=(0, 0))
    benchmark.measure_e2e_ttft_ms = MagicMock(return_value=1.0)
    with patch("integration.vllm_integration_test.time.monotonic", side_effect=[0, 1, 21]), \
         patch("integration.vllm_integration_test.time.sleep"):
        with pytest.raises(AssertionError, match="external=True"):
            benchmark.measure_request_with_cache_source([1, 2], True)


@pytest.mark.parametrize("parent_exited", [False, True])
def test_stop_vllm_cleans_children_in_other_sessions_and_preserves_unrelated_process(tmp_path, parent_exited):
    import subprocess
    import sys
    import time

    import psutil

    child_file = tmp_path / "child.pid"
    child_code = "import time; time.sleep(120)"
    parent_code = (
        "import subprocess,sys,time; "
        f"p=subprocess.Popen([sys.executable,'-c',{child_code!r}],start_new_session=True); "
        f"open({str(child_file)!r},'w').write(str(p.pid)); time.sleep(120)"
    )
    parent = subprocess.Popen([sys.executable, "-c", parent_code], start_new_session=True)
    unrelated = subprocess.Popen([sys.executable, "-c", child_code], start_new_session=True)
    benchmark = VLLMBenchmark.__new__(VLLMBenchmark)
    benchmark._process = parent
    try:
        deadline = time.monotonic() + 10
        while not child_file.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert child_file.exists()
        child = psutil.Process(int(child_file.read_text()))
        benchmark._remember_children()
        if parent_exited:
            parent.kill()
            parent.wait(timeout=5)
        benchmark.stop_vllm()
        assert parent.poll() is not None
        assert not child.is_running() or child.status() == psutil.STATUS_ZOMBIE
        assert unrelated.poll() is None
    finally:
        benchmark.stop_vllm()
        unrelated.kill()
        unrelated.wait(timeout=5)


@pytest.mark.parametrize("local_hits", [0, 3840])
def test_native_mixed_workload_records_actual_hits_after_query_metrics_update(local_hits):
    from integration.vllm_integration_test import _native_prefix_sample

    benchmark = MagicMock()
    benchmark.cache_hit_counts.side_effect = [(0, 0, 0), (local_hits, 0, 4096)]
    benchmark.measure_e2e_ttft_ms.return_value = 12.0
    sample = _native_prefix_sample(benchmark, [1, 2], expected_local=None)
    assert sample["local_hit_tokens"] == local_hits
    assert sample["queried_tokens"] == 4096
    benchmark.measure_e2e_ttft_ms.assert_called_once_with([1, 2], reset_local=False)


def test_native_measurement_rejects_external_cache_hits():
    from integration.vllm_integration_test import _native_prefix_sample

    benchmark = MagicMock()
    benchmark.cache_hit_counts.side_effect = [(0, 0, 0), (0, 128, 4096)]
    with pytest.raises(AssertionError, match="used external cache"):
        _native_prefix_sample(benchmark, [1, 2], expected_local=None)


def test_native_fixture_skips_without_npu_before_creating_server():
    from integration.vllm_integration_test import native_benchmark

    with patch("integration.vllm_integration_test._npu_available", return_value=False), \
         patch("integration.vllm_integration_test._create_benchmark") as create:
        with pytest.raises(pytest.skip.Exception):
            next(native_benchmark.__wrapped__())
    create.assert_not_called()
