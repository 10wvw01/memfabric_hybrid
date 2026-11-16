from unittest.mock import Mock

import pytest

from integration import eviction_checks as checks
from integration import large_capacity_vllm_test as largecap


def metadata(medium="DRAM", state=1, valid=True, ranks=(0, 1, 2, 3)):
    return [
        {"key": str(rank), "valid": valid, "blobs": [{"rank": rank, "medium": medium, "state": state}]}
        for rank in ranks
    ]


@pytest.mark.parametrize(
    "items", [[], metadata("HBM"), metadata(state=0), metadata(valid=False), metadata(ranks=(0, 1))]
)
def test_rejects_incomplete_or_non_dram_cache(items):
    assert not checks.readable(items, 4, "DRAM")


def test_accepts_complete_readable_dram_cache():
    assert checks.readable(metadata(), 4, "DRAM")


@pytest.mark.parametrize("evict,dram,candidates", [(5, 0, [{}]), (0, 10, [{}]), (5, 10, [])])
def test_eviction_requires_counter_dram_usage_and_readable_prompt(monkeypatch, evict, dram, candidates):
    monkeypatch.setattr(checks, "meta_summary", lambda: {"evict": evict, "hbm_used": 10, "dram_used": dram})
    monkeypatch.setattr(checks, "dram_candidates", lambda *_: candidates)
    monkeypatch.setattr(checks.time, "monotonic", Mock(side_effect=[0, 0, 31]))
    monkeypatch.setattr(checks.time, "sleep", lambda _: None)
    with pytest.raises(AssertionError, match="No complete readable DRAM prompt"):
        checks.wait_dram_candidates([{}], 4, 0)


@pytest.mark.parametrize(
    "tail,message",
    [
        ([b'data: {"error":{"message":"late failure"}}\n'], "late failure"),
        ([], "before"),
        ([b'data: [DONE]\n'], "finish_reason"),
    ],
)
def test_stream_checks_errors_after_first_token(tail, message):
    first = b'data: {"choices":[{"token_ids":[17]}]}\n'
    with pytest.raises((RuntimeError, AssertionError), match=message):
        checks.read_token_stream(iter([first, *tail]), 0)


def test_stream_reads_all_tokens_and_finish():
    stream = iter(
        [
            b'data: {"choices":[{"token_ids":[17]}]}\n',
            b'data: {"choices":[{"token_ids":[18],"finish_reason":"length"}]}\n',
            b'data: [DONE]\n',
        ]
    )
    _, tokens = checks.read_token_stream(stream, 0)
    assert tokens == [17, 18]


def test_missing_metrics_cannot_pass_as_a_miss(monkeypatch):
    response = Mock(status=200)
    response.read.return_value = b"# missing counters"
    connection = Mock()
    connection.getresponse.return_value = response
    monkeypatch.setattr(checks.http.client, "HTTPConnection", lambda *_a, **_kw: connection)
    with pytest.raises(AssertionError, match="Missing required"):
        checks.cache_counts(Mock(_port=1))


def test_miss_waits_for_request_query_statistics(monkeypatch):
    monkeypatch.setattr(
        checks,
        "cache_counts",
        Mock(
            side_effect=[
                [0, 0, 0],
                [0, 0, 0],
                [0, 0, 4096],
            ]
        ),
    )
    monkeypatch.setattr(checks, "completion_with_tokens", lambda *_: (1.0, [17]))
    monkeypatch.setattr(checks.time, "sleep", lambda _: None)
    assert checks.checked_completion(Mock(), [1], False) == (1.0, [17])
    assert checks.cache_counts.call_count == 3


def test_dram_hit_rejects_different_output(monkeypatch):
    sample = {"prompt": [1], "keys": {"key"}, "tokens": [17]}
    monkeypatch.setattr(largecap, "MEASURED_REQUESTS", 1)
    monkeypatch.setattr(checks, "query_keys", lambda _: metadata())
    monkeypatch.setattr(checks, "checked_completion", lambda *_: (1.0, [18]))
    with pytest.raises(AssertionError, match="differs from miss reference"):
        largecap._measure_evicted_mixed(Mock(_tensor_parallel_size=4), [sample], 1)


def test_prefill_does_not_pass_without_eviction(monkeypatch):
    monkeypatch.setattr(largecap, "EVICT_PREPOP_COUNT", 2)
    monkeypatch.setattr(checks, "eviction_prompt", lambda *_: [1])
    monkeypatch.setattr(checks, "meta_summary", lambda: {"evict": 0, "hbm_used": 10, "dram_used": 0})
    monkeypatch.setattr(checks, "all_keys", lambda: set())
    monkeypatch.setattr(checks, "checked_completion", lambda *_: (1.0, [17]))
    monkeypatch.setattr(checks, "wait_saved_keys", lambda *_: {"key"})
    wait = Mock(side_effect=AssertionError("No eviction"))
    monkeypatch.setattr(checks, "wait_dram_candidates", wait)
    with pytest.raises(AssertionError, match="No eviction"):
        largecap._prefill_for_eviction(Mock(_tensor_parallel_size=4))
    wait.assert_called_once()


