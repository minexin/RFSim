# 代码目录与模块归属

公共 C++17 类型仍在 `rfmodel` 命名空间中。目录按代码职责划分，函数签名、数值公式、C ABI 符号和 CMake target 名称保持不变。

```text
include/rfmodel/
  adapters/    模型能力绑定、已有模型适配和厂商版本映射
  core/        矩阵、线性求解、模型基础、插值、网络参数与功率波参考
  io/          Touchstone 导入与表格模型
  models/      RLC、传输线、电缆、滤波器及理想多端口器件
  linear/      线性网络、物理接线、扫描和链路分析
  noise/       噪声矩阵、噪声系数、加载噪声、通道积分与相位噪声
  nonlinear/   放大器、压缩、多项式、混频与非线性频谱
  coherent/    相干组、来源表达式、流管线与可执行系统图
  conversion/  变频模型、工作点、线性化、频率规划及组合仿真
  analysis/    测量、频谱任务、优化与 Monte Carlo 编排
  api/         C ABI 声明
src/
  api/         C ABI 实现
  cli/         原生 CSV 命令行程序
examples/
  linear/ coherent/ conversion/ analysis/  C++ 调用示例
  ...          原有数据与历史 JSON 示例
tests/
  core/ io/ models/ linear/ noise/ nonlinear/ coherent/ conversion/ analysis/
  api/         C ABI 回归
  reference/   原生物理/归档探针
  support/     测试断言等公共辅助代码
  fixtures/    Touchstone 及其他静态测试输入
  cmake/       配置、CLI、安装和 SDK 检查
  consumer/    独立安装消费者（含新旧头文件路径验证）
  reference_consumer/  独立安装的开发参考探针
cmake/
  RFModelHeaders.cmake   公共头文件清单与兼容头生成
  RFModelTests.cmake     原生测试注册
  RFModelExamples.cmake  示例注册
  NativeReferenceReplay.cmake  显式开发重放
```

## 新代码的 include

```cpp
#include <rfmodel/linear/network.hpp>
#include <rfmodel/coherent/coherent_system.hpp>
#include <rfmodel/conversion/conversion_simulation.hpp>
#include <rfmodel/analysis/analysis_task.hpp>
```

继续通过 `RFModel::rfmodel` 链接；独立安装包的 `find_package` 方式和 CLI 名称不变。只直接使用源码头文件时，添加仓库 `include` 目录并采用上面的模块路径。

旧的 `<rfmodel/network.hpp>`、`<rfmodel/analysis_task.hpp>` 和 `<rfmodel/c_api.h>` 等扁平路径仍可在 CMake target 与安装后的 SDK 中使用。兼容头由 CMake 按公共头清单生成到构建树的 `legacy-include/rfmodel/`，安装时一起发布；每个文件只包含对应模块头，不重复实现。源码根目录不保留另一套转发文件。新公共头需登记到 `cmake/RFModelHeaders.cmake`；配置时校验清单完整性及文件名唯一性，避免兼容路径冲突。

## 测试与维护

测试按被测职责归类，公共断言集中到 `tests/support`，静态夹具集中到 `tests/fixtures`。增加测试时更新 `cmake/RFModelTests.cmake`；新增示例更新 `cmake/RFModelExamples.cmake`。根 CMake 只负责产品目标、安装、选项及模块注册，不改变既有测试/示例可执行文件的位置。

目录之间用完整的 `<rfmodel/模块/头文件>` 路径引用，不依赖跨目录的隐式搜索路径。模块目录表示代码归属，并不承诺这些模块已是独立链接库。格式脚本现在覆盖 `include`、`src`、`tests` 和 `examples`。

原始归档数据与已有验证记录不改写；历史记录中的文件路径对应其绑定的提交。当前路径映射与本轮验证见 [目录整理记录](../validation/module-layout-validation-20261013.json)。

## 2026-10-13 目录整理验证

本机 MSVC / C++17：Debug、Release 完整回归各 **126/126**；重定位 C++ SDK 消费者各 **8/8**，覆盖所有旧头文件路径与模块路径混用；C ABI 安装调用另验 **2/2**。211 个 C/C++ 文件格式检查通过。根 CMake 从 **516 行**缩减为 **65 行**；数值源文件排除 include、空白与明确路径更新后，与基线一致。三平台 CI 以本轮提交的工作流结论为准。

提交 `003bdd7` 的[三平台 CI](https://github.com/minexin/RFSim/actions/runs/38386549185) 已完成：Windows、Ubuntu、macOS 的 Debug/Release，核心构建、纯 C++ SDK 和原生归档重放合计 **18/18** 个作业成功。此后补交的文件仅记录该结果，未改变已验证代码。
