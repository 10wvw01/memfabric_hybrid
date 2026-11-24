#!/usr/bin/env bash
# Pre-flight check: NPU device liveness and driver health before running tests.
#
# 驱动异常时 npu-smi 命令会卡死，且 put_from_layers 的 ioctl 退化数千倍，
# 导致 workload 单次 prefill 从 ~4ms 退化到 ~27s。本脚本在测试前做两段检查：
#   1. 用 ACL runtime API 代替 npu-smi 检测目标设备是否存活（即时返回）
#   2. 对目标设备做一次 256MB D2D 拷贝，超过阈值即判定驱动不健康
#
# 用法: bash check_env.sh --device-ids 4,5 [--latency-threshold-ms 50]
#   --device-ids <a,b,...>   目标设备 ID（必选）
#   --latency-threshold-ms N SDMA 单次拷贝延迟阈值，默认 50ms
# 退出码: 0=通过, 1=失败
set -euo pipefail

DEVICE_IDS=""
LATENCY_THRESHOLD_MS=100

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device-ids)          DEVICE_IDS="$2"; shift 2 ;;
        --latency-threshold-ms) LATENCY_THRESHOLD_MS="$2"; shift 2 ;;
        --help|-h)
            sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "[ERROR] 未知参数: $1" >&2; exit 2 ;;
    esac
done

[[ -n "$DEVICE_IDS" ]] || { echo "[ERROR] --device-ids 必选" >&2; exit 2; }

# 256MB D2D 拷贝预检：正常 ~50ms，驱动异常时数千 ms
CHECK_SCRIPT=$(cat <<'PYEOF'
import acl, sys, time

device_ids = [int(x) for x in sys.argv[1].split(",")]
threshold_ms = int(sys.argv[2])
size = 256 * 1024 * 1024

acl.init()
failed = []
for dev in device_ids:
    try:
        ret = acl.rt.set_device(dev)
        if ret != 0:
            failed.append(f"dev{dev}: set_device ret={ret}")
            continue
        ctx, ret = acl.rt.create_context(dev)
        if ret != 0:
            failed.append(f"dev{dev}: create_context ret={ret}")
            continue
        ptr_a, ret = acl.rt.malloc(size, 0)
        ptr_b, ret = acl.rt.malloc(size, 0)
        stream, ret = acl.rt.create_stream()
        if ret != 0:
            failed.append(f"dev{dev}: create_stream ret={ret}")
            continue
        acl.rt.memset(ptr_a, size, 0, size)
        acl.rt.memset(ptr_b, size, 1, size)
        acl.rt.synchronize_stream(stream)
        t0 = time.perf_counter()
        acl.rt.memcpy(ptr_b, size, ptr_a, size, 2)
        acl.rt.synchronize_stream(stream)
        elapsed_ms = (time.perf_counter() - t0) * 1000
        acl.rt.free(ptr_a)
        acl.rt.free(ptr_b)
        acl.rt.destroy_stream(stream)
        acl.rt.destroy_context(ctx)
        if elapsed_ms > threshold_ms:
            failed.append(f"dev{dev}: D2D 256MB latency={elapsed_ms:.0f}ms > {threshold_ms}ms (driver unhealthy)")
        else:
            print(f"[CHECK] dev{dev}: D2D 256MB latency={elapsed_ms:.1f}ms OK")
    except Exception as e:
        failed.append(f"dev{dev}: {e}")

if failed:
    for msg in failed:
        print(f"[FAIL] {msg}", file=sys.stderr)
    sys.exit(1)
print("[CHECK] all target devices healthy")
PYEOF
)

echo "[INFO] 预检目标设备: $DEVICE_IDS (SDMA 阈值 ${LATENCY_THRESHOLD_MS}ms)"
python3 -c "$CHECK_SCRIPT" "$DEVICE_IDS" "$LATENCY_THRESHOLD_MS"
rc=$?
if [[ $rc -ne 0 ]]; then
    echo "[ERROR] 预检失败，驱动异常或设备不可用。" >&2
    echo "[ERROR] 可能原因：其他进程的 devmm 映射错误导致驱动层全局锁竞争。" >&2
    echo "[ERROR] 排查：dmesg | grep 'devmm.*Map address failed' 查看是否有持续错误。" >&2
    exit 1
fi
