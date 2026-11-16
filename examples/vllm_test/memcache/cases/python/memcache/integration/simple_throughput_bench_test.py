import json
import sys
from unittest.mock import MagicMock, patch

import pytest

from integration import simple_throughput_bench as bench


def response(payload):
    result = MagicMock()
    result.__enter__.return_value = result
    result.read.return_value = payload
    return result


def test_missing_usage_details_is_reported():
    payload = json.dumps({"usage": {"prompt_tokens": 1, "completion_tokens": 1}}).encode()
    with patch.object(bench.urllib.request, "urlopen", return_value=response(payload)):
        with pytest.raises(RuntimeError, match="enable-prompt-tokens-details"):
            bench.send_request("http://localhost/v1/completions", "model", [1], 1)


def test_cache_metrics_sum_engine_labels():
    payload = b'vllm:prefix_cache_hits_total{engine="0"} 64\nvllm:prefix_cache_hits_total{engine="1"} 32\n'
    payload += b'vllm:external_prefix_cache_hits_total{engine="0"} 128\n'
    with patch.object(bench.urllib.request, "urlopen", return_value=response(payload)):
        assert bench.cache_hit_counts("localhost", 4077) == {
            "prefix_cache_hits_total": 96,
            "external_prefix_cache_hits_total": 128,
        }


def test_reset_preserves_external_cache():
    with patch.object(bench.urllib.request, "urlopen", return_value=response(b"")) as request:
        bench.reset_prefix_cache("localhost", 4077)
    assert request.call_args.args[0].full_url.endswith("/reset_prefix_cache?reset_external=false")


@pytest.mark.parametrize(
    ("failed", "external", "message"),
    [(1, 128, "requests failed"), (0, 0, "without external")],
)
def test_main_rejects_incomplete_cache_hit_round(failed, external, message):
    result = {"failed_requests": failed, "external_hit_tokens": external}
    with patch.object(sys, "argv", ["bench", "--cache-test"]):
        with patch.object(bench, "run_benchmark", return_value=result):
            with pytest.raises(RuntimeError, match=message):
                bench.main()
