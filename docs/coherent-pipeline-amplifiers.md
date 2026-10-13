# 类型化相干流的限幅与高阶放大器

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

`<rfmodel/coherent/coherent_pipeline.hpp>` 在有界的 C++17 前馈管线中提供两个模型适配器。
只需安装后的 `RFModel::rfmodel`，不依赖 C ABI、Python 或 JSON。数值仍来自现有
`CoherentLimitedAmplifier`、`CoherentHighOrderAmplifier` 和 `CoherentPolynomial`；
这里新增的是共同驱动、流局部来源历史和物理身份的组合，不改变增益或系数公式。

## API 和调用

```cpp
enum class AmplifierInputMode { sources_only, propagate_distortion };

// 以下成员属于 rfmodel::CoherentPipeline。
AmplifierResult limited_amplifier(
    Stream input, const CoherentLimitedAmplifier &model,
    AmplifierInputMode mode = AmplifierInputMode::sources_only);
AmplifierResult highorder_amplifier(
    Stream input, const CoherentHighOrderAmplifier &model,
    std::size_t max_source_order,
    AmplifierInputMode mode = AmplifierInputMode::sources_only);
```

例如两个限幅级，再接显式高阶级：

```cpp
#include <rfmodel/coherent/coherent_pipeline.hpp>

rfmodel::CoherentPipeline pipeline(1e8, 50., 2);
const auto input = pipeline.seed({
    {8, rfmodel::SpectrumKind::source, 1., 1, {.01, .003}},
    {12, rfmodel::SpectrumKind::source, 1., 1, {.02, -.002}}
});
const rfmodel::CoherentLimitedAmplifier first_model(10., 20., 23., 30., 20., 50.);
const rfmodel::CoherentLimitedAmplifier second_model(0., 20., 23., 40., 30., 50.);
const auto first = pipeline.limited_amplifier(input, first_model);
const auto second = pipeline.limited_amplifier(
    first.output, second_model, rfmodel::AmplifierInputMode::propagate_distortion);
// 构造参数中的系数是 a2, a3, ...，不是从 a0 开始的完整向量。
const rfmodel::CoherentHighOrderAmplifier high_model(0., 20., 23., {.1, -.01}, 50.);
const auto high = pipeline.highorder_amplifier(
    second.output, high_model, 9, rfmodel::AmplifierInputMode::propagate_distortion);
const auto &snapshot = pipeline.inspect(high.output);
```

- `sources_only` 拒绝任何非 source 记录，即使该记录的总幅度是零。
- `propagate_distortion` 允许 source、harmonic、intermod；已有失真参与共同驱动并
  获得共同直接增益，但只有输入中原本为 source 的记录生成新的非线性项。
  不把失真改标成 source，不发生次级失真再混频。
