# 模型能力与引擎适配契约

本轮将模型接入统一为 C++17 `ModelAdapter`，把数学行为、分析能力、模型来源和求解过程分开。新增模型可以提供回调接入现有线性、频谱、工作点及相干图入口，不需要向网络求解器增加器件类型判断。既有模型类和原生接口继续可用。

## 交付与验收

| 项目 | 实施内容 | 验收依据 |
|---|---|---|
| 能力声明 | 由实际回调生成五项能力；不支持时抛 `ModelCapabilityUnavailable` | 缺失能力、参数相关能力与错误回调回归 |
| 线性/噪声 | S、固定逐端口复参考、显式协方差进入原网络 | 独立反射反馈解析解、热噪声绝对单位、复参考及无源性检查 |
| 频谱/工作点 | 频谱阶段和局部响应/Jacobian 绑定 | 独立谐波公式、复方向有限差分、自定义非线性反馈解 |
| 相干扩展 | `CoherentSystem::stage`；四个既有非线性入口复用它 | 多输出、来源历史、冻结计划、销毁后运行、异常重试、外来句柄、取消 |
| 厂商边界 | 2023 AM/PM 常量移至 `adapters/`，数值相位组合改为模板 | 既有 AM/PM 回归和原生归档重放；公式和经验常量不变 |
| SDK | 公共头清单、示例、独立双翻译单元消费者 | Debug/Release 构建、CTest、安装后消费者及 CI |

实际运行结果单独记录在 `validation/model-adapters-validation-20261014.json`；上述验收项不是运行成功声明。

2026-10-14 本地验收：MSVC x64 Debug/Release 均完成 clean-first 全量构建。148 项完整回归首轮分别通过 146/148、147/148：SDK 配置测试的示例清单漏列新增目标，修正后两配置均通过；Debug 归档协议测试在并行构建期间触发 240 秒超时，停止其他负载后的单项复跑在 114.96 秒通过，未修改超时或数值阈值。两配置的 148 项测试均已有通过证据，但不称首轮全绿。纯 C++ SDK 经安装重定位后，独立消费者各 9/9；221 个 C/C++ 文件格式检查通过。远程 CI 结果以绑定具体提交的记录为准。

## 类型与能力

公共入口是 `<rfmodel/adapters/model_adapter.hpp>`，常用工厂位于 `<rfmodel/adapters/builtin_model_adapters.hpp>`。

`ModelDescriptor` 保存 `ModelIdentity`、实例名、物理端口。身份包含 `type_id`、适配器 `revision`、可选 `vendor` 和 `vendor_version`。这些字段标记来源，不等于兼容验收，也不进行自动注册、查找或参数转换。宿主可将适配器存放在自己的模型库中。

物理端口 ID 必须连续为 0..N-1，数量 1..1024；参考阻抗有限且实部为正。当前描述符采用固定逐端口参考坐标。需要频率相关参考或工作点相关参考的模型，应显式变换到这些固定坐标，或继续使用底层原生接口；不能只改元数据标签。

`ModelFunctions` 是构建时的可选回调集合。`ModelAdapter` 按值保存描述和函数，构造后不提供修改接口。`capabilities()` 从回调是否存在生成，用户不能另外设置一个与实现脱节的能力位。

| 能力 | 模型提供 | 接入引擎 |
|---|---|---|
| `linear` | `scattering(f) -> SMatrix` | `add_to(network,f)` 使用完整逐端口功率波参考；接线、反射和求解仍归 `LinearNetwork` |
| `noise` | `noise(f) -> NoiseCorrelation`，W/Hz | `noise_correlation(f)` 验证尺寸、有限性、Hermitian/PSD；由宿主显式装配为网络内生噪声 |
| `spectrum` | `PowerWaveSpectrum -> PowerWaveSpectrum` | `spectrum_stage(name)` 接入 `analyze_spectrum`；限定匹配的 0→1 两端口、正实参考，保持频率步长 |
| `operating_point` | 入射波、频率通道 → 出射波与直接/共轭 Jacobian | `operating_device(index,spacing,channels)` 接入现有工作点求解器 |
| `coherent` | 管线与输入流 → 输出流和阶段统计；显式输入/输出流数量 | `coherent_stage(system,...)` 接入统一阶段；要求模型和管线采用相同公共正实参考 |

缺失噪声能力绝不自动生成零噪声。静态噪声回调要求同时具备线性能力；它不代表工作点相关变频 C/P 噪声模型。后者继续由 `ConversionDevice` 的显式噪声和现有变频 API 表达。

工作点适配在调用模型前验证频率、端口、重复通道、波和 DC；返回值必须保留通道顺序、物理身份、频率步长及逐通道参考。不能通过回调改变频率网格；先由宿主/`ConversionGrid` 完成结构规划。Jacobians 是当前完整复波坐标的导数，不是单纯小信号增益。噪声、频率规划、器件间接线和收敛算法不由适配器隐式推断。

