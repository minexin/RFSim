# 类型化相干流管线（C++17）

`<rfmodel/coherent_pipeline.hpp>` 提供 header-only 的 `rfmodel::CoherentPipeline`。
只需链接安装包中的 `RFModel::rfmodel`；不依赖 C ABI、Python 或 JSON。
它把现有数值内核串成五种显式操作：`seed`、`mix`、`linear_network`、
`polynomial` 和 `compress_fundamentals`。每次返回新流，输入流可复用、分支和重合并。

这是有界的前馈计算入口，不是通用系统图调度器。没有 JSON/CLI 装配、序列化、
自动时钟推断、多输出共享提取、限幅/级联/高阶放大器适配或反馈工作点求解。
现有 Python 系统图仍保留这些未迁移功能；此入口不构成删除 Python 的依据。
也不增加 SystemVue MaxOrder、RFAMP 系数、子谱削减、LO 驱动、转换噪声、AM/PM
或完整厂商兼容性声明。

## 调用和所有权

```cpp
#include <rfmodel/coherent_pipeline.hpp>

rfmodel::CoherentPipeline pipeline(1e8, 50., 2);
const auto source = pipeline.seed({
    {8, rfmodel::SpectrumKind::source, 1., 1, {.01, .003}},
    {12, rfmodel::SpectrumKind::source, 1., 1, {.02, -.002}}
});
const auto mixed = pipeline.mix(source, {10, 2, 0., .3});
const auto expanded = pipeline.polynomial(mixed, {0., 1., .5}, 4);
const auto &snapshot = pipeline.inspect(expanded.output);
```

- 构造参数依次为频率间隔 Hz、公共实参考阻抗 Ω、最大预留源相干组号。
  所有将使用的 RF/LO 源组必须在 `1..reserved_source_group_max` 内提前预留；
  管线生成组和内部临时组使用更高编号。编号是分析内标识，不是稳定的持久 ID。
- `CoherentComponent` 顺序为 bin、种类、带宽 Hz、相干组、复幅度 sqrt(W)。
  bin 是正整数，频率为 bin × spacing；宽度必须正且频带不得跨越负频率。
  `seed` 只接受 `SpectrumKind::source`，由调用者明确指定相干关系。
- `Stream` 是可复制的只读句柄，不可自行默认构造；只在创建它的存活管线中有效。
  跨管线和失效句柄会被拒绝，另一管线复用相同对象地址也不会接受旧句柄。
  管线上下文不可复制/移动。句柄不延长上下文寿命。
- `inspect` 返回不可修改的 `StreamSnapshot` 引用；它及已有流在后续成功或失败
  操作后保持有效，直到上下文析构。快照含 `records` 和按物理身份恢复的 `reduction`。
  操作同步完成；本接口没有异步任务、取消令牌或自动并行。上下文不是线程安全的。

## 物理身份与相消历史

每个 `StreamRecord` 包含三个不同层次：`component` 的当前复波、
`physical_identity.factors` 的物理身份，以及 `history` 中原始 RF/LO 单项式及各自复贡献。
`physical_root(id)` 和 `history_root(id)` 返回各自的 `RootInfo`；两个根表的 ID 不可混用。
根元数据为组、bin、宽度和 `RootRole::rf/lo/mixed`。

一个 RF 组可以跨多个 bin。相同 RF 物理组配相同 LO 组，在不同 `mix` 调用中也复用
混频物理组；折叠到同 bin/宽度/种类的路径相干求和。混频后的物理身份重置为该输出
bin 的 `mixed` 根，原始 RF/LO 历史完整保留，并且历史仅属于当前流。
不同 RF 组或 LO 组不会因频率相同而自动相干。

例如 RF8 为 A、RF12 为 B，LO10 相位为 φ，则 IF2 为
`conj(A)*exp(iφ) + B*exp(-iφ)`。A = 0.01、B = -0.01、φ = 0 时，IF 复波为零，
但仍有两个非零历史。相消不意味着历史消失：平方级仍可生成三个非零来源贡献，
它们再次相消。相同单项式合并后的已知零项也保留在归一化表达式中。
多项式内核不会为精确零输入贡献生成路径；它会展开零总波下仍非零的不同历史。

