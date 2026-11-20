
#ifndef TORCHNPU_TORCH_NPU_CSRC_ATEN_OPS_OP_API_PTA_COMMON_H_
#define TORCHNPU_TORCH_NPU_CSRC_ATEN_OPS_OP_API_PTA_COMMON_H_

// ops_common.h：自定义算子 PyTorch 适配层的公共机制，源自 torch_npu 插件
// 的 op_api PTA 模板。本文件与具体算子无关，提供三块能力：
//   1. aclnn 符号解析——GetOpApiFuncAddr 按优先级在自定义算子包、默认
//      vendors 目录、CANN 特性库、libopapi.so 中查找符号；
//   2. 参数类型转换——ConvertType 重载族把 at::Tensor/at::Scalar/int64_t 等
//      转成 aclTensor/aclScalar 等 ACL 类型，Release 族负责析构；
//   3. 两段式发射宏 EXEC_NPU_CMD_V1——先调 GetWorkspaceSize 查询工作区，
//      再异步下发真正的 aclnn 调用（与 torch_npu 内置算子的下发路径一致）。
// 算子自己的宿主入口在 npu_smem_mm_ar_310p3.cpp，只调用本文件的宏。

#include <fstream>
#include <sys/stat.h>
#include <dlfcn.h>
#include <vector>
#include <functional>
#include <type_traits>
#include <ATen/Tensor.h>
#include <ATen/NamedTensorUtils.h>
#include <acl/acl_base.h>
#include <acl/acl_rt.h>
#include <c10/util/Exception.h>
#include <torch/extension.h>

#include "torch_npu/csrc/aten/CustomFunctions.h"
#include "torch_npu/csrc/aten/NPUNativeFunctions.h"
#include "torch_npu/csrc/core/npu/NPUStream.h"
#include "torch_npu/csrc/core/npu/NPUFunctions.h"
#include "torch_npu/csrc/core/npu/NpuVariables.h"
#include "torch_npu/csrc/core/npu/NPUFormat.h"
#include "torch_npu/csrc/core/npu/register/OptionsManager.h"
#include "torch_npu/csrc/framework/OpCommand.h"
#include <torch_npu/csrc/framework/utils/CalcuOpUtil.h>
#include <torch_npu/csrc/framework/utils/OpAdapter.h>
#include "torch_npu/csrc/framework/utils/OpPreparation.h"
#include "torch_npu/csrc/framework/utils/RandomOpAdapter.h"
#include "torch_npu/csrc/framework/interface/AclOpCompileInterface.h"
#include "torch_npu/csrc/framework/interface/EnvVariables.h"
#include "torch_npu/csrc/flopcount/FlopCount.h"
#include "torch_npu/csrc/flopcount/FlopCounter.h"


typedef struct aclOpExecutor aclOpExecutor;
typedef struct aclTensor aclTensor;
typedef struct aclScalar aclScalar;
typedef struct aclIntArray aclIntArray;
typedef struct aclFloatArray aclFloatArray;
typedef struct aclBoolArray aclBoolArray;
typedef struct aclTensorList aclTensorList;
typedef struct aclScalarList aclScalarList;

typedef aclTensor *(*_aclCreateTensor)(const int64_t *view_dims, uint64_t view_dims_num, aclDataType data_type,
                                       const int64_t *stride, int64_t offset, aclFormat format,
                                       const int64_t *storage_dims, uint64_t storage_dims_num, void *tensor_data);
typedef aclScalar *(*_aclCreateScalar)(void *value, aclDataType data_type);
typedef aclIntArray *(*_aclCreateIntArray)(const int64_t *value, uint64_t size);
typedef aclFloatArray *(*_aclCreateFloatArray)(const float *value, uint64_t size);
typedef aclBoolArray *(*_aclCreateBoolArray)(const bool *value, uint64_t size);
typedef aclTensorList *(*_aclCreateTensorList)(const aclTensor *const *value, uint64_t size);
typedef aclScalarList *(*_aclCreateScalarList)(const aclScalar *const *value, uint64_t size);

// ---- aclCreate/aclDestroy 函数指针类型 ----
// acl 层对象不进 ABI 头，这里按签名手工声明函数指针，再经 dlsym 取地址。
typedef int (*_aclDestroyTensor)(const aclTensor *tensor);
typedef int (*_aclDestroyScalar)(const aclScalar *scalar);
typedef int (*_aclDestroyIntArray)(const aclIntArray *array);
typedef int (*_aclDestroyFloatArray)(const aclFloatArray *array);
typedef int (*_aclDestroyBoolArray)(const aclBoolArray *array);
typedef int (*_aclDestroyTensorList)(const aclTensorList *array);
typedef int (*_aclDestroyScalarList)(const aclScalarList *array);

using OpApiFunc = int (*)(void *, uint64_t, aclOpExecutor *, const aclrtStream);


// ---- 常量与环境变量 ----
const int N = 32;
// SmallVector 预留的形状维度容量
const int SIZE = 8;

namespace {
  constexpr int64_t MAX_DIM_NUM = 5;
  constexpr int64_t NCL_DIM_NUM = 3;
  constexpr int64_t NCHW_DIM_NUM = 4;
  constexpr int64_t NCDHW_DIM_NUM = 5;
}

