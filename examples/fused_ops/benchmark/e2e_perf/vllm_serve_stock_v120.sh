#!/bin/bash
# vllm serve — stock baseline (no fusion), TP2, trunk v1.2.0 runtime variant.
#
# Diff vs vllm_serve_stock.sh (v28):
#   - MFOUT: trunk output tree (memfabric_trunk_v120/output)
#   - MF_SDMA_ORCH_DEPLOY_HELPER / MF_SDMA_ORCH_JSON / MEMFABRIC_HYBRID_HOME_PATH:
#     removed (v28-only external-helper deploy chain)
#   - MF_SDMA_ORCH_DIR: trunk orch kernel artifact dir
# Prerequisites:
#   - CANN 9.1.0 GA (source'd below)
#   - memfabric trunk v1.2.0 SDK: /home/USER/310P_2/memfabric_trunk_v120/output
#   - prd_code vllm-ascend checkout installed (editable)
# DFLASH=0 drops --speculative-config (dflash off): prefill splits into
# 1x3840+256 (mamba-aligned). TPOT regresses ~2.5x without dflash —
# TTFT-comparison only.
set -u
GA=/usr/local/Ascend/cann-9.1.0
MFOUT=/home/USER/310P_2/memfabric_trunk_v120/output

source $GA/set_env.sh
# 宿主环境可能全局导出 ASCEND_RT_VISIBLE_DEVICES（本机 qwen-dflash 预设 0,1），
# 泄漏进 serve 后设备 4/5 被重映射掉 → set_device(4) Invalid device ID。
# serve 必须看到真实物理 id，此处无条件 unset 兜底。
unset ASCEND_RT_VISIBLE_DEVICES
# Workers land on physical devices via the 310P worker device offset
# (default 4/5; override VLLM_ASCEND_DEVICE_OFFSET for other die pairs).
# Do NOT use ASCEND_RT_VISIBLE_DEVICES here: the remapping makes the smem
# library see logical ids 0/1, which selects the (asymmetric) device-0
# window layout and breaks the symmetric pool ("import gva out of window").
export VLLM_ASCEND_DEVICE_OFFSET=${VLLM_ASCEND_DEVICE_OFFSET:-4}
export MF_SDMA_ORCH_DIR=$MFOUT/hybm/aicpu_kernel
export MEMFABRIC_HYBRID_EXTEND_LIB_PATH=$MFOUT/hybm/lib64
export MIXFABRIC_SDK_OUT=$MFOUT
export LD_LIBRARY_PATH=$MFOUT/smem/lib64:$MFOUT/hybm/lib64:$MFOUT/acc_links/lib64:${LD_LIBRARY_PATH:-}
# stock baseline: no fusion operator enabled.
unset VLLM_ASCEND_ENABLE_SMEM_MM_AR_310P3
unset VLLM_ASCEND_ENABLE_SMEM_MM_IAR_310P3
unset SMEM_MM_310P3_OP

DFLASH=${DFLASH:-1}
# 真正的 dflash 开关：on = speculative 7-token + FULL_DECODE_ONLY ACLGraph；
# off = 去 speculative-config + 仅 enforce-eager。
SPEC_OPTS=(--speculative-config '{"method":"dflash","model":"/home/models/Qwen/Qwen3.6-35B-A3B-DFlash","num_speculative_tokens":7}')
CG_OPTS=(--compilation-config '{"cudagraph_mode":"FULL_DECODE_ONLY","cudagraph_capture_sizes":[8,80]}')
if [ "$DFLASH" != "1" ]; then
  SPEC_OPTS=()
  # dflash off：去 speculative-config，且 FULL_DECODE_ONLY 是 310P dflash 专属
  # 作用域，非 dflash 跑法 = 仅 --enforce-eager
  CG_OPTS=()
fi

exec vllm serve /home/models/Qwen/Qwen3.6-35B-A3B-w8a8 \
  --host 0.0.0.0 --port 8000 \
  --served-model-name qwen3.6-35b-a3b-w8a8 \
  --tensor-parallel-size 2 \
  --quantization ascend \
  --dtype float16 \
  --max-model-len 8192 \
  --max-num-seqs 16 \
  --max-num-batched-tokens 4096 \
  --gpu-memory-utilization 0.90 \
  --enable-prefix-caching \
  --mamba-ssm-cache-dtype float16 \
  --mamba-cache-mode align \
  --enable-chunked-prefill \
  --async-scheduling \
  --enforce-eager \
  "${CG_OPTS[@]}" \
  "${SPEC_OPTS[@]}" \
  --profiler-config.profiler=torch \
  --profiler-config.torch_profiler_dir=${SMEM_PROFILE_DIR:-/tmp/opencode/e2e_profile} \
  --additional-config '{"enable_flashcomm1":false,"enable_flashcomm2_parallel_size":0,"enable_prefill_mc2":false,"enable_fused_mc2":0,"ascend_compilation_config":{"fuse_norm_quant":false,"enable_npugraph_ex":false}}'