def test_eviction_config_is_scoped_to_the_case(tmp_path, monkeypatch):
    config = tmp_path / "original.conf"
    config.write_text("ock.mmc.local_service.hbm.size = 3GB\n")
    monkeypatch.setenv("MMC_LOCAL_CONFIG_PATH", str(config))
    benchmark = Mock(_tensor_parallel_size=4)
    seen = []

    def start():
        import os
        from pathlib import Path

        seen.append(Path(os.environ["MMC_LOCAL_CONFIG_PATH"]).read_text())

    benchmark.start_vllm_with_memcache.side_effect = start
    monkeypatch.setattr(largecap, "_prefill_for_eviction", lambda *_: ([{}], 10, {"evict": 1}))
    monkeypatch.setattr(largecap, "_measure_evicted_mixed", lambda *_: ([1.0], 6))
    largecap.test_eviction_aware_mixed_4rank(benchmark, tmp_path, monkeypatch, Mock(), 0.3)
    assert seen == ["ock.mmc.local_service.hbm.size = 1GB\n"]
    assert config.read_text() == "ock.mmc.local_service.hbm.size = 3GB\n"


@pytest.mark.parametrize("ratio", ["0", "1", "-1", "nan"])
def test_rejects_invalid_shared_device_memory_ratio(monkeypatch, ratio):
    monkeypatch.setenv("MMC_TEST_MAX_USED_MEMORY_RATIO", ratio)
    bench = largecap.VLLMTTFTBenchmark("model", "name", 4077, "0", 4)
    with pytest.raises(ValueError, match="between 0 and 1"):
        bench._wait_for_device_memory()


def test_shared_device_memory_ratio_requires_three_samples(monkeypatch):
    from types import SimpleNamespace
    from integration import vllm_integration_test as integration

    monkeypatch.setenv("MMC_TEST_MAX_USED_MEMORY_RATIO", "0.65")
    probe = Mock(return_value=SimpleNamespace(stdout="| 0 0 | bus | 0 60 / 100 |\n"))
    monkeypatch.setattr(integration.subprocess, "run", probe)
    monkeypatch.setattr(integration.time, "sleep", lambda _: None)
    bench = largecap.VLLMTTFTBenchmark("model", "name", 4077, "0", 4)
    bench._wait_for_device_memory()
    assert probe.call_count == 3


def test_summary_reports_actual_pool_capacity(monkeypatch):
    monkeypatch.setattr(checks, "meta_text", lambda _: "evict=3 hbm_used=10/400 dram_used=20/800")
    assert checks.meta_summary() == {
        "evict": 3,
        "hbm_used": 10,
        "dram_used": 20,
        "hbm_total": 400,
        "dram_total": 800,
    }


def test_query_rejects_missing_key_metadata(monkeypatch):
    monkeypatch.setattr(checks, "meta_text", lambda _: '{"success":true,"data":[]}')
    with pytest.raises(AssertionError, match="Incomplete cache metadata"):
        checks.query_keys({"missing"})


def test_eviction_prompts_keep_length_question_and_unique_prefix(monkeypatch):
    tokenizer = Mock()
    tokenizer.encode.side_effect = lambda text, **_: list(map(ord, text))
    monkeypatch.setattr(checks, "_tokenizer_for_model", lambda _: tokenizer)
    first = checks.eviction_prompt("model", 8000)
    second = checks.eviction_prompt("model", 8001)
    assert len(first) == len(second) == 4096
    assert first[:384] != second[:384]
    assert first == checks.eviction_prompt("model", 8000)
    text = "".join(map(chr, first))
    assert text.startswith("Verification case 8000. The verification phrase is:")
    assert text.endswith("without explanation.\nVerification phrase:")


def test_eviction_prompt_rejects_budget_that_cuts_verification_phrase(monkeypatch):
    tokenizer = Mock()
    tokenizer.encode.side_effect = lambda text, **_: list(map(ord, text))
    monkeypatch.setattr(checks, "_tokenizer_for_model", lambda _: tokenizer)
    with pytest.raises(ValueError, match="budget"):
        checks.eviction_prompt("model", 8000, 4)


def test_unstable_hbm_reference_fails_before_eviction(monkeypatch):
    monkeypatch.setattr(checks, "meta_summary", lambda: {"evict": 0})
    monkeypatch.setattr(checks, "all_keys", lambda: set())
    monkeypatch.setattr(checks, "eviction_prompt", lambda *_: [1])
    monkeypatch.setattr(checks, "wait_saved_keys", lambda *_: {"key"})
    monkeypatch.setattr(checks, "checked_completion", Mock(side_effect=[(1.0, [17]), (1.0, [18])]))
    with pytest.raises(AssertionError, match="Unstable HBM reference"):
        largecap._prefill_for_eviction(Mock(_tensor_parallel_size=4))
