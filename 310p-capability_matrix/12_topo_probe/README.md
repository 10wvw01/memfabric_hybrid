# 12：8设备拓扑与peer接口

## 运行

```bash
source /usr/local/Ascend/cann/set_env.sh
export ASCEND_HOME=/usr/local/Ascend/cann
cd 310p-capability_matrix/12_topo_probe
bash run.sh
```

## 2026-11-13状态

未执行：八设备未同时空闲。设备0、1、4、5有其他任务，设备2、3、6、7空闲。

## 原有实验过程与数据

以下保留CANN 9.1.0下的原有实验条件、步骤、输出及分析。2026-11-13的执行情况见上表。

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
- EnablePeerAccess:die 对预期返回 507899(drv internal error,同卡双die的该调用返回驱动内部错误);跨卡预期全部返回 0(每对测试后立即 disable 避免累积状态污染);
- RESULT: PASS(矩阵打印成功即 PASS,can 值 / enable 返回码是平台事实不算失败)。

## 实测结论

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
- **die 对 (0,1)(2,3)(4,5)(6,7):can=0(双向,4/4)——符合预期,该接口对同卡双die返回can=0。**
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
2. **EnablePeerAccess:该次die对(同卡双die)调用返回507899;跨卡全部成功(48/48)。**
3. 每对测试后立即 disable 可消除累积状态污染,确保所有跨卡 enable 正常完成。