- 模型按 const 引用同步读取，不存储指针；模型只需在调用期间存活。
  输入/输出流和不可变快照遵守[管线所有权约定](coherent-pipeline.md#调用和所有权)。
- 模型实参考阻抗必须与管线精确相同；不会自动重新归一化电压系数。
  `CoherentLimitedAmplifier::reference_ohms()` 与高阶模型提供相同的只读查询。
  非法 mode、参考或 cutoff 在空流/零流上也会被拒绝。

## 共同工作点和来源阶数

先使用完整输入快照的 `reduction.total_power_w`，且只求一次模型工作点。
同物理键的历史复贡献先相干求和，再对不同物理键求功率和；不能把历史展开后的
功率相加，也不能把所有同频的独立身份强制相干。已有失真及将被 cutoff 删除的
输入历史都计入这个工作点。

`fundamental_amplitude_gain` 直接乘每条保留历史；
`nonlinear_input_scale` 乘原输入的每条 source 历史，再由原始电压系数枚举。
这样 n 次局部项自然带 scale 的 n 次方，不再重复缩放系数。
模型高阶完整向量的 a0/a1 为零，基波直接增益另行传输；非线性产生的载波同频项
仍保留自身 intermod 身份，不覆盖直接 source。

高阶的 `max_source_order` 必须显式指定为 1..256，统计全部原始历史因子，包含
LO、重复及正负成对因子。它不是局部多项式次数，也不是 SystemVue MaxOrder。
直接历史与新生成路径分别按此阶数过滤；工作点仍来自未过滤的完整输入。
截断可能破坏相消而增大保留输出，因此返回显式丢弃计数。

限幅适配器没有语义 cutoff。传播和生成的历史超过 256 因子时，整个操作失败；
不能把 256 当作隐式过滤值而静默丢弃。两种适配器只输出正频 RF；生成的 DC 和
负频路径不会进入后续级，不包含反向端口功率、噪声或反馈求解。

## 身份、相消和结果

直接传输保持种类、规范带宽、物理身份和历史因子。新项按物理有符号因子组合后
分类，与前级同源失真及并行分支使用同一规范身份，因此可相干合并。身份键包含
bin、kind、规范带宽和完整物理因子多重集；不能只按频率或完整历史分组。
`+x,+x,-x` 不化简成 `+x`。混频重置物理根而保留 RF/LO 历史，这一边界不改变。

精确零总波不会触发提前返回。折叠 IF 的 A 与 -A 仍可分别获得小信号增益，并生成
A²、-2A²、A² 三条非零历史，随后再次相消。已归一化的精确零历史仍被直接保留和
计数，但自身不活跃，不产生新的多项式路径。

`CoherentPipeline::AmplifierResult` 包含：

- `output`：一次原子提交的新不可变流。
- `input_power_w`：选择 source 和 cutoff 之前的完整物理输入功率。
- `operating_point`：本次实际使用的 `AmplifierOperatingPoint`，包括共同基波幅度增益、
  非线性输入缩放、限幅输入功率及 a2/a3。高阶完整系数以模型的
  `voltage_coefficients()` 为准，结果中的 a2/a3 不是完整高阶响应。
- `nonlinear_counts.physical_input_count`：仅生成用 source 物理记录数。
  `expanded_input_count` 是其归一化历史数，包含零项；`generated_term_count` 是
  cutoff 和物理合并前生成的非零局部 RF 候选路径数。
  `discarded_term_count` 与 `discarded_by_source_order` 只记录 cutoff 丢弃的生成路径。
- `direct_contribution_count`：直接传输考虑的输入归一化历史条数，包含零项；
  `discarded_direct_contributions`：高阶 cutoff 丢弃的直接历史条数。
  限幅级的直接及非线性 cutoff 丢弃计数始终为零。

上述路径/历史计数不是最终记录数或最终不同单项式数；后两者从输出快照读取。

## 所有权、失败和固定预算

每次成功只消耗一个流和一次变换，不发布中间流。参数错误、内存分配失败、
非有限中间结果、支持宽度不匹配、组号耗尽或任何预算超限都保持旧快照、注册表和
已提交 usage 不变。返回值中可能分配内存的数据在提交前准备完成。
上下文不可复制/移动、非线程安全，仍遵守原有重入约束。

[完整管线限制](coherent-pipeline.md#固定资源上限)全部适用，特别是：

- 最多 4096 个生成用展开历史，含零项；存在非零非线性系数时最多 64 个活跃历史。
  全零系数仍验证输入及展开预算。
- 局部次数最多 11，候选正频 RF 路径最多 4096，枚举访问最多 10,000,000。
  即使 cutoff 最后删除大多数路径，原生枚举预算仍先适用。
- 直接保留记录加生成路径记录，在物理合并前合计最多 4096。
- 单个 origin 最多 256 因子；上下文最多 4096 流、512 变换；累计物理记录/历史项
  各 65,536、历史/身份因子各 1,048,576，每个根表 65,536。表达式子预算同样适用。

这些固定边界比部分 Python 图更窄：Python 可接受较多原始记录再归并为少量输出，
而本接口先限制 4096 条待归并记录。两者组号分配也不承诺逐字节一致；规范带宽的
补偿求和不保证与 Python `math.fsum` 逐位一致。
极端相消即使最终总波可表示，也可能因单条贡献或内核中间功率溢出而显式失败；
不会静默裁剪、重缩放或伪造零输出。

## 安装消费和兼容性边界

[第一翻译单元](../tests/consumer/coherent_pipeline.cpp)用独立标量增益公式与电压
乘积检查两个模型、传播模式、同源 H2 合并、cutoff 和完整驱动；
[第二翻译单元](../tests/consumer/coherent_pipeline_probe.cpp)再次实例化两个适配器及
传播模式，并检查零驱动相消历史与含 LO 的 cutoff。所有检查在 Release/NDEBUG
下保持启用。安装后移动整个前缀，再仅通过 `RFModel::rfmodel` 编译和运行；
见 [SDK 安装命令](cpp-sdk.md)。这不等于新的跨平台或 SystemVue 验收记录。

直接原生的[独立物理回归](../tests/coherent/coherent_pipeline_amplifier_test.cpp)及
[资源/事务边界回归](../tests/coherent/coherent_pipeline_amplifier_limits_test.cpp)分别验证数值历史
和失败不提交契约；安装消费者补充实际公开 SDK 的构建、链接及组合调用覆盖。
另有[分配失败回归](../tests/coherent/coherent_pipeline_allocation_test.cpp)，对 `polynomial`、
两个放大器的每个分配边界注入失败，检查旧快照、usage 和后续确定性恢复。
这些检查在 Debug/Release 都执行，并关闭可选 NRVO，保留返回值构造期间的失败覆盖。

MSVC 的 checked-iterator 实现会在 `noexcept` 的 vector 默认/移动构造函数中
分配调试代理；代理分配失败时由标准库直接终止进程，不能传播为可捕获的
`std::bad_alloc`。因此只有独立、单翻译单元、仅链接 header-only SDK 的分配失败
测试私有设置 `_ITERATOR_DEBUG_LEVEL=0`；Debug CRT、正常数值/资源测试的默认
迭代器检查及公开 SDK 编译设置均不改变。仍注入全部可传播的分配失败，包括
MSVC Release/level-0 map 移动时的哨兵分配，不按分配大小或序号跳过失败。
强异常保证适用于传播回调用者的异常，不能令标准库内部的 `noexcept` 终止恢复执行。

本地 Release 安装重定位的配置、源文件哈希和结果见
[消费验证记录](../validation/coherent-pipeline-amplifier-consumer-validation-20261013.json)。

两个适配器只补充类型化前馈 API，不替换阶段 3A 四操作参考探针或独立评分逻辑，
也不解除 Python 级联比较器对 legacy graph orchestration 的依赖。
已有级联归档仍为 230 项检查、22 项失败，两个 0 dB 级联差异仍保留；
本适配工作本身不重写归档、容差、失败标签或升级兼容声明。
没有新增 RFAMP 自动系数、SystemVue MaxOrder/子谱削减语义、通用 JSON 图调度、
噪声、AM/PM 或反馈工作点能力。完整 Python 退役仍须单独完成比较器迁移与验收，
见[退役计划](python-retirement-plan.md)和[阶段 3A 重放边界](native-reference-replay.md)。
