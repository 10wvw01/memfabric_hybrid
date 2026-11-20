
#include "../op_kernel/smem_mm_ar_310p3_tiling.h"
#include "register/op_def_registry.h"


namespace optiling {
// TilingFunc：把宿主传入的 7 个 int64 属性原样转存进 tiling 结构，并做
// 协议契约校验（校验不过直接返回失败，kernel 不会被发射）。本算子的几何
// 参数全部来自宿主调用方（会话层已建好对称池），tiling 阶段不自行计算
// 切分，因此 workspace 尺寸为 0、BlockDim 固定为 15（8 个 Cube 核做矩阵乘
// + 7 个 Vector 核做通信调度与归约加法）。其余协议参数（SDMA 包长 1MiB、
// 通信组 1024 行、矩阵乘 8 核）已固化为 kernel 编译期常量，不再经属性传入。
// 校验项（常量同源于 tiling.h 布局推导段，唯一事实源）：
//   行数 kRowsAlign 对齐且在 [kMinRows, kMaxRows] 内；
//   池单侧 symm >= kPoolFloor（建池下限由算子自身规格推导，不满足在
//   tiling 期即拒，不再等到 kernel 内 Trap）。
static ge::graphStatus TilingFunc(gert::TilingContext* context)
{
  SmemMmAr310p3TilingData *tiling = context->GetTilingData<SmemMmAr310p3TilingData>();
  const auto* attrs = context->GetAttrs();
  if (attrs == nullptr) {
    return ge::GRAPH_FAILED;
  }
  const int64_t* v[7] = {};
  for (size_t i = 0; i < 7; ++i) {
    v[i] = attrs->GetInt(i);
    if (v[i] == nullptr) {
      return ge::GRAPH_FAILED;
    }
  }
  tiling->gva = static_cast<uint64_t>(v[0][0]);
  tiling->symm = static_cast<uint64_t>(v[1][0]);
  tiling->statusPtr = static_cast<uint64_t>(v[2][0]);
  tiling->tracePtr = static_cast<uint64_t>(v[3][0]);
  tiling->rank = static_cast<uint32_t>(v[4][0]);
  tiling->epoch = static_cast<uint32_t>(v[5][0]);
  tiling->rows = static_cast<uint32_t>(v[6][0]);
  // 去除填充的契约校验：末尾组允许不足整组行数。
  if (tiling->rows % kRowsAlign != 0 || tiling->rows < kMinRows
      || tiling->rows > kMaxRows) {
    return ge::GRAPH_FAILED;
  }
  // 建池下限校验：对称池单侧必须容纳完整布局（work + scratch + 控制区
  // + seats，见 tiling.h 布局推导段）。
  if (tiling->symm < kPoolFloor) {
    return ge::GRAPH_FAILED;
  }
  context->SetBlockDim(15);
  size_t *currentWorkspace = context->GetWorkspaceSizes(1);
  currentWorkspace[0] = 0;
  return ge::GRAPH_SUCCESS;
}
}


namespace ge {
// InferShape：输出 y 与输入 x 同形状（y = AllReduce(x @ w^T)，矩阵乘不改变行数，
// 列数 K 与 N 相等且都为 2048，故形状直接透传）。
static ge::graphStatus InferShape(gert::InferShapeContext* context)
{
    const gert::Shape* x_shape = context->GetInputShape(0);
    gert::Shape* y_shape = context->GetOutputShape(0);
    *y_shape = *x_shape;
    return GRAPH_SUCCESS;
}
// InferDataType：输出 y 与输入 x 同数据类型（fp16）。
static ge::graphStatus InferDataType(gert::InferDataTypeContext *context)
{
    const auto inputDataType = context->GetInputDataType(0);
    context->SetOutputDataType(0, inputDataType);
    return GRAPH_SUCCESS;
}
}

namespace ops {
// NOTE: OP_ADD 注册的算子类型名（ABI）= 类名字符串。CANN GA 运行时的 executor
// 注册表不支持带下划线的算子类型名（实测 snake_case 逐字 rc=161001），故 ABI
// 用 CamelCase：
//   op 类型 SmemMmAr310p3，aclnn 符号
//   aclnnSmemMmAr310p3 / aclnnSmemMmAr310p3GetWorkspaceSize。
// 目录/文件/kernel 入口（smem_mm_ar310p3）/torch op
//（npu_smem_mm_ar_310p3）/vendor（smem_mm_ar_310p3）保持 snake_case。
// 算子定义：x/w 为输入、y 为预分配输出，7 个 int64 属性承载对称池与协议
// 参数（含义见 smem_mm_ar_310p3_tiling.h 的字段注解；SDMA 包长/通信组
// 行数/矩阵乘核数为 kernel 编译期常量；组完成通知数组在池内 seats 区）。
class SmemMmAr310p3 : public OpDef {
public:
    explicit SmemMmAr310p3(const char* name) : OpDef(name)
    {
        this->Input("x")
            .ParamType(REQUIRED)
            .DataType({ge::DT_FLOAT16})
            .Format({ge::FORMAT_ND})
            .UnknownShapeFormat({ge::FORMAT_ND});
        this->Input("w")
            .ParamType(REQUIRED)
            .DataType({ge::DT_FLOAT16})
            .Format({ge::FORMAT_ND})
            .UnknownShapeFormat({ge::FORMAT_ND});
        this->Output("y")
            .ParamType(REQUIRED)
            .DataType({ge::DT_FLOAT16})
            .Format({ge::FORMAT_ND})
            .UnknownShapeFormat({ge::FORMAT_ND});

        this->Attr("gva").AttrType(REQUIRED).Int();
        this->Attr("symm").AttrType(REQUIRED).Int();
        this->Attr("statusPtr").AttrType(REQUIRED).Int();
        this->Attr("tracePtr").AttrType(REQUIRED).Int();
        this->Attr("rank").AttrType(REQUIRED).Int();
        this->Attr("epoch").AttrType(REQUIRED).Int();
        this->Attr("rows").AttrType(REQUIRED).Int();

        this->SetInferShape(ge::InferShape).SetInferDataType(ge::InferDataType);

        this->AICore()
            .SetTiling(optiling::TilingFunc);
        this->AICore().AddConfig("ascend310p");

    }
};

OP_ADD(SmemMmAr310p3);
}