// ---- at::ScalarType 到 aclDataType 的映射表 ----
// BuildATenToAclTable：按 at::ScalarType 枚举下标建查表，torch 数据类型
// 一一映射到 ACL 数据类型；ACL 没有对应类型的填 ACL_DT_UNDEFINED，
// 后续 ConvertType 会把未定义类型的张量直接拒绝掉。
inline const std::array<aclDataType, static_cast<size_t>(at::ScalarType::NumOptions) + 1>
BuildATenToAclTable(){
  std::array<aclDataType, static_cast<size_t>(at::ScalarType::NumOptions) + 1> t{};
  t.fill(ACL_DT_UNDEFINED);
  t[static_cast<size_t>(at::ScalarType::Byte)]            = ACL_UINT8;       
  t[static_cast<size_t>(at::ScalarType::Char)]            = ACL_INT8;        
  t[static_cast<size_t>(at::ScalarType::Short)]           = ACL_INT16;       
  t[static_cast<size_t>(at::ScalarType::Int)]             = ACL_INT32;       
  t[static_cast<size_t>(at::ScalarType::Long)]            = ACL_INT64;       
  t[static_cast<size_t>(at::ScalarType::Half)]            = ACL_FLOAT16;     
  t[static_cast<size_t>(at::ScalarType::Float)]           = ACL_FLOAT;       
  t[static_cast<size_t>(at::ScalarType::Double)]          = ACL_DOUBLE;      
  t[static_cast<size_t>(at::ScalarType::ComplexHalf)]     = ACL_DT_UNDEFINED;
  t[static_cast<size_t>(at::ScalarType::ComplexFloat)]    = ACL_COMPLEX64;   
  t[static_cast<size_t>(at::ScalarType::ComplexDouble)]   = ACL_COMPLEX128;
  t[static_cast<size_t>(at::ScalarType::Bool)]            = ACL_BOOL;
  t[static_cast<size_t>(at::ScalarType::QInt8)]           = ACL_DT_UNDEFINED;
  t[static_cast<size_t>(at::ScalarType::QUInt8)]          = ACL_DT_UNDEFINED;
  t[static_cast<size_t>(at::ScalarType::QInt32)]          = ACL_DT_UNDEFINED;
  t[static_cast<size_t>(at::ScalarType::BFloat16)]        = ACL_BF16;
  t[static_cast<size_t>(at::ScalarType::QUInt4x2)]        = ACL_DT_UNDEFINED;
  t[static_cast<size_t>(at::ScalarType::QUInt2x4)]        = ACL_DT_UNDEFINED;
  t[static_cast<size_t>(at::ScalarType::UInt16)]          = ACL_UINT16;
  t[static_cast<size_t>(at::ScalarType::UInt64)]          = ACL_UINT64;
  t[static_cast<size_t>(at::ScalarType::Undefined)]       = ACL_DT_UNDEFINED;
  t[static_cast<size_t>(at::ScalarType::NumOptions)]      = ACL_DT_UNDEFINED;
  return t;
}

inline const auto kATenScalarTypeToAclDataTypeTable = BuildATenToAclTable();

// ---- 路径与动态库辅助函数 ----
// split_str：按分隔符切分字符串（用于切分路径列表）。
static std::vector<std::string> split_str(std::string s, const std::string &del)
{
    int end = s.find(del);
    std::vector<std::string> path_list;
    while (end != -1) {
        path_list.push_back(s.substr(0, end));
        s.erase(s.begin(), s.begin() + end + 1);
        end = s.find(del);
    }
    path_list.push_back(s);
    return path_list;
}

// is_file_exist：判断路径是否存在（含空串与超长保护）。
static bool is_file_exist(const std::string &path)
{
    if (path.empty() || path.size() > PATH_MAX) {
        return false;
    }
    return (access(path.c_str(), F_OK) == 0) ? true : false;
}

// real_path：解析符号链接得到绝对路径；解析失败返回空串。
inline  std::string real_path(const std::string &path)
{
    if (path.empty() || path.size() > PATH_MAX) {
        return "";
    }
    char realPath[PATH_MAX] = {0};
    if (realpath(path.c_str(), realPath) == nullptr) {
        return "";
    }
    return std::string(realPath);
}

// get_custom_lib_path：从 ASCEND_CUSTOM_OPP_PATH 环境变量生成自定义算子包
// 的 op_api/lib 目录列表（变量支持冒号分隔的多路径）。未配置则返回空表。
inline std::vector<std::string> get_custom_lib_path()
{
    char *ascend_custom_opppath = std::getenv("ASCEND_CUSTOM_OPP_PATH");
    std::vector<std::string> custom_lib_path_list;

    if (ascend_custom_opppath == NULL) {
        ASCEND_LOGW("ASCEND_CUSTOM_OPP_PATH is not exists");
        return std::vector<std::string>();
    }

    std::string ascend_custom_opppath_str(ascend_custom_opppath);
    // 按冒号切分为多个路径
    custom_lib_path_list = split_str(ascend_custom_opppath_str, ":");
    if (custom_lib_path_list.empty()) {
        return std::vector<std::string>();
    }
    // 每个路径追加算子包 op_api 库的固定子目录
    for (auto &it : custom_lib_path_list) {
        it = it + "/op_api/lib/";
    }

    return custom_lib_path_list;
}

// get_default_custom_lib_path：读取 CANN 默认 vendors 目录的 config.ini，
// 按 load_priority 配置项生成默认加载顺序下的 op_api/lib 路径列表。
inline std::vector<std::string> get_default_custom_lib_path()
{
    char *ascend_opp_path = std::getenv("ASCEND_OPP_PATH");
    std::vector<std::string> default_vendors_list;

    if (ascend_opp_path == NULL) {
        ASCEND_LOGW("ASCEND_OPP_PATH is not exists");
        return std::vector<std::string>();
    }

    std::string vendors_path(ascend_opp_path);
    vendors_path = vendors_path + "/vendors";
    std::string vendors_config_file = real_path(vendors_path + "/config.ini");
    if (vendors_config_file.empty()) {
        ASCEND_LOGW("config.ini is not exists");
        return std::vector<std::string>();
    }

    if (!is_file_exist(vendors_config_file)) {
        ASCEND_LOGW("config.ini is not exists or the path length is more than %d", PATH_MAX);
        return std::vector<std::string>();
    }

    std::ifstream ifs(vendors_config_file);
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.find("load_priority=") == 0) {
            break;
        }
    }
    std::string head = "load_priority=";
    line.erase(0, head.length());

    // 按逗号切分加载优先级名单
    default_vendors_list = split_str(line, ",");
    if (default_vendors_list.empty()) {
        return std::vector<std::string>();
    }
    for (auto &it : default_vendors_list) {
        it = real_path(vendors_path + "/" + it + "/op_api/lib/");
    }

    return default_vendors_list;
}

