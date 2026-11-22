# 310P AICPU服务生命周期检查

先构建项目并加载CANN环境，选择空闲设备，通过环境变量指定设备号和独立store端口。

    export TEST_DEVICE_BASE=6
    export STORE_URL=tcp://127.0.0.1:18937
    bash test/integration/310p_service_lifecycle/run.sh normal

normal检查三个池的创建和销毁、调用线程Context恢复，以及服务常驻期间的业务Context同步。池使用不同ID，避免沿用旧store键。

故障检查会暂停、恢复和强杀该脚本创建的探针进程；运行前确认设备空闲。

    export TEST_LOG_DIR=/workspace/my-logs-dir/memfabric_hybrid/service-lifecycle
    python3 test/integration/310p_service_lifecycle/faults.py

lease检查host停止更新超过租约后，设备发布失联错误及资源确认。hold检查host强杀后的设备管理查询和新进程初始化。脚本不执行设备恢复。SQ满、发布中断及完成戳缺失由单元测试注入，真实传输使用相邻的310p_shmem_signal用例。

HCCL混用检查分别运行hccl-first和hccl-after，验证初始化顺序、AllReduce结果及销毁后的Context。

PyPTO混用检查使用与当前Python版本匹配的已构建PyPTO包：

    bash test/integration/310p_service_lifecycle/run.sh --build-pypto
    mpirun --allow-run-as-root --stdin none -np 2 python3 test/integration/310p_service_lifecycle/pypto_coexist.py 0
    mpirun --allow-run-as-root --stdin none -np 2 python3 test/integration/310p_service_lifecycle/pypto_coexist.py 1

设置LD_LIBRARY_PATH指向本次构建的smem、hybm和CANN库，MF_SDMA_ORCH_DIR指向编排kernel目录。两个顺序分别验证先运行PyPTO、先启动MF服务，均在服务运行期间执行PyPTO并检查结果。