因子规范化保留重复和正负成对因子，不把 `+LO,-LO` 约掉。
频率按带符号根 bin 相加；支持宽度按所有根宽度无符号相加，LO 历史根宽度为零。
宽度用规范根顺序的补偿和计算，验证使用 `1e-12` 相对容差；分组使用规范化宽度。
不承诺跨平台与 Python `math.fsum` 逐位一致。

物理功率由 `snapshot.reduction.total_power_w` 给出：同物理键的复波先相加，
不同物理键再加功率。不能把展开历史的功率直接相加，也不能把所有同频但独立身份
的复波强行合成。临时展开组不会成为功率或压缩驱动的物理身份。

## 五种操作的边界

- `seed(components)`：建立 RF 源根，合并重复物理键并保留来源信息。
- `mix(input, MixerDrive{lo_bin, lo_group, conversion_gain_db, phase_radians})`：
  理想实混频器输出差频和和频，转换增益只应用一次。RF < LO 的差频共轭整个输入
  贡献，包括其复幅度及所有历史因子符号；RF > LO 则共轭 LO。
  RF == LO 的精确 DC 转换被拒绝。保留输入种类，无来源阶数截断，但仍受 256 因子限制。
- `linear_network(inputs, external_ports, output_port, builder)`：
  `inputs` 是 `{Stream, input_port}` 列表，端口是 `LinearNetwork` 全局索引，
  不是 `external_ports` 中的位置。求匹配外部入射波在一个指定外部端口的出射波。
  网络参考必须与管线相同；外部端口应未连接/未终接，其余端口须正确连接或终接，
  网络不得含独立源。零传输保留已知历史。可用多端口 S 矩阵进行复权重重合并。
- `polynomial(input, voltage_coefficients, max_source_order)`：
  系数按 `a[0], a[1], ...` 表示 `y(t)=sum(a[n]*v(t)^n)`，有限实数，
  `a[0]` 必须为零，最高局部次数 11。每级只保留正频 RF，DC 不传到下一级。
  `max_source_order` 必须显式指定为 1..256：统计展开后的全部原始因子，包含 LO、
  重复和共轭因子，不等于本级多项式次数。超限路径在组合历史之前丢弃，
  但原生枚举/输入预算仍先约束整个候选展开。一次局部线性传输也可能保留高来源阶数。
  局部一阶保留输入种类；高阶只有展开物理因子全为同一正根时标为 harmonic，
  其余标为 intermod。因混频物理根已重置，折叠 IF 的平方可为 harmonic。
- `compress_fundamentals(input, model)`：仅接受 source 种类；对归一化的实际物理
  总功率调用一次 `SaturatingFundamentalCompression::amplitude_gain`，同一实增益
  乘到所有历史。返回 `output`、`input_power_w`、`common_amplitude_gain`。
  不能用单个历史功率代替驱动，也不通过输入/输出总波相除推断增益。
  零驱动使用有限的小信号增益，因此相消来源可以大于净驱动而仍合法。

`PolynomialResult::counts` 中 `physical_input_count` 是实际物理记录数，
`expanded_input_count` 是展开历史数（包括展开的零项），`generated_term_count` 是
过滤和合并前原生生成的非零局部 RF 路径数。`discarded_term_count` 和
`discarded_by_source_order` 只统计来源阶数过滤所丢弃的局部路径，不是最终单项式数。
上面非相消示例为 3 个物理输入、4 个展开输入、19 条生成路径；cutoff 3 丢弃 15 条
四阶来源路径，cutoff 1 丢弃全部 19 条。

## 回调与失败

网络 builder 接收实际输入的频率 Hz，每个不同 bin 在单次单输出调用中构造一次，
也包括新生成频率及含已知零项的 bin。空输入仍以 0 Hz 构造网络以验证端口和参考。
所有声明的输入端口即使对应空流也会验证。多次请求不同输出不共享提取缓存。

