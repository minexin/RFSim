# 仿真软件的 C++17 编排 API

RFModel 的交付入口是安装后的 `RFModel::rfmodel`。宿主用 C++ 类型构建请求并调用函数，数值计算、图执行、频率扩展、参数绑定、优化和容差分析均无需 Python。完整安装消费者见 [simulation_tasks.cpp](../tests/consumer/simulation_tasks.cpp) 与 [第二翻译单元](../tests/consumer/simulation_tasks_probe.cpp)；编译命令见 [SDK 指南](cpp-sdk.md)。

## 按仿真任务选择入口

| 任务 | 入口 | 结果与调用责任 |
|---|---|---|
| 线性扫描、内生噪声、输出复参考变换 | `analyze_linear` / `linear_analysis.hpp` | 宿主回调按频率构建 `LinearNetwork`；结果拥有 S、噪声及逐端口参考 |
| 确定性频谱级联 | `analyze_spectrum` / `spectrum_analysis.hpp` | 有类型阶段回调；复用放大器、混频器和 `transmit_linear_spectrum` |
| 放大器来源项及线性后级 | `MultiToneLimitedAmplifier`、`transmit_linear_terms` | 保留来源身份和零幅度项；宿主可循环保留各阶段快照 |
| 相干图、混频、失真级联 | `CoherentSystem` / `coherent_system.hpp` | `input`、`mixer_bank`、多输出 `linear_network`、`polynomial`、`fundamental_compression`、`limited_amplifier`、`highorder_amplifier` |
| 结构扩频与数据迁移 | `ConversionGrid` / `conversion_grid.hpp` | 原生闭包；按物理端口/频点映射源、反射、初值、偏置、复参考、完整 C/P 及 AM/PM 通道选择 |
| 变频、工作点、相位噪声、测量 | `analyze_conversion_simulation` / `conversion_simulation.hpp` | `ConversionSimulationRequest` 返回波、V/I、净功率、通道测量、噪声实验、工作点诊断及接受点 Jacobian 快照 |
| 参数化优化、Monte Carlo | `run_optimization_task`、`run_monte_carlo_task` / `analysis_task.hpp` | 数值槽位、一对多仿射绑定、测量投影、目标/门限、确定性统计；回调可返回任意有类型分析结果 |

## 相干图与所有权

`CoherentSystem(spacing_hz, reference_ohms, sources)` 通过源名与参考时钟分配相干组。输入只接受声明过的源组。流句柄属于各自图；不允许跨图使用。`compile()` 冻结定义；随后修改构建器不会改变已有计划。计划与执行结果拥有必要状态，构建器销毁后仍可执行/读取。

多输出网络每个实际频点只构建、求解一次；混频组在一次操作中运行全部分支。管线在完整批次准备成功后发布流、统计和组号，异常不改变旧快照。来源历史保留相消路径，并按全历史因子执行来源阶数截断。一个图最多 4096 条流、512 次变换；单混频组最多 2048 分支及 2048 输入记录。其余记录/历史预算沿用 `CoherentPipeline`。

执行结果提供 `stream(handle/name)`、阶段统计、来源根查询及使用计数。计划可重复执行；并发执行要求宿主捕获的回调对象自身支持并发。取消只在阶段间检查；已开始的回调不被强行中断。回调外部副作用由宿主管理。

## 频率扩展与混合参考

先构造 `ConversionGrid`，再使用 `plan().channels` 建立器件模型。`expand(device, seeds, generated_reference, boundary)` 生成局部数据；`global_noise` 保留跨器件 C/P。`align_device`/`align_vector` 处理已有模型的 bin-major 与规划的 port-major 顺序差异。新增外部通道可设置统一边界和热噪声；接线端不会被赋予外部边界。DC 参考和波为实数，DC 热噪声具有 C=P。

扩展后的参考是模型坐标元数据。宿主应在构建模型时使用对应参考，已有模型需使用 `renormalize_power_waves` 及边界变换保留物理电路，不能只改标签。

`ConversionSimulationRequest` 拷贝拥有请求。非线性器件使用现有 `ConversionNonlinearDevice` 回调；求解后保留被接受的线性化、偏置和噪声。后处理不再额外调用非线性模型。相位噪声成员用物理器件/端口/bin 标识，每组共享一个相位过程，不同组及显式附加 C/P 相互独立；在指定的公共实参考中生成后，完整转换回局部复参考。噪声系数实验使用接受点模型，按显式参考/热通道重新激励。

## 参数绑定与任务结果

`ParameterizedAnalysis<Result>` 定义带名称/单位的数值槽位和 `evaluate(vector<double>) -> Result` 回调。每个变量可绑定多个槽位，使用 `scale*x+offset`；槽位唯一，拒绝别名重叠。整数变量的整数仿射映射用检查过的整数运算，保证 2^53-1 范围内精确。

测量回调读取 `Result`，投影支持实值、实部、虚部、幅度、模平方、幅度 dB、相位。优化支持最小、最大、目标值、上下门限以及权重/尺度。`OptimizationTaskResult.best` 拥有最佳参数、分析结果和目标测量，不为输出最佳结果重新求解。Monte Carlo 沿用原生六类分布、相关输入、联合统计、失败记录、固定种子和多指标联合门限。

任务拷贝参数/绑定/目标规格；回调顺序执行，取消在评估之间检查。外部依赖可用 `dependency_revisions` 在任务前后校验；推荐捕获已加载的不可变 Touchstone 数据。宿主必须检查停止原因、有限点/有效样本状态；异常或无可用样本不能当作成功。结果值类型应拥有数据；若宿主返回裸指针或引用，其生命周期仍由宿主负责。

## 单位和边界

频率 Hz、阻抗 ohm、功率 W、C/P 为 W/Hz，RF 波幅为 sqrt(W)。`SMatrix.values` 行优先。C++ API 抛出标准异常，宿主在插件/仿真软件边界捕获。C++17 头文件 API 需由宿主重新编译；跨编译器二进制集成可使用保留的 C ABI，但新增编排层目前以 C++ 类型 API 为准。

旧 JSON 文档的文本解析、动态字段路径、Python CLI 和 wheel 已退役；宿主负责自己的文件格式、UI 与序列化。它们不再是公开产品入口。迁移对应表和回归证据见 [退役记录](python-interface-retirement.md)。完整 SystemVue 兼容仍未完成。