extern const std::vector<std::string> g_custom_lib_path;
extern const std::vector<std::string> g_default_custom_lib_path;
void *GetOpApiFuncAddrFromFeatureLib(const char *api_name);


// ---- 动态库符号查找 ----
// GetOpApiLibName / GetCustOpApiLibName：CANN 主 opapi 库与自定义算子包
// 库的文件名。
inline const char *GetOpApiLibName(void)
{
    return "libopapi.so";
}

inline const char *GetCustOpApiLibName(void)
{
    return "libcust_opapi.so";
}

// GetOpApiFuncAddrInLib：在已打开的动态库句柄里按符号名取函数地址，
// 失败时记告警日志。
inline void *GetOpApiFuncAddrInLib(void *handler, const char *libName, const char *apiName)
{
    auto funcAddr = dlsym(handler, apiName);
    if (funcAddr == nullptr) {
        ASCEND_LOGW("dlsym %s from %s failed, error:%s.", apiName, libName, dlerror());
    }
    return funcAddr;
}

// GetOpApiLibHandler：按库名打开动态库（懒加载模式），失败时记告警日志。
inline void *GetOpApiLibHandler(const char *libName)
{
    auto handler = dlopen(libName, RTLD_LAZY);
    if (handler == nullptr) {
        ASCEND_LOGW("dlopen %s failed, error:%s.", libName, dlerror());
    }
    return handler;
}


// GET_OP_API_FUNC_FROM_FEATURE_LIB：打开指定特性库并在其中查找符号，
// 找到即返回；每个库的句柄只打开一次（static）。
#define GET_OP_API_FUNC_FROM_FEATURE_LIB(lib_handler, lib_name, api_name)                                              \
    static auto lib_handler = GetOpApiLibHandler(lib_name);                                                            \
    if ((lib_handler) != nullptr) {                                                                                    \
        auto funcAddr = GetOpApiFuncAddrInLib(lib_handler, lib_name, api_name);                                        \
        if (funcAddr != nullptr) {                                                                                     \
            return funcAddr;                                                                                           \
        }                                                                                                              \
    }

// GET_OP_API_FUNC：把符号名解析成对应签名的函数指针（签名须先用
// typedef _##apiName 声明过）。
#define GET_OP_API_FUNC(apiName) \
  reinterpret_cast<_##apiName>(GetOpApiFuncAddr(#apiName))

// GetOpApiFuncAddr：aclnn 符号解析主入口，按以下优先级逐层查找，
// 任一层命中即返回：
//   1. ASCEND_CUSTOM_OPP_PATH 指定的自定义算子包（libcust_opapi.so）
//   2. CANN 默认 vendors 目录（按 config.ini 的加载优先级）
//   3. CANN 特性库（libopapi_math/nn/cv/transformer/legacy.so）
//   4. CANN 主库 libopapi.so
//   5. ops_common.cpp 里的其余特性库（infer/train/sparse/fft/rand）
// 全部未命中返回空指针，调用方负责判空。
inline void *GetOpApiFuncAddr(const char *apiName)
{
    if (!g_custom_lib_path.empty()) {
        for (auto &it : g_custom_lib_path) {
            auto cust_opapi_lib = real_path(it + "/" + GetCustOpApiLibName());
            if (cust_opapi_lib.empty()) {
                continue;
            }
            auto custOpApiHandler = GetOpApiLibHandler(cust_opapi_lib.c_str());
            if (custOpApiHandler != nullptr) {
                auto funcAddr =
                    GetOpApiFuncAddrInLib(custOpApiHandler, GetCustOpApiLibName(), apiName);
                if (funcAddr != nullptr) {
                    ASCEND_LOGI("%s is found in %s.", apiName, cust_opapi_lib.c_str());
                    return funcAddr;
                }
            }
        }
        ASCEND_LOGI("%s is not in custom lib.", apiName);
    }

    if (!g_default_custom_lib_path.empty()) {
        for (auto &it : g_default_custom_lib_path) {
            auto default_cust_opapi_lib = real_path(it + "/" + GetCustOpApiLibName());
            if (default_cust_opapi_lib.empty()) {
                continue;
            }
            auto custOpApiHandler = GetOpApiLibHandler(default_cust_opapi_lib.c_str());
            if (custOpApiHandler != nullptr) {
                auto funcAddr =
                    GetOpApiFuncAddrInLib(custOpApiHandler, GetCustOpApiLibName(), apiName);
                if (funcAddr != nullptr) {
                    ASCEND_LOGI("%s is found in %s.", apiName, default_cust_opapi_lib.c_str());
                    return funcAddr;
                }
            }
        }
        ASCEND_LOGI("%s is not in default custom lib.", apiName);
    }

    GET_OP_API_FUNC_FROM_FEATURE_LIB(opapiMathHandler, "libopapi_math.so", apiName);
    GET_OP_API_FUNC_FROM_FEATURE_LIB(opapiNnHandler, "libopapi_nn.so", apiName);
    GET_OP_API_FUNC_FROM_FEATURE_LIB(opapiCvHandler, "libopapi_cv.so", apiName);
    GET_OP_API_FUNC_FROM_FEATURE_LIB(opapiTransformerHandler, "libopapi_transformer.so", apiName);
    GET_OP_API_FUNC_FROM_FEATURE_LIB(opapiLegacyHandler, "libopapi_legacy.so", apiName);

    static auto opApiHandler = GetOpApiLibHandler(GetOpApiLibName());
    if (opApiHandler != nullptr) {
        auto funcAddr = GetOpApiFuncAddrInLib(opApiHandler, GetOpApiLibName(), apiName);
        if (funcAddr != nullptr) {
            return funcAddr;
        }
    }
    return GetOpApiFuncAddrFromFeatureLib(apiName);
}


