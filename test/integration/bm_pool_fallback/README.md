# 310P BM 设备内存回退回归

使用设备0–3。同卡组合为0/1，跨卡组合为0/2。运行前确认设备空闲。
测试创建BM设备池和主机DRAM池，并校验各rank池的首尾数据。
P2P_DDR不足后的普通设备DDR仅允许同卡共享；跨卡成员加入失败时，校验跨卡HAL导入仅使用P2P内存；已有同卡成员可以先加入成功。
加入前后使用文件屏障，成功加入后按BM示例等待1秒，随后校验数据。
混合分配用例只让设备0的P2P申请返回内存不足，其他rank保持P2P分配。动态加入过程中，已有P2P成员可以先完成共享。
故障注入只作用于64MiB设备池；4GiB用例使用实际分配行为。

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

跨卡共享要求各rank使用带共享能力标记的描述符；旧描述符继续允许同卡共享，跨卡导入前返回失败。
