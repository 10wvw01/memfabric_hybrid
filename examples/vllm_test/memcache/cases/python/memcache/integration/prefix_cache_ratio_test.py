"""Measure shared-prefix TTFT with complete, fixed-length generation."""

import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import statistics
import time

from transformers import AutoTokenizer

if __package__:
    from .vllm_integration_test import VLLMBenchmark
else:
    from vllm_integration_test import VLLMBenchmark

INPUT_TOKENS = 4096
RATIOS = (0, 25, 50, 75, 100)


def digest(tokens):
    return hashlib.sha256(json.dumps(tokens).encode()).hexdigest()


def build_dataset(model, corpus, samples):
    tokenizer = AutoTokenizer.from_pretrained(model, trust_remote_code=True)
    rows = [json.loads(line) for line in Path(corpus).read_text().splitlines() if line.strip()]
    cases = []
    for ratio in RATIOS:
        shared = INPUT_TOKENS * ratio // 100
        for index in range(samples):
            group = f'case-{ratio}-{index}'
            header = tokenizer.encode(
                f'Agent session {group}. Tools: search, read_file, run_command. '
                'Use the following background and previous tool observations.\n',
                add_special_tokens=False,
            )
            source = tokenizer.encode(rows[(index + ratio) % len(rows)]['question'], add_special_tokens=False)
            prefix = (header + source * 5)[:shared]
            suffix = tokenizer.encode(
                f'\nCurrent step {group}: inspect this new tool result and plan the next action.\n',
                add_special_tokens=False,
            ) + source * 5
            prompt = prefix + suffix[:INPUT_TOKENS - shared]
            assert len(prompt) == INPUT_TOKENS
            seed_prompt = prompt.copy()
            if shared < INPUT_TOKENS:
                seed_prompt[shared] = (seed_prompt[shared] + 1) % tokenizer.vocab_size
            cases.append({'seed_prompt': seed_prompt, 'id': group, 'ratio': ratio, 'shared_tokens': shared,
                          'prefix': prefix, 'prompt': prompt, 'prompt_sha256': digest(prompt)})
    return cases


class RatioBenchmark(VLLMBenchmark):
    def __init__(self, mode, **kwargs):
        super().__init__(**kwargs)
        self.mode = mode

    def _build_serve_cmd(self, with_memcache):
        cmd = super()._build_serve_cmd(with_memcache)
        cmd[cmd.index('--max-num-seqs') + 1] = '1'
        cmd.extend(['--block-size', '1024'])
        cmd[cmd.index('--max-num-batched-tokens') + 1] = '1024'
        if self.mode == 'disabled':
            cmd[cmd.index('--enable-prefix-caching')] = '--no-enable-prefix-caching'
            cmd[cmd.index('--mamba-cache-mode') + 1] = 'none'
        return cmd

    def generate(self, prompt, output_tokens):
        body = {'model': self._served_model_name, 'prompt': prompt,
                'max_tokens': output_tokens, 'min_tokens': output_tokens,
                'stream': True, 'stream_options': {'include_usage': True},
                'return_token_ids': True, 'temperature': 0, 'seed': 42, 'ignore_eos': True}
        conn = http.client.HTTPConnection('127.0.0.1', self._port, timeout=1800)
        started = time.perf_counter()
        first = None
        tokens = []
        usage = None
        last_report = 0
        try:
            conn.request('POST', '/v1/completions', json.dumps(body), {'Content-Type': 'application/json'})
            response = conn.getresponse()
            if response.status != 200:
                raise RuntimeError(f'HTTP {response.status}: {response.read().decode()}')
            for line in response:
                if not line.startswith(b'data: ') or b'[DONE]' in line:
                    continue
                event = json.loads(line[6:])
                if 'error' in event:
                    raise RuntimeError(event['error'])
                usage = event.get('usage') or usage
                for choice in event.get('choices', []):
                    ids = choice.get('token_ids') or []
                    if first is None and (ids or choice.get('text')):
                        first = time.perf_counter()
                    tokens.extend(ids)
                    if len(tokens) >= last_report + 256:
                        print(f'GENERATED {len(tokens)}/{output_tokens}', flush=True)
                        last_report = len(tokens)
            elapsed = time.perf_counter() - started
            assert first is not None, 'No generated token received'
            assert usage and usage['prompt_tokens'] == len(prompt), usage
            assert usage['completion_tokens'] == output_tokens, usage
            assert len(tokens) == output_tokens, (len(tokens), usage)
            return {'ttft_ms': (first - started) * 1000, 'total_ms': elapsed * 1000,
                    'usage': usage, 'output_sha256': digest(tokens),
                    'output_token_ids': tokens, 'first_tokens': tokens[:16]}
        finally:
            conn.close()

    def metrics_delta(self, before):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            after = self.cache_hit_counts(include_queries=True)
            delta = [end - start for start, end in zip(before, after)]
            if self.mode == 'disabled' or delta[2] >= INPUT_TOKENS:
                return dict(zip(('local_hit_tokens', 'external_hit_tokens', 'queried_tokens'), delta))
            time.sleep(0.5)
        raise RuntimeError(f'Metrics did not include the completed request: {delta}')