// ---- 参数搬运与类型转换 ----
// CopyTensorHostToDevice：CPU 张量经页锁定内存复制到 NPU。
inline at::Tensor CopyTensorHostToDevice(const at::Tensor &cpu_tensor) {
  at::Tensor cpuPinMemTensor = cpu_tensor.pin_memory();
  int deviceIndex = 0;
  return cpuPinMemTensor.to(c10::Device(torch_npu::utils::get_npu_device_type(), deviceIndex),
                            cpuPinMemTensor.scalar_type(), true, true);
}

// CopyScalarToDevice：标量先转张量再复制到 NPU（包装标量场景使用）。
inline at::Tensor CopyScalarToDevice(const c10::Scalar &cpu_scalar,
                                     at::ScalarType scalar_data_type) {
  return CopyTensorHostToDevice(
      scalar_to_tensor(cpu_scalar).to(scalar_data_type));
}


// ConvertType(aclTensor*)：手工构造好的 aclTensor 直接透传
//（例如 npu_smem_mm_ar_310p3.cpp 里“ND 标记 + NZ 存储”的 w 张量）。
// 所有权归 EXEC_NPU_CMD_V1 的 ReleaseConvertTypes（内部调 aclDestroyTensor）。
inline aclTensor *ConvertType(aclTensor *t) { return t; }

// ConvertType(const at::Tensor&)：torch 张量转 aclTensor。
// 处理三件事：① 数据类型经映射表转 ACL 类型（无对应类型则报错）；
// ② 布局——NPU 张量按其物理格式（2=ND、29=FRACTAL_NZ）填写，CPU 张量
// 按维数猜 NCL/NCHW/NCDHW；③ 包装标量（0 维数字）先复制上设备再构造。
inline aclTensor *ConvertType(const at::Tensor &at_tensor) {
  static const auto aclCreateTensor = GET_OP_API_FUNC(aclCreateTensor);
  if (aclCreateTensor == nullptr) {
    return nullptr;
  }

  if (!at_tensor.defined()) {
    return nullptr;
  }
  at::ScalarType scalar_data_type = at_tensor.scalar_type();
  aclDataType acl_data_type =
      kATenScalarTypeToAclDataTypeTable[static_cast<int64_t>(scalar_data_type)];
  TORCH_CHECK(
      acl_data_type != ACL_DT_UNDEFINED,
      std::string(c10::toString(scalar_data_type)) + " has not been supported")
  c10::SmallVector<int64_t, 5> storageDims;
  // 存储维度按“存储字节数 / 单元素字节数”填写；ACL_STRING 无存储维度。
  auto itemsize = at_tensor.itemsize();
  if (itemsize == 0) {
    AT_ERROR("When ConvertType, tensor item size of cannot be zero.");
    return nullptr;
  }
  if (acl_data_type != ACL_STRING) {
    storageDims.push_back(at_tensor.storage().nbytes() / itemsize);
  }

  const auto dimNum = at_tensor.sizes().size();
  aclFormat format = ACL_FORMAT_ND;
  // NPU 张量：按物理格式标记（2=ND、29=FRACTAL_NZ），其余一律按 ND。
  if (torch_npu::utils::is_npu(at_tensor)) {
    auto npu_format = at_npu::native::get_npu_format(at_tensor);
    switch (npu_format) {
      case 2:
        format = ACL_FORMAT_ND;
        break;
      case 29:
        format = ACL_FORMAT_FRACTAL_NZ;
        break;
      default:
        format = ACL_FORMAT_ND;
    }
  } else {
    // CPU 张量：没有物理格式信息，按维数对应 NCL/NCHW/NCDHW。
    switch (dimNum) {
      case 3:
        format = ACL_FORMAT_NCL;
        break;
      case 4:
        format = ACL_FORMAT_NCHW;
        break;
      case 5:
        format = ACL_FORMAT_NCDHW;
        break;
      default:
        format = ACL_FORMAT_ND;
    }
  }

  if (at_tensor.unsafeGetTensorImpl()->is_wrapped_number()) {
    // 包装标量：torch 把 0 维数字包装成张量传入，先复制上设备再构造。
    c10::Scalar expScalar = at_tensor.item();
    at::Tensor aclInput = CopyScalarToDevice(expScalar, scalar_data_type);
    return aclCreateTensor(aclInput.sizes().data(), aclInput.sizes().size(),
                           acl_data_type, aclInput.strides().data(),
                           aclInput.storage_offset(), format,
                           storageDims.data(), storageDims.size(),
                           const_cast<void *>(aclInput.storage().data()));
  }

  auto acl_tensor = aclCreateTensor(
      at_tensor.sizes().data(), at_tensor.sizes().size(), acl_data_type,
      at_tensor.strides().data(), at_tensor.storage_offset(), format,
      storageDims.data(), storageDims.size(),
      const_cast<void *>(at_tensor.storage().data()));
  return acl_tensor;
}

