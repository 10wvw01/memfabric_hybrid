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
