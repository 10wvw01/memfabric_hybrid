import sys

sys.path.insert(0, "tests/e2e/_310p")
from memfabric_mm_ar_serve_lib import ServeProcess

with ServeProcess("fused", graph=True, log_path="/tmp/opencode/g3/probe_fused_graph.log") as srv:
    o_proj, out_proj = srv.count_enabled_layers()
    print(f"layers: o_proj={o_proj} out_proj={out_proj}")
    import openai

    client = openai.OpenAI(base_url=f"http://127.0.0.1:{srv.port}/v1", api_key="probe")
    out = client.completions.create(
        model="qwen3.6-35b-a3b-w8a8",
        prompt="Count from one to twenty, step 1: ",
        max_tokens=16,
        temperature=0.0,
    )
    print("decode output:", out.choices[0].text.strip()[:60])
    long_prompt = "summarize: " + ("the quick brown fox jumps over the lazy dog. " * 700)
    out2 = client.completions.create(
        model="qwen3.6-35b-a3b-w8a8",
        prompt=long_prompt[:16384],
        max_tokens=16,
        temperature=0.0,
    )
    print("prefill-4096 output:", out2.choices[0].text.strip()[:60])
print("PROBE DONE")
