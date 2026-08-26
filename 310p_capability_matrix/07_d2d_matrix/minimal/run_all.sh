#!/bin/bash
# run_all.sh — 运行 13 个最小化测试并汇总结果
set -e
cd "$(dirname "$0")"

make -s minitest 2>/dev/null

echo "=========================================="
echo " 07_d2d_matrix 最小化测试 (13 tests)"
echo "=========================================="
echo ""

PASS=0
FAIL=0

run_test() {
    local t=$1
    local a_dev=${2:-0}
    local b_dev=${3:-1}
    local desc="$4"

    printf "T%02d | dev(%d,%d) | %-40s | " "$t" "$a_dev" "$b_dev" "$desc"

    output=$(T=$t A_DEV=$a_dev B_DEV=$b_dev timeout 30 mpirun --allow-run-as-root -np 2 ./minitest 2>&1) || true

    # 检查是否有 FAIL(507899)
    if echo "$output" | grep -q "FAIL(507899)"; then
        echo "❌ FAIL(507899)"
        FAIL=$((FAIL+1))
    elif echo "$output" | grep -q "FAIL(other)"; then
        echo "❌ FAIL(other)"
        FAIL=$((FAIL+1))
    else
        echo "✅ OK"
        PASS=$((PASS+1))
    fi

    # 打印关键行
    echo "$output" | grep -E "r[01]\(dev[0-9]+\):" | sed 's/^/     /'
    echo ""
}

# 第一组: 最小复现 (同卡 card0: dev 0,1)
echo "--- 第一组: 最小复现 ---"
run_test 1  0 1 "r0=PEER(r1) → r1=PEER(r0)"
run_test 2  0 1 "r0=DEFAULT(r1) → r1=PEER(r0)"
run_test 3  0 1 "r0=nothing → r1=PEER(r0)"

# 第二组: 定位阻塞条件 (同卡 card0)
echo "--- 第二组: 定位阻塞条件 ---"
run_test 4  0 2 "r0=PEER(r1@dev2) → r1=PEER(r0@dev0)"
run_test 5  0 4 "r0=PEER(r1@dev4) → r1=PEER(r0@dev0)"
run_test 6  0 1 "r0=PEER(r1) → r1=DEFAULT(r0)"

# 第三组: 卡/Socket 维度
echo "--- 第三组: 卡/Socket 维度 ---"
run_test 7  0 2 "r0=PEER(r1@dev2) → r1=PEER(r0@dev0) [跨卡同socket]"
run_test 8  4 5 "r0=PEER(r1@dev5) → r1=PEER(r0@dev4) [socket1同卡]"
run_test 9  0 4 "r0=PEER(r1@dev4) → r1=PEER(r0@dev0) [跨socket]"

# 第四组: EnablePeerAccess 直接测试
echo "--- 第四组: EnablePeerAccess ---"
run_test 10 0 1 "r0=EPA(r1) → r1=PEER(r0)"
run_test 11 0 1 "r0=EPA(r1) → r1=EPA(r0)"

# 第五组: 数量与顺序
echo "--- 第五组: 数量与顺序 ---"
run_test 12 0 1 "r0=PEER(r1)×4 → r1=PEER(r0)"
run_test 13 0 1 "r0=nothing → r1=PEER(r0)×2"

echo "=========================================="
echo " 汇总: PASS=$PASS, FAIL=$FAIL"
echo "=========================================="
