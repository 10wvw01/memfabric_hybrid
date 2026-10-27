import glob
import json
import sys


def load(session, mode):
    files = sorted(glob.glob(f"/tmp/mf_v2_g3_eager_session_{session}/{mode}_eager_rep*_run.json"))
    if not files:
        return None
    return json.load(open(files[-1]))


def fmt(d):
    if d is None:
        return "  (pending)"
    p999 = d.get("p99.9_ttft_ms", d.get("p99_ttft_ms", 0))
    return (
        f"output={d['output_throughput']:.2f} tok/s  "
        f"TTFT p50={d.get('median_ttft_ms', 0):.0f}/p90={d.get('p90_ttft_ms', 0):.0f}/"
        f"p99.9={p999:.0f} ms  "
        f"TPOT p50={d.get('median_tpot_ms', 0):.1f}  "
        f"ok={d['completed']}/{d['num_prompts']} fail={d['failed']}"
    )


for s in sys.argv[1:] or ["1", "2", "3"]:
    st, fu = load(s, "stock"), load(s, "fused")
    print(f"== EAGER session {s} ==")
    print("  stock:", fmt(st))
    print("  fused:", fmt(fu))
    if st and fu:
        dt = (fu["output_throughput"] - st["output_throughput"]) / st["output_throughput"] * 100
        dttft = (fu.get("median_ttft_ms", 0) - st.get("median_ttft_ms", 0)) / st.get("median_ttft_ms", 1) * 100
        dtpot = (fu.get("median_tpot_ms", 0) - st.get("median_tpot_ms", 0)) / st.get("median_tpot_ms", 1) * 100
        print(f"  delta: output {dt:+.2f}%  TTFT-p50 {dttft:+.2f}%  TPOT-p50 {dtpot:+.2f}%")
