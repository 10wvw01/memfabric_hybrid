# 12_topo_probe：设备可达性与 P2P enable


## 测试内容

查询 CanAccessPeer，并逐对 enable；成功后立即 disable，记录每条有向关系的返回码。

## 本轮条件与结果

2026-11-11，Ascend310P3 / Atlas 300I Duo，aarch64，CANN 9.2.1，驱动 26.1.0。

物理设备 2、3、6、7 映射为进程内 0～3；单进程 4×4 矩阵。

**通过，退出码 0。**

本轮仅覆盖两个 Duo 卡上的四个 die；同卡两个有向设备对均 can=0、enable=507899；跨卡四个设备对均双向 can=1，八次有向 enable 均返回 0。CanAccessPeer 和 EnablePeerAccess 是不同观测，不能仅凭前者判断后者或完整数据链路可用。

## 运行

在仓库根目录执行，设备号以当前进程可见的编号为准：

```bash
source /usr/local/Ascend/cann/set_env.sh
ASCEND_RT_VISIBLE_DEVICES=2,3,6,7 MATRIX_N=4 bash 310p-capability_matrix/12_topo_probe/run.sh
```

脚本先编译，传递编译和运行的退出码；超时返回 124，超时后强制结束可能返回 137。
本轮条件、退出码与关键输出收录在[结果记录](../results/2026-11-11.json)。

`MATRIX_N` 支持 4 或 8，默认 8；切换规模时脚本强制重新编译。
可见设备列表必须按完整 Duo 卡的两个 die 相邻排列，使逻辑 (0,1)、(2,3) 等仍对应同卡。
本轮执行了四设备模式，八设备模式仅编译，未运行。

## 详细测试步骤、API 调用链与历史性能数据

以下保留原文的详细流程、结果和性能数据，环境及设备组合以各段原始记录为准。
本轮结果见上文；涉及不同环境或未复测拓扑时，分别阅读，不合并为一次测试结果。

### 12_topo_probe — 设备拓扑探测(CanAccessPeer / EnablePeerAccess)

Atlas 300I Duo (310P3) / CANN 9.1.0 / aarch64,单进程,无 MPI。遍历 8 个 dev(4 卡 × 2 die,die 对 (0,1)(2,3)(4,5)(6,7))。

## 验证能力
- 8 dev SetDevice 可用性
- CanAccessPeer 8×8 矩阵(预期:die 对 can=0,跨卡 can=1)
- EnablePeerAccess 返回码矩阵(die 对预期 507899)

## API 链
- `aclInit` → `aclrtSetDevice(0..7)` → `aclrtDeviceCanAccessPeer(&can, i, j)` 8×8 全查 → `aclrtSetDevice(i)` + `aclrtDeviceEnablePeerAccess(j, 0)` 逐对试探(56 个有序对,i≠j)→ 每对测试后立即 `aclrtDeviceDisablePeerAccess(j)` → `aclrtResetDevice(0..7)` → `aclFinalize`
- 说明:先把 CanAccessPeer 全部查完再做 enable 试探,减少 sticky enable 状态对 can 查询结果的污染;每对 enable 测试后立即 disable,避免累积状态污染后续测试。
- 507899 = `ACL_ERROR_RT_DRV_INTERNAL_ERROR`(drv internal error,见 rt_error_codes.h)。

## 运行方法
```
make && ./test     # 或 ./run.sh
```

## 预期结果
- 8 个 dev SetDevice 全部 OK;
- CanAccessPeer 矩阵:die 对(0,1)(2,3)(4,5)(6,7)can=0,其余 48 个有序跨卡对 can=1;
- EnablePeerAccess:die 对预期返回 507899(drv internal error,同卡双 die 无 PCIe P2P 通路);跨卡预期全部返回 0(每对测试后立即 disable 避免累积状态污染);
- RESULT: PASS(矩阵打印成功即 PASS,can 值 / enable 返回码是平台事实不算失败)。

## ★实测结论

**2026-08-25,单进程,`make && ./test`,RESULT: PASS(每对测试后立即 disable,消除累积状态污染;8 dev SetDevice 全 OK,两矩阵打印成功)。**

### CanAccessPeer 8×8 矩阵(cell = can 值;行=i 访问方,列=j 被访问方)
```
                0       1       2       3       4       5       6       7
  i=0           -       0       1       1       1       1       1       1
  i=1           0       -       1       1       1       1       1       1
  i=2           1       1       -       0       1       1       1       1
  i=3           1       1       0       -       1       1       1       1
  i=4           1       1       1       1       -       0       1       1
  i=5           1       1       1       1       0       -       1       1
  i=6           1       1       1       1       1       1       -       0
  i=7           1       1       1       1       1       1       0       -
```
- 全部 64 次查询 ret=0,无 API 错误;矩阵完全对称。
- **die 对 (0,1)(2,3)(4,5)(6,7):can=0(双向,4/4)——符合预期,同卡双 die 之间无 P2P 通路。**
- **跨卡对(i<j)共 24 对:can=1 双向 24/24,can=0 为 0,无 mixed、无 API 错误——符合预期。**

### EnablePeerAccess 返回码矩阵(on dev i 调 `aclrtDeviceEnablePeerAccess(j,0)`,测试后立即 disable)
```
                0       1       2       3       4       5       6       7
  i=0           -  507899       0       0       0       0       0       0
  i=1      507899       -       0       0       0       0       0       0
  i=2           0       0       -  507899       0       0       0       0
  i=3           0       0  507899       -       0       0       0       0
  i=4           0       0       0       0       -  507899       0       0
  i=5           0       0       0       0  507899       -       0       0
  i=6           0       0       0       0       0       0       -  507899
  i=7           0       0       0       0       0       0  507899       -
```
- **die 对(8 个有序对):enable 全部 507899(=ACL_ERROR_RT_DRV_INTERNAL_ERROR)——符合预期。**
- **跨卡(48 个有序对):ret=0 共 48 个(全部成功),other=0——符合预期。**

### 结论摘要
1. **拓扑事实确认:die 对 can=0(4/4)、跨卡 can=1(24/24),CanAccessPeer 完整可用且对称,与 4 卡 × 2 die 的 PCIe P2P 拓扑一致。**
2. **EnablePeerAccess:die 对(同卡双 die)必然 507899;跨卡全部成功(48/48)。**
3. 每对测试后立即 disable 可消除累积状态污染,确保所有跨卡 enable 正常完成。
