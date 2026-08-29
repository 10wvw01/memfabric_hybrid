#!/bin/bash
# precheck.sh — 跑测试前的环境健康检查
# 用法: ./precheck.sh  (在 0824-test/ 下运行)
# 如果输出 WARN/FATAL,不要继续跑测试!

cd "$(dirname "$0")"

echo "===== 环境检查 ====="

# 1. D 态残留进程检查
d_count=$(ps aux | grep '\[test\]' | grep -v grep | grep ' D ' | wc -l)
if [ "$d_count" -gt 0 ]; then
    echo "❌ FATAL: 发现 $d_count 个 D 态残留进程(SIGKILL 不可回收)"
    echo "   这些进程占着设备内存不释放,继续跑会耗尽 hugepages"
    echo "   处理方法:"
    echo "     1. 宿主机: npu-smi set -t reset -i <npu_id> -c <chip_id>"
    echo "     2. 宿主机: rmmod 相关驱动模块后 modprobe"
    echo "     3. 重启机器"
    echo "   在容器内无法清理 D 态进程!"
    ps aux | grep '\[test\]' | grep -v grep
    exit 1
fi
echo "✅ 无 D 态残留进程"

# 2. hugepages 使用率检查
hp_line=$(npu-smi info 2>/dev/null | grep "Hugepages" | head -1)
if [ -z "$hp_line" ]; then
    echo "⚠  WARN: 无法获取 hugepages 信息(npu-smi 不可用?)"
else
    # 提取 "18258 / 18258" 格式
    used=$(echo "$hp_line" | grep -oP '\d+(?= \/ )' | head -1)
    total=$(echo "$hp_line" | grep -oP'(?<=\/ )\d+' | head -1)
    if [ -n "$used" ] && [ -n "$total" ] && [ "$total" -gt 0 ]; then
        pct=$((used * 100 / total))
        if [ "$pct" -gt 80 ]; then
            echo "❌ FATAL: hugepages ${used}/${total} (${pct}%) 已接近耗尽"
            echo "   继续跑会导致 aclrtMalloc(HUGE) 失败或挂死"
            echo "   需要重启或清理 D 态进程"
            exit 1
        elif [ "$pct" -gt 50 ]; then
            echo "⚠  WARN: hugepages ${used}/${total} (${pct}%) 偏高,建议谨慎"
        else
            echo "✅ hugepages ${used}/${total} (${pct}%) 正常"
        fi
    fi
fi

# 3. 设备内存使用率检查(取第一个 dev)
mem_line=$(npu-smi info 2>/dev/null | grep "Memory-Usage" -A1 | tail -1)
if [ -n "$mem_line" ]; then
    used=$(echo "$mem_line" | grep -oP '\d+(?= \/ )' | head -1)
    total=$(echo "$mem_line" | grep -oP '(?<=\/ )\d+' | head -1)
    if [ -n "$used" ] && [ -n "$total" ] && [ "$total" -gt 0 ]; then
        pct=$((used * 100 / total))
        if [ "$pct" -gt 85 ]; then
            echo "❌ FATAL: dev0 内存 ${used}/${total}MB (${pct}%) 接近耗尽"
            exit 1
        elif [ "$pct" -gt 60 ]; then
            echo "⚠  WARN: dev0 内存 ${used}/${total}MB (${pct}%) 偏高"
        else
            echo "✅ dev0 内存 ${used}/${total}MB (${pct}%) 正常"
        fi
    fi
fi

# 4. 设备健康状态
health=$(npu-smi info 2>/dev/null | grep "OK" | wc -l)
if [ "$health" -lt 8 ]; then
    echo "⚠  WARN: 仅 $health/8 个 dev 状态 OK(可能有设备异常)"
else
    echo "✅ $health/8 个 dev 健康"
fi

echo "===== 检查通过,可以运行测试 ====="
exit 0