// ConvertType(at::Scalar)：标量转 aclScalar，仅支持 double/long/bool/
// complex-double 四种（足够覆盖本算子用到的类型），其余返回空。
inline aclScalar *ConvertType(const at::Scalar &at_scalar) {
  static const auto aclCreateScalar = GET_OP_API_FUNC(aclCreateScalar);
  if (aclCreateScalar == nullptr) {
    return nullptr;
  }

  at::ScalarType scalar_data_type = at_scalar.type();
  aclDataType acl_data_type =
      kATenScalarTypeToAclDataTypeTable[static_cast<int64_t>(scalar_data_type)];
  TORCH_CHECK(
      acl_data_type != ACL_DT_UNDEFINED,
      std::string(c10::toString(scalar_data_type)) + " has not been supported")
  aclScalar *acl_scalar = nullptr;
  switch (scalar_data_type) {
    case at::ScalarType::Double: {
      double value = at_scalar.toDouble();
      acl_scalar = aclCreateScalar(&value, acl_data_type);
      break;
    }
    case at::ScalarType::Long: {
      int64_t value = at_scalar.toLong();
      acl_scalar = aclCreateScalar(&value, acl_data_type);
      break;
    }
    case at::ScalarType::Bool: {
      bool value = at_scalar.toBool();
      acl_scalar = aclCreateScalar(&value, acl_data_type);
      break;
    }
    case at::ScalarType::ComplexDouble: {
      auto value = at_scalar.toComplexDouble();
      acl_scalar = aclCreateScalar(&value, acl_data_type);
      break;
    }
    default:
      acl_scalar = nullptr;
      break;
  }
  return acl_scalar;
}

// ---- ConvertType 其余重载：数组/列表/可选值/标量值 ----
// IntArrayRef 与 SymInt 数组转 aclIntArray。
inline aclIntArray *ConvertType(const at::IntArrayRef &at_array) {
  static const auto aclCreateIntArray = GET_OP_API_FUNC(aclCreateIntArray);
  if (aclCreateIntArray == nullptr) {
    return nullptr;
  }
  auto array = aclCreateIntArray(at_array.data(), at_array.size());
  return array;
}

// 固定长度 bool 数组转 aclBoolArray。
template <std::size_t N>
inline aclBoolArray *ConvertType(const std::array<bool, N> &value) {
  static const auto aclCreateBoolArray = GET_OP_API_FUNC(aclCreateBoolArray);
  if (aclCreateBoolArray == nullptr) {
    return nullptr;
  }

  auto array = aclCreateBoolArray(value.data(), value.size());
  return array;
}

// 变长 bool 数组转 aclBoolArray。
inline aclBoolArray *ConvertType(const at::ArrayRef<bool> &value) {
  static const auto aclCreateBoolArray = GET_OP_API_FUNC(aclCreateBoolArray);
  if (aclCreateBoolArray == nullptr) {
    return nullptr;
  }

  auto array = aclCreateBoolArray(value.data(), value.size());
  return array;
}

// SymInt 数组（torch.compile 符号形状）先落成具体 int 数组再转 aclIntArray。
inline aclIntArray *ConvertType(const at::ArrayRef<c10::SymInt> &at_array)
{
    static const auto aclCreateIntArray = GET_OP_API_FUNC(aclCreateIntArray);
    if (aclCreateIntArray == nullptr) {
        return nullptr;
    }
    auto int_array = c10::asIntArrayRefUnchecked(at_array);
    auto array = aclCreateIntArray(int_array.data(), int_array.size());
    return array;
}

// 张量列表：逐个转 aclTensor 后打包成 aclTensorList。
inline aclTensorList *ConvertType(const at::TensorList &at_tensor_list) {
  static const auto aclCreateTensorList = GET_OP_API_FUNC(aclCreateTensorList);
  if (aclCreateTensorList == nullptr) {
    return nullptr;
  }

  std::vector<const aclTensor *> tensor_list(at_tensor_list.size());
  for (size_t i = 0; i < at_tensor_list.size(); i++) {
    tensor_list[i] = ConvertType(at_tensor_list[i]);
  }
  auto acl_tensor_list =
      aclCreateTensorList(tensor_list.data(), tensor_list.size());
  return acl_tensor_list;
}

// 标量列表：逐个转 aclScalar 后打包成 aclScalarList。
inline aclScalarList *ConvertType(const at::ArrayRef<at::Scalar> &at_scalar_list)
{
    static const auto aclCreateScalarList = GET_OP_API_FUNC(aclCreateScalarList);
    if (aclCreateScalarList == nullptr) {
        return nullptr;
    }

    std::vector<const aclScalar *> scalar_list(at_scalar_list.size());
    for (size_t i = 0; i < at_scalar_list.size(); i++) {
        scalar_list[i] = ConvertType(at_scalar_list[i]);
    }
    auto acl_scalar_list = aclCreateScalarList(scalar_list.data(), scalar_list.size());
    return acl_scalar_list;
}

// ---- optional/平凡类型的转换 ----
// 有值的 optional 解包后按内部类型转换，空值统一转成空指针（ACL 用
// 空指针表示“参数未传”）。
inline aclTensor *ConvertType(const c10::optional<at::Tensor> &opt_tensor) {
  if (opt_tensor.has_value() && opt_tensor.value().defined()) {
    return ConvertType(opt_tensor.value());
  }

  return nullptr;
}

inline aclIntArray *ConvertType(const c10::optional<at::IntArrayRef> &opt_array) {
  if (opt_array.has_value()) {
    return ConvertType(opt_array.value());
  }
  return nullptr;
}

inline aclScalar *ConvertType(const c10::optional<at::Scalar> &opt_scalar) {
  if (opt_scalar.has_value()) {
    return ConvertType(opt_scalar.value());
  }
  return nullptr;
}


