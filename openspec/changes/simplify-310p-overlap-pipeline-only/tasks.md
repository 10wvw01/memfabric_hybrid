# 任务清单

- [x] 编排 kernel 精简：删 mode13 case/role1 分支/未用弱符号与结构/实验钩子，
      submitSqe 收敛纯入队、pollSlot 去 hasSecond（hybm_overlap_kernel.cc）
- [x] wire 契约 v2：字段删减 + chunks 新增 + static_assert 重算（hybm_overlap_contract.h），
      MF_OVERLAP_KVER 1→2（CMakeLists.txt），缺省 json 更名 v2
- [x] host API 合并：hybm_overlap_submit(dst, flag, chunks)（big_mem.h/capi.cpp），
      冒烟切流水 chunks=1（hybm_overlap_prep.cpp）
- [x] smem 层对齐：smem_shm_overlap_launch(handle, dst, flag, chunks)（smem_shm.h/.cpp、
      smem_shm_entry.h/.cpp 三方法收敛）
- [x] 08 示例两阶段改造 + README 同步（v2 json/验收标准）
- [x] UT 对齐新 API（五用例语义不变，SubmitChunksBounds 更名）
- [x] 主 spec 同步：API 参数与错误语义要求更新为单一提交接口
- [x] kernel 时序修复: 尾部 head 前进诊断由"先睡后查 100ms"改为"先查后睡 100µs 粒度"(原固定 +100ms/launch
      封顶流水吞吐, 08 示例 7.87x 实为摊薄该睡眠的度量而非真实掩盖); pollSlot 退避 1ms→100µs 细化流水节拍;
      KVER bump v3(CP1 文件名缓存纪律), 缺省 launch json 与 README 同步 v3
- [x] 冗余清理: prep 死 include(cstring)/示例 kernel 死参数 physSize/STAGE 历史中间态枚举值/
      UT 宏 hack(#define private public)移除; 示例标志区魔数改设备头常量; ResetWorkspace 隐晦写法显式化
      (Result 面包屑字段与 timeline 诊断路径经评估保留)
- [x] 初始化冒烟自证移除（InitOnce 回归三步；spec 就绪门控语义同步；UT 注释适配方就绪门控表述）
- [x] 08 示例 AICore kernel 四角色合并为两角色(role2/3 删除, 单块=流水 chunks=1,
      host 两阶段共用 role4/5 声明上提; 角色头注释 v1 阶段编号同步修正)
- [x] 08 示例双 rank 会合改用 MPI_Barrier（rank 由 MPI 分配，去除 /tmp 文件轮询与
      工作目录管理，串行对照轮间会合语义不变、基线口径变纯，加速比需重新实机标定）