## 已有模型怎样接入

`adapt_device_model(concrete_model, identity)` 按值复制/移动具体模型，依据现有能力接口连接 S、噪声和频谱，不用 RTTI。适用于实现 `RFDeviceModel` 的 RLC、传输线、电缆、滤波器、多端口、表格 S/噪声、小信号放大器、多项式放大器及理想混频器。只声明它们已经实现的能力，不自动给线性器件添加频谱或相干入口。线性器件可通过已有相干图 `linear_network()` 接入。

`adapt_passive_model(model, identity, temperature_k)` 是显式温度的无源噪声适配。每次评估检查无源性，包括零温度。调用者选择此工厂即选择无源热噪声定义；不会从器件名称、厂商 NF 默认值或环境中猜测温度。

`adapt_polynomial_amplifier(model, identity, max_source_order)` 额外提供工作点及相干适配。工作点和频谱保留 DC；相干接口沿用 RF 投影及来源阶数截断，不包含 DC 反馈。非零常数项的模型保留线性/频谱/工作点能力，关闭相干能力。`max_source_order` 为 1..256，与已有管线相同，不等于电压多项式最高十一阶。

`adapt_saturating_amplifier(name, model, reference)` 提供公共基波压缩的工作点和相干能力，不虚构噪声或完整谐波模型。其他组合可用 `ModelDescriptor + ModelFunctions` 显式构造。

```cpp
auto adapter = rfmodel::adapt_polynomial_amplifier(
    rfmodel::MatchedPolynomialAmplifier("PA", {0., 2., 0., -.1}),
    {"myproduct.polynomial", "1", "", ""});
auto stage = adapter.spectrum_stage("PA");
auto result = rfmodel::analyze_spectrum({1e8, {{10, .02}}}, 50., {stage});
```

完整运行例见 `examples/analysis/model_adapter.cpp`，自定义反馈器件见 `tests/models/model_adapter_test.cpp`。

## 相干图扩展、所有权和失败

`CoherentSystem::stage(name, inputs, output_names, operation)` 是宿主扩展入口。回调收到当前执行的 `CoherentPipeline` 和本阶段输入句柄，返回 `StageEvaluation{outputs, statistics}`。输出数量必须与声明一致，每个句柄必须属于当前管线；可返回已有输入的别名。构图时检查输入所有权、名称冲突及图预算，检查失败不会发布新定义。

回调应通过现有管线操作保留物理身份和来源历史，可以组合多个操作。不得通过 `seed()` 偷换来源、保留管线引用供执行结束后使用或返回其他执行的句柄。这是可信宿主 C++ 扩展契约，不是隔离不可信插件的沙箱。任意全新非线性来源代数仍需定义相应的管线数学操作；仅有普通频谱输出不能自动恢复来源历史。

每个通用阶段占一个图阶段预算；其内部实际变换由管线逐次计数并检查现有 512 次等运行预算。阶段可以返回多个流，内部中间流也计入管线预算。四个既有入口 `polynomial`、`fundamental_compression`、`limited_amplifier`、`highorder_amplifier` 已复用同一输出校验路径，原签名不变。

编译计划持有定义及回调，每次执行创建独立管线。失败执行不发布部分 `Result`，后续可重试；已返回的结果和已有计划不受构建器后续修改影响。回调外部副作用不能回滚。标准工厂拥有 `shared_ptr<const Model>` 或参数值；自定义回调捕获的引用、指针、共享可变状态，其生命周期及并发安全由宿主负责。取消仍在阶段间检查，不打断正在执行的回调。

## 厂商适配与兼容

通用 `LegacyPointDerivativeAmPm` 和 `AmPmResponse` 位于 `nonlinear/point_derivative_am_pm.hpp`。`SystemVue2023PointDerivativeAmPm` 的冻结经验常量和版本说明位于 `adapters/systemvue2023_am_pm.hpp`。数值链式导数组合位于 `conversion/output_phase_linearization.hpp`，`apply_output_am_pm` 接受具有相同 `response(power)` 契约的类型，不识别厂商名称。

旧 `nonlinear/am_pm.hpp` 与 `conversion/am_pm_linearization.hpp` 保留为兼容入口，继续同时公开旧类型和函数。类名、参数单位、公式、经验常量及现有 C ABI 不变。本轮没有新增 SystemVue 数值兼容结论，DataPoints、自动 RFAMP 高阶系数、完整器件库和 PHD 仍按原验收记录推进。

这层是源码级 C++17 API，与 `RFModel::rfmodel` 一起安装；不定义跨编译器插件 ABI，不恢复已退役 Python/JSON 产品接口。