def run_case(bench, case, output_tokens):
    bench.reset_local_prefix_cache()
    seed_result = None
    if bench.mode != 'disabled' and case['prefix']:
        seed_result = bench.generate(case['seed_prompt'], 1)
        time.sleep(0.5)
        if bench.mode == 'memcache_dram':
            bench.reset_local_prefix_cache()
    before = bench.cache_hit_counts(include_queries=True)
    result = bench.generate(case['prompt'], output_tokens)
    result.update(bench.metrics_delta(before))
    result.update({key: case[key] for key in ('id', 'ratio', 'shared_tokens', 'prompt_sha256')})
    hits = result['local_hit_tokens'] + result['external_hit_tokens']
    result['actual_reuse_ratio'] = hits / INPUT_TOKENS
    result['cache_verified'] = (
        hits == 0 if bench.mode == 'disabled' or case['ratio'] == 0 else
        (0 < hits < INPUT_TOKENS if case['ratio'] == 100 else hits == case['shared_tokens'])
    )
    if bench.mode != 'memcache_dram':
        result['cache_verified'] &= result['external_hit_tokens'] == 0
    else:
        result['cache_verified'] &= result['local_hit_tokens'] == 0
    if case['ratio'] == 100 and seed_result is not None:
        result['seed_first_token'] = seed_result['first_tokens'][0]
        result['first_token_matches_seed'] = result['first_tokens'][0] == result['seed_first_token']
        result['cache_verified'] &= result['first_token_matches_seed']
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--model', required=True)
    parser.add_argument('--corpus', required=True)
    parser.add_argument('--result-dir', required=True)
    parser.add_argument('--mode', choices=('disabled', 'native', 'memcache_dram'), required=True)
    parser.add_argument('--tp', type=int, choices=(1, 2), required=True)
    parser.add_argument('--devices', required=True)
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--port', type=int, default=4217)
    parser.add_argument('--samples', type=int, default=3)
    parser.add_argument('--output-tokens', type=int, default=2048)
    parser.add_argument('--ratios', type=int, nargs='+', default=list(RATIOS))
    args = parser.parse_args()
    result_dir = Path(args.result_dir)
    result_dir.mkdir(parents=True, exist_ok=True)
    cases = build_dataset(args.model, args.corpus, args.samples)
    (result_dir / 'dataset.json').write_text(json.dumps(cases))
    bench = RatioBenchmark(args.mode, model_path=args.model, served_model_name='prefix35b',
                           port=args.port, device_ids=args.devices, tensor_parallel_size=args.tp)
    result_path = result_dir / 'results.json'
    result = {'mode': args.mode, 'tp': args.tp, 'output_tokens': args.output_tokens,
              'samples': [], 'errors': []}
    if args.resume and result_path.exists():
        result = json.loads(result_path.read_text())
        assert (result['mode'], result['tp'], result['output_tokens']) == (
            args.mode, args.tp, args.output_tokens
        )
        by_id = {case['id']: case for case in cases}
        assert all(sample['prompt_sha256'] == by_id[sample['id']]['prompt_sha256']
                   for sample in result['samples'])
        result['errors'] = []
    completed_ids = {sample['id'] for sample in result['samples']}
    try:
        cmd = bench._build_serve_cmd(args.mode == 'memcache_dram')
        result['command'] = cmd
        bench._start_vllm(cmd)
        result['server_log'] = bench._log_path
        bench.generate([42] * INPUT_TOKENS, 1)
        for case in cases:
            if case['ratio'] not in args.ratios or case['id'] in completed_ids:
                continue
            print(f"START {args.mode} TP={args.tp} {case['id']}", flush=True)
            sample = run_case(bench, case, args.output_tokens)
            result['samples'].append(sample)
            (result_dir / 'results.json').write_text(json.dumps(result, indent=2))
            print(json.dumps(sample), flush=True)
        result['summary'] = [
            {'ratio': ratio, 'count': len(group),
             'ttft_p50_ms': statistics.median(x['ttft_ms'] for x in group),
             'total_p50_ms': statistics.median(x['total_ms'] for x in group),
             'cache_verified': all(x['cache_verified'] for x in group)}
            for ratio in args.ratios
            if (group := [x for x in result['samples'] if x['ratio'] == ratio])
        ]
        assert all(x['cache_verified'] for x in result['samples']), (
            'Actual cache reuse differs from the requested prefix'
        )
    except Exception as error:
        result['errors'].append(repr(error))
        raise
    finally:
        bench.stop_vllm()
        (result_dir / 'results.json').write_text(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