builder 应为确定性的纯网络构造函数，捕获对象由调用者管理寿命。
回调可以读取 `inspect`/`usage`，但重入修改同一管线会抛出 `std::logic_error`。
若回调抛异常，管线不提交输出或注册表/计数变化，
但回调自己造成的外部副作用无法回滚。参数、非有限数、组/bin 溢出和资源超限以
C++ 异常报告；空流或零波不会绕过必要参数检查。失败后旧快照和已提交状态不变。
本接口不写文件或启动进程。

## 固定资源上限

这些边界是硬限制，不静默裁剪、不按较低来源阶数放宽，也不可配置。

| 范围 | 上限 |
| --- | --- |
| 种子输入 / 单次待归一化记录 / 单流物理记录 | 4096 |
| 非线性多项式活跃展开贡献 | 64（精确零贡献不活跃） |
| 多项式展开输入，包含零项 | 4096；纯线性级也受此限制 |
| 多项式局部次数 / 生成 RF 路径 / 枚举访问 | 11 / 4096 / 10,000,000 |
| 任一来源或物理 origin 的因子 | 256 |
| 表达式输入父表达式数 / 每父项数 / 总项数 / 总因子 | 4096 / 4096 / 65,536 / 1,048,576 |
| 单个规范输出表达式项数 / 总因子 | 4096 / 65,536 |
| 单次表达式乘法组合工作 | 1,000,000 |
| 线性网络输入流端口条目 / 展开入射贡献 / 不同频点 / 外部端口 | 4096 / 4096 / 2048 / 1024 |
| 混频物理输入 | 2048 |
| 上下文保留流 / 变换操作 | 4096 / 512 |
| 上下文累计物理记录 / 历史项 / 历史因子 | 65,536 / 65,536 / 1,048,576 |
| 每个根表 | 65,536 个根 |
| 物理身份注册表累计因子 | 1,048,576 |

`usage()` 返回 `PipelineUsage` 的当前已提交计数：流、变换、物理记录、历史项/因子、
两类根及注册物理身份因子。`seed` 消耗流和存储预算，不消耗变换次数；
`mix`、`linear_network`、`polynomial`、`compress_fundamentals` 每次成功各消耗一次变换。
上下文预算累计所有保留快照，不只最新输出；释放一个用户句柄不会回收流预算。
源和生成相干组使用 `uint64_t`，耗尽显式报错。所有限制取交集，因此某阶段可能先
触及更小的原生表达式、网络或枚举上限。

## 可运行示例与安装消费者

[examples/coherent_pipeline.cpp](../examples/coherent_pipeline.cpp) 使用 IF 相消、
线性 IF-only 滤波、公共压缩和平方级两条分支，展示历史与物理净波的区别。

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release \
  -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF \
  -DRFMODEL_BUILD_PYTHON_TESTS=OFF -DRFMODEL_BUILD_EXAMPLES=ON
cmake --build build-native --target coherent_pipeline_example
./build-native/coherent_pipeline_example
```

示例输出：

```text
folded IF: wave=(0,0), histories=2
compression: physical drive=0 W, common gain=10
compressed IF histories: (0.1,0) (-0.1,0)
quadratic IF harmonic: wave=(0,0), histories=3
polynomial: physical inputs=3, expanded inputs=4, generated paths=5, discarded paths=0
```

[安装消费者](../tests/consumer/coherent_pipeline.cpp)与
[第二翻译单元](../tests/consumer/coherent_pipeline_probe.cpp)仅包含公开头文件，
覆盖混频 → 多项式 → 线性网络、独立有符号频谱卷积、手算来源阶数计数、
历史相消、零驱动公共增益和跨管线句柄拒绝。Release 下检查仍有效。

按 [SDK 安装说明](cpp-sdk.md) 关闭 C ABI/Python 后安装，将安装目录整体移到新路径，
再用新前缀配置 `tests/consumer` 并执行 `installed_coherent_pipeline_consumer`。
消费工程不得添加源码 include 路径；只链接 `RFModel::rfmodel`，才能检查安装完整性、
可重定位性及两个翻译单元的 header-only 链接。
本指南中的命令不是平台验收记录；实际执行的构建配置和兼容套件结果应另行记录，
Linux 证据不能代表 MSVC、macOS 或 SystemVue 已验证。
