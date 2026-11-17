
#include "../op_kernel/smem_mm_ar_310p3_tiling.h"
#include "register/op_def_registry.h"


namespace optiling {
static ge::graphStatus TilingFunc(gert::TilingContext* context)
{
  SmemMmAr310p3TilingData *tiling = context->GetTilingData<SmemMmAr310p3TilingData>();
  const auto* attrs = context->GetAttrs();
  if (attrs == nullptr) {
    return ge::GRAPH_FAILED;
  }
  const int64_t* v[13] = {};
  for (size_t i = 0; i < 13; ++i) {
    v[i] = attrs->GetInt(i);
    if (v[i] == nullptr) {
      return ge::GRAPH_FAILED;
    }
  }
  tiling->gva = static_cast<uint64_t>(v[0][0]);
  tiling->peerGva = static_cast<uint64_t>(v[1][0]);
  tiling->symm = static_cast<uint64_t>(v[2][0]);
  tiling->ibPtr = static_cast<uint64_t>(v[3][0]);
  tiling->statusPtr = static_cast<uint64_t>(v[4][0]);
  tiling->tracePtr = static_cast<uint64_t>(v[5][0]);
  tiling->rank = static_cast<uint32_t>(v[6][0]);
  tiling->epoch = static_cast<uint32_t>(v[7][0]);
  tiling->packet = static_cast<uint32_t>(v[8][0]);
  tiling->rows = static_cast<uint32_t>(v[9][0]);
  tiling->commRows = static_cast<uint32_t>(v[10][0]);
  tiling->aicCount = static_cast<uint32_t>(v[11][0]);
  tiling->enabled = static_cast<uint32_t>(v[12][0]);
  // 去 pad 契约：rows%256==0 + [1024,4096]（4096 = work 区 16MiB 上限），
  // 部分尾组/末子组原生支持；commRows 仅 1024/512 两档。
  if ((tiling->commRows != 1024 && tiling->commRows != 512) ||
      tiling->rows % 256 != 0 || tiling->rows < 1024 || tiling->rows > 4096 ||
      (tiling->rows + tiling->commRows - 1) / tiling->commRows > 8 ||
      tiling->aicCount != 8) {
    return ge::GRAPH_FAILED;
  }
  context->SetBlockDim(15);
  size_t *currentWorkspace = context->GetWorkspaceSizes(1);
  currentWorkspace[0] = 0;
  return ge::GRAPH_SUCCESS;
}
}


namespace ge {
static ge::graphStatus InferShape(gert::InferShapeContext* context)
{
    const gert::Shape* x_shape = context->GetInputShape(0);
    gert::Shape* y_shape = context->GetOutputShape(0);
    *y_shape = *x_shape;
    return GRAPH_SUCCESS;
}
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
        this->Attr("peerGva").AttrType(REQUIRED).Int();
        this->Attr("symm").AttrType(REQUIRED).Int();
        this->Attr("ibPtr").AttrType(REQUIRED).Int();
        this->Attr("statusPtr").AttrType(REQUIRED).Int();
        this->Attr("tracePtr").AttrType(REQUIRED).Int();
        this->Attr("rank").AttrType(REQUIRED).Int();
        this->Attr("epoch").AttrType(REQUIRED).Int();
        this->Attr("packet").AttrType(REQUIRED).Int();
        this->Attr("rows").AttrType(REQUIRED).Int();
        this->Attr("commRows").AttrType(REQUIRED).Int();
        this->Attr("aicCount").AttrType(REQUIRED).Int();
        this->Attr("enabled").AttrType(REQUIRED).Int();

        this->SetInferShape(ge::InferShape).SetInferDataType(ge::InferDataType);

        this->AICore()
            .SetTiling(optiling::TilingFunc);
        this->AICore().AddConfig("ascend310p");

    }
};

OP_ADD(SmemMmAr310p3);
}
