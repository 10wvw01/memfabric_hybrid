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
