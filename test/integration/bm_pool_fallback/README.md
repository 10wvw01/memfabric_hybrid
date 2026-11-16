# 310P BM 设备内存回退回归

固定使用设备0–3。单rank使用设备0，双rank固定使用同卡设备0/1，四rank使用设备0–3。
测试创建BM设备池和主机DRAM池，并校验各rank池的首尾数据。
单rank、同卡双rank在P2P_DDR不足后允许回退普通设备DDR。
超过两个rank的设备池在P2P_DDR不足时必须创建失败，验证没有尝试普通设备DDR，且没有进入join。
混合分配用例只让设备0的P2P申请返回内存不足，其他rank保持P2P分配。
故障注入只作用于64MiB设备池；4GiB用例使用实际分配行为。
加入前后使用文件屏障，成功加入后按BM示例等待1秒，随后校验数据。

## 构建

在仓库根目录，加载当前CANN环境后执行：

    bash script/build_and_pack_run.sh --build_mode RELEASE --build_python OFF --xpu_type NPU
    cmake -S test/integration/bm_pool_fallback -B build/bm_pool_fallback \
      -DMF_OUTPUT="$PWD/output" -DCANN_HOME="$ASCEND_HOME_PATH"
    cmake --build build/bm_pool_fallback -j8

## 运行

使用可容纳至少20GiB共享内存的独立测试环境；四rank DRAM用例每rank申请5GiB。

    export ASCEND_RT_VISIBLE_DEVICES=0,1,2,3
    export LD_LIBRARY_PATH="$PWD/output/smem/lib64:$PWD/output/hybm/lib64:$LD_LIBRARY_PATH"
    export LD_LIBRARY_PATH="$PWD/output/acc_links/lib64:$PWD/output/acc_offload/lib64:$LD_LIBRARY_PATH"
    python3 test/integration/bm_pool_fallback/test_pool_fallback.py \
      --binary build/bm_pool_fallback/pool_worker \
      --injector build/bm_pool_fallback/libp2p_oom_injector.so \
      --log-dir /workspace/my-logs-dir/bm_pool_fallback -v

可在命令末尾追加PoolFallbackTest.test_same_card_injected_fallback等用例名运行单项。
日志写入--log-dir指定目录。测试失败返回非零。

双rank跨卡组合由UT验证在导入前拒绝，实机双rank用例仅使用同卡两个die。