inline aclIntArray *ConvertType(const c10::OptionalArrayRef<c10::SymInt> &opt_array)
{
    if (opt_array.has_value()) {
        return ConvertType(opt_array.value());
    }

    return nullptr;
}

// ---- 平凡类型直接透传 ----
// char*/int64_t/float 本身就是 ACL 接口形参类型，原样返回。
inline char *ConvertType(char *ptr) {
    return ptr;
}

inline int64_t ConvertType(int64_t value) {
    return value;
}

inline float ConvertType(float value) {
    return value;
}

// at::ScalarType 经映射表转 aclDataType。
inline aclDataType ConvertType(const at::ScalarType scalarType) {
  return kATenScalarTypeToAclDataTypeTable[static_cast<int64_t>(scalarType)];
}


// ---- TensorWrapper：携带显式 ACL 数据类型的张量包装 ----
// 某些算子需要 torch 类型与 ACL 类型不一致（如 bf16 张量按 fp16 传），
// 用包装结构把目标类型一并传进来。
typedef struct {
    const at::Tensor& tensor_;
    aclDataType dtype;
} TensorWrapper;

typedef struct {
    const at::TensorList& tensor_list_;
    aclDataType dtype;
} TensorListWrapper;


c10::SmallVector<int64_t, SIZE> array_to_small_vector(c10::IntArrayRef shape);

// ConvertType(TensorWrapper)：按包装里指定的 ACL 类型构造 aclTensor。
// 布局处理：非基础格式（如 NZ）的 NPU 张量直接取其物理格式描述与存储
// 维度；基础格式按维数对应 NCL/NCHW/NCDHW。
inline aclTensor *ConvertType(const TensorWrapper &tensor_r)
{
    static const auto aclCreateTensor = GET_OP_API_FUNC(aclCreateTensor);
    if (aclCreateTensor == nullptr) {
        return nullptr;
    }

    const at::Tensor &at_tensor = tensor_r.tensor_;

    if (!at_tensor.defined()) {
        return nullptr;
    }
    TORCH_CHECK(torch_npu::utils::is_npu(at_tensor),
        "Expected all tensors to be on the same device. "
        "Expected NPU tensor, please check whether the input tensor device is correct.",
        OPS_ERROR(ErrCode::TYPE));

    aclDataType acl_data_type = tensor_r.dtype;
    c10::SmallVector<int64_t, MAX_DIM_NUM> storageDims;
    c10::SmallVector<int64_t, MAX_DIM_NUM> wrapperStride = array_to_small_vector(at_tensor.strides());
    c10::SmallVector<int64_t, MAX_DIM_NUM> wrapperShape = array_to_small_vector(at_tensor.sizes());

    const auto dimNum = at_tensor.sizes().size();
    aclFormat format = ACL_FORMAT_ND;
    if (!at_npu::native::FormatHelper::IsOpInputBaseFormat(at_tensor)) {
        // 非基础格式（FRACTAL_NZ 等）：直接取张量自带的格式与存储维度描述。
        format = torch_npu::NPUBridge::GetNpuStorageImpl(at_tensor)->npu_desc_.npu_format_;
        // ACL_STRING 类型没有存储维度。
        if (acl_data_type != ACL_STRING) {
            TORCH_CHECK(at_tensor.itemsize() > 0, "the itemsize of tensor must be greater than 0.",
                OPS_ERROR(ErrCode::VALUE));
            storageDims = torch_npu::NPUBridge::GetNpuStorageImpl(at_tensor)->npu_desc_.storage_sizes_;
        }
    } else {
        // 基础格式：按维数对应 NCL/NCHW/NCDHW。
        switch (dimNum) {
            case NCL_DIM_NUM:
                format = ACL_FORMAT_NCL;
                break;
            case NCHW_DIM_NUM:
                format = ACL_FORMAT_NCHW;
                break;
            case NCDHW_DIM_NUM:
                format = ACL_FORMAT_NCDHW;
                break;
            default:
                format = ACL_FORMAT_ND;
        }
        // ACL_STRING 类型没有存储维度。
        if (acl_data_type != ACL_STRING) {
            TORCH_CHECK(at_tensor.itemsize() > 0, "the itemsize of tensor must be greater than 0.",
                        OPS_ERROR(ErrCode::VALUE));
            storageDims.push_back(at_tensor.storage().nbytes() / at_tensor.itemsize());
        }
    }

    auto acl_tensor =
        aclCreateTensor(wrapperShape.data(), at_tensor.sizes().size(), acl_data_type, wrapperStride.data(),
                        at_tensor.storage_offset(), format, storageDims.data(), storageDims.size(),
                        const_cast<void *>(at_tensor.storage().data()));
    return acl_tensor;
}

// ConvertType(TensorListWrapper)：按包装类型逐个转换后打包成 aclTensorList。
inline aclTensorList *ConvertType(const TensorListWrapper &tensor_list_wrapper)
{
    static const auto aclCreateTensorList = GET_OP_API_FUNC(aclCreateTensorList);
    if (aclCreateTensorList == nullptr) {
        return nullptr;
    }

    std::vector<const aclTensor *> tensor_list(tensor_list_wrapper.tensor_list_.size());
    for (size_t i = 0; i < tensor_list.size(); i++) {
        tensor_list[i] = ConvertType(TensorWrapper{
            tensor_list_wrapper.tensor_list_[i], tensor_list_wrapper.dtype});
    }
    auto acl_tensor_list = aclCreateTensorList(tensor_list.data(), tensor_list.size());
    return acl_tensor_list;
}


// 泛型兜底：其余类型（枚举、整数等）不需要转换，原样透传。
template <typename T>
T ConvertType(T value) {
  return value;
}


