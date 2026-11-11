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
def test_ttft_waits_for_generated_token_after_empty_metadata(token_event):
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
        assert benchmark.measure_e2e_ttft_ms([1]) >= 0
    benchmark.reset_local_prefix_cache.assert_called_once()


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