// ---- 两段式调用的函数签名适配 ----
// ConvertToOpApiFunc：把“参数元组的各成员类型”展开成 aclnn 函数指针的
// 形参列表（GetWorkspaceSize 与真正调用的签名相同），返回转换后的
// 函数指针。两次重载只是把 index_sequence 展开封装掉。
template <typename Tuple, size_t... I>
auto ConvertToOpApiFunc(const Tuple &params, void *opApiAddr,
                        std::index_sequence<I...>) {
  typedef int (*OpApiFunc)(
      typename std::decay<decltype(std::get<I>(params))>::type...);
  auto func = reinterpret_cast<OpApiFunc>(opApiAddr);
  return func;
}

template <typename Tuple>
auto ConvertToOpApiFunc(const Tuple &params, void *opApiAddr) {
  static constexpr auto size = std::tuple_size<Tuple>::value;
  return ConvertToOpApiFunc(params, opApiAddr,
                            std::make_index_sequence<size>{});
}


// ---- Release 族：ACL 对象的析构 ----
// 每个 aclCreate 出来的对象在 EXEC_NPU_CMD_V1 调用完成后必须销毁；
// 平凡类型（int64_t 等）的 Release 是空操作（模板兜底在下方）。
inline void Release(aclTensor *p) {
  static const auto aclDestroyTensor = GET_OP_API_FUNC(aclDestroyTensor);
  if (aclDestroyTensor == nullptr) {
    return;
  }
  aclDestroyTensor(p);
}

inline void Release(aclScalar *p) {
  static const auto aclDestroyScalar = GET_OP_API_FUNC(aclDestroyScalar);
  if (aclDestroyScalar == nullptr) {
    return;
  }
  aclDestroyScalar(p);
}

inline void Release(aclIntArray *p) {
  static const auto aclDestroyIntArray = GET_OP_API_FUNC(aclDestroyIntArray);
  if (aclDestroyIntArray == nullptr) {
    return;
  }

  aclDestroyIntArray(p);
}

inline void Release(aclBoolArray *p) {
  static const auto aclDestroyBoolArray = GET_OP_API_FUNC(aclDestroyBoolArray);
  if (aclDestroyBoolArray == nullptr) {
    return;
  }

  aclDestroyBoolArray(p);
}

inline void Release(aclTensorList *p) {
  static const auto aclDestroyTensorList =
      GET_OP_API_FUNC(aclDestroyTensorList);
  if (aclDestroyTensorList == nullptr) {
    return;
  }

  aclDestroyTensorList(p);
}

inline void Release(aclScalarList *p)
{
    static const auto aclDestroyScalarList = GET_OP_API_FUNC(aclDestroyScalarList);
    if (aclDestroyScalarList == nullptr) {
        return;
    }

    aclDestroyScalarList(p);
}

template <typename T>
void Release(T value) {
  (void)value;
}

template <typename Tuple, size_t... I>
void CallRelease(Tuple t, std::index_sequence<I...>) {
  (void)std::initializer_list<int>{(Release(std::get<I>(t)), 0)...};
}

// ReleaseConvertTypes：把参数元组里的每个成员逐个 Release。
// EXEC_NPU_CMD_V1 的真正调用完成后由它统一回收所有 acl 对象。
template <typename Tuple>
void ReleaseConvertTypes(Tuple &t) {
  static constexpr auto size = std::tuple_size<Tuple>::value;
  CallRelease(t, std::make_index_sequence<size>{});
}

// ConvertTypes：把本次调用的全部实参逐个 ConvertType，打包成元组
//（后两个成员固定是 workspaceSize 指针与 executor 指针，由宏追加）。
template <typename... Ts>
constexpr auto ConvertTypes(Ts &... args) {
  return std::make_tuple(ConvertType(args)...);
}

template <typename Function, typename Tuple, size_t... I>
auto call(Function f, Tuple t, std::index_sequence<I...>) {
  return f(std::get<I>(t)...);
}

template <typename Function, typename Tuple>
auto call(Function f, Tuple t) {
  static constexpr auto size = std::tuple_size<Tuple>::value;
  return call(f, t, std::make_index_sequence<size>{});
}


// ---- PTA 内存/缓存钩子 ----
// 这些符号由 CANN 侧提供，用于每次调用前后的线程本地大页内存与
// aclnn 缓存初始化/回收；取不到地址时宏内会判空跳过，不影响主流程。
typedef int (*InitHugeMemThreadLocal)(void *, bool);
typedef void (*UnInitHugeMemThreadLocal)(void *, bool);
typedef void (*ReleaseHugeMem)(void *, bool);
typedef void (*InitPTACacheThreadLocal)();
typedef void (*SetPTAHashKey)(uint64_t);
typedef void (*UnInitPTACacheThreadLocal)();


// UnInitCacheThreadLocal：调用结束后回收线程本地的 aclnn 缓存。
inline void UnInitCacheThreadLocal()
{
    static const auto unInitPTACacheThreadLocalAddr = GetOpApiFuncAddr("UnInitPTACacheThreadLocal");
    UnInitPTACacheThreadLocal unInitPTACacheThreadLocalFunc =
        reinterpret_cast<UnInitPTACacheThreadLocal>(unInitPTACacheThreadLocalAddr);
    if (unInitPTACacheThreadLocalFunc) {
        unInitPTACacheThreadLocalFunc();
    }
}


// EXEC_NPU_CMD_V1：两段式 aclnn 调用宏（与 torch_npu 内置算子同款路径）。
// 用法：EXEC_NPU_CMD_V1(aclnnXxx, 实参...)，展开后按顺序做五件事：
//   1. 符号解析：取 aclnnXxxGetWorkspaceSize / aclnnXxx 以及内存、缓存
//      钩子的函数地址（static，进程内只解析一次）；
//   2. 调用前准备：初始化线程本地大页内存与 aclnn 缓存；
//   3. 参数转换：全部实参 ConvertType 成 ACL 类型（末尾自动补上
//      workspaceSize 指针和 executor 指针两个出参），先同步调用
//      GetWorkspaceSize 拿到工作区大小；
//   4. 异步下发：工作区非零时分配 NPU 字节张量，然后把真正的 aclnn
//      调用包成闭包经 OpCommand::RunOpApiV2 投到当前 NPU 流上——
//      调用本身是异步的，流同步由外部控制；
//   5. 调用后回收：闭包内先释放全部 ACL 对象与大页内存，宏尾部再回收
//      线程本地缓存。
// 宿主线程立即返回；kernel 完成顺序由流保证。
#define EXEC_NPU_CMD_V1(aclnn_api, ...)                                                                                \
    do {                                                                                                               \
        static const auto getWorkspaceSizeFuncAddr = GetOpApiFuncAddr(#aclnn_api "GetWorkspaceSize");                  \
        static const auto opApiFuncAddr = GetOpApiFuncAddr(#aclnn_api);                                                \
        static const auto initMemAddr = GetOpApiFuncAddr("InitHugeMemThreadLocal");                                    \
        static const auto unInitMemAddr = GetOpApiFuncAddr("UnInitHugeMemThreadLocal");                                \
        static const auto releaseMemAddr = GetOpApiFuncAddr("ReleaseHugeMem");                                         \
        static const auto initPTACacheThreadLocalAddr = GetOpApiFuncAddr("InitPTACacheThreadLocal");                   \
        static const auto setPTAHashKeyAddr = GetOpApiFuncAddr("SetPTAHashKey");                                       \
        TORCH_CHECK(getWorkspaceSizeFuncAddr != nullptr && opApiFuncAddr != nullptr, #aclnn_api, " or ",               \
                    #aclnn_api "GetWorkspaceSize", " not in ", GetOpApiLibName(), ", or ", GetOpApiLibName(),          \
                    "not found.");                                                                                     \
        auto acl_stream = c10_npu::getCurrentNPUStream().stream(false);                                                \
        uint64_t workspace_size = 0;                                                                                   \
        uint64_t *workspace_size_addr = &workspace_size;                                                               \
        aclOpExecutor *executor = nullptr;                                                                             \
        aclOpExecutor **executor_addr = &executor;                                                                     \
        InitHugeMemThreadLocal initMemFunc = reinterpret_cast<InitHugeMemThreadLocal>(initMemAddr);                    \
        UnInitHugeMemThreadLocal unInitMemFunc = reinterpret_cast<UnInitHugeMemThreadLocal>(unInitMemAddr);            \
        InitPTACacheThreadLocal initPTACacheThreadLocalFunc =                                                          \
            reinterpret_cast<InitPTACacheThreadLocal>(initPTACacheThreadLocalAddr);                                    \
        SetPTAHashKey setPTAHashKeyFunc = reinterpret_cast<SetPTAHashKey>(setPTAHashKeyAddr);                          \
        if (initPTACacheThreadLocalFunc && setPTAHashKeyFunc) {                                                        \
            initPTACacheThreadLocalFunc();                                                                             \
            setPTAHashKeyFunc(0);                                                                                      \
        }                                                                                                              \
        if (initMemFunc) {                                                                                             \
            initMemFunc(nullptr, false);                                                                               \
        }                                                                                                              \
        auto converted_params = ConvertTypes(__VA_ARGS__, workspace_size_addr, executor_addr);                         \
        static auto getWorkspaceSizeFunc = ConvertToOpApiFunc(converted_params, getWorkspaceSizeFuncAddr);             \
        auto workspace_status = call(getWorkspaceSizeFunc, converted_params);                                          \
        TORCH_CHECK(workspace_status== 0, "call " #aclnn_api " failed");                                               \
        void *workspace_addr = nullptr;                                                                                \
        at::Tensor workspace_tensor;                                                                                   \
        if (workspace_size != 0) {                                                                                     \
            at::TensorOptions options =                                                                                \
                at::TensorOptions(torch_npu::utils::get_npu_device_type());                                            \
            auto workspace_tensor =                                                                                    \
                at::empty({workspace_size}, options.dtype(at::kByte));                                                 \
            workspace_addr = const_cast<void *>(workspace_tensor.storage().data());                                    \
        }                                                                                                              \
        auto acl_call = [converted_params, workspace_addr, workspace_size, acl_stream, executor]()->int {              \
            OpApiFunc opApiFunc = reinterpret_cast<OpApiFunc>(opApiFuncAddr);                                          \
            auto api_ret = opApiFunc(workspace_addr, workspace_size, executor, acl_stream);                            \
            TORCH_CHECK(api_ret==0, "call " #aclnn_api " failed");                                                     \
            ReleaseConvertTypes(converted_params);                                                                     \
            ReleaseHugeMem releaseMemFunc = reinterpret_cast<ReleaseHugeMem>(releaseMemAddr);                          \
            if (releaseMemFunc) {                                                                                      \
                releaseMemFunc(nullptr, false);                                                                        \
            }                                                                                                          \
            return api_ret;                                                                                            \
        };                                                                                                             \
        at_npu::native::OpCommand::RunOpApiV2(#aclnn_api, acl_call);                                                   \
        if (unInitMemFunc) {                                                                                           \
            unInitMemFunc(nullptr, false);                                                                             \
        }                                                                                                              \
        UnInitCacheThreadLocal();                                                                                      \
    } while (false)

#endif  // TORCHNPU_TORCH_NPU_CSRC_ATEN_OPS_OP_API_PTA_COMMON_H_
