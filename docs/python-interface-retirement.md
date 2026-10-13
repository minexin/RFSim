# Python 产品接口退役与 C++ 功能迁移

基线为 GitHub main `e47ea901a2ded2528f7e26502d8635f4d599ab24`。用户最终通过仿真软件调用 C++ API，本次先迁移 Python 的仿真编排职责，再删除产品绑定。数值内核及厂商兼容结论没有因接口退役而改变。

## 产品交付

- 删除 `python/` 中 27 个模块、包构建配置、wheel 验证工具，以及 32 个绑定/JSON 适配测试文件。删除清单保存全部 SHA-256、固定版本链接及原 503 个测试方法名，见 [机读清单](../validation/python-retirement-inventory-20261013.json)。
- 交付 C++17 `RFModel::rfmodel`、可选原有 C ABI 和原生 S 数据 CSV CLI。默认 CMake、核心 CI 与纯 SDK CI 不查找 Python；显式 ON 的旧绑定测试开关报退役错误。
- 保留独立开发采集、元数据审计、参考评分脚本和全部参考数据；Python 仅是这些开发工具的运行时。
- 不再提供 Python 对象接口、通用 JSON 文档分发、动态 JSON 字段路径或 Python CLI。宿主用类型化请求和数值参数槽位；其文件/UI/序列化由宿主管理。

## 原模块到原生接口的对应

| 原 Python 模块 | C++ 接口 | 数值/编排职责 |
|---|---|---|
| `__init__`, `__main__` | `C++ public headers`, `C ABI retained`, `native CSV CLI retained` | Language binding and Python/JSON dispatch retired; host owns serialization. |
| `affine_conversion`, `mixer_linearization_file` | `conversion_simulation.hpp`, `mixer_linearization.hpp` | Typed offsets and model linearization. |
| `amplifier_file` | `multitone_amplifier.hpp`, `term_propagation.hpp` | Native component analysis and identity-preserving post-stage propagation. |
| `analysis_file`, `model_file`, `linear_stage_file` | `linear_analysis.hpp`, `power_wave_network.hpp`, `power_wave_noise.hpp`, `tabulated_model.hpp`, `device_model.hpp` | Host builds typed devices/networks and owns file/path selection; Touchstone remains native. |
| `channel_measurements` | `conversion_simulation.hpp`, `channel_noise.hpp` | Physical-channel lookup, sorting and measurement composition migrated. |
| `coherence_file` | `coherence.hpp`, `source_coherence.hpp`, `coherent_system.hpp` | Source resolution and component reduction. |
| `coherent_network_file`, `coherent_graph_support`, `coherent_origins`, `coherent_system_file`, `expression_system` | `coherent_system.hpp`, `coherent_pipeline.hpp`, `coherent_network.hpp`, `origin_expression.hpp` | Compiled graph, names, multi-output network, batched mixer, snapshots, physical identity/history and nonlinear propagation migrated. |
| `conversion_file`, `conversion_network_file`, `operating_point`, `phase_noise` | `conversion_simulation.hpp`, `conversion_operating_point.hpp`, `phase_noise.hpp` | Request composition, topology, accepted-point snapshot, phase-group C/P, V/I and noise experiments migrated. |
| `frequency_plan`, `frequency_plan_file` | `conversion_grid.hpp`, `conversion_frequency_plan.hpp` | Closure already native; source/noise/reference/offset/initial/channel-selection remaps migrated. |
| `optimization`, `optimization_file`, `monte_carlo`, `monte_carlo_file`, `parameter_binding` | `analysis_task.hpp`, `optimization.hpp`, `monte_carlo.hpp` | Numeric slots replace JSON paths; affine binding, objectives, metric projections, dependency guards and best-result snapshot migrated. |
| `spectrum_file` | `spectrum_analysis.hpp`, `nonlinear_spectrum.hpp` | Typed spectrum stages compose native models. |

全部 27 个模块逐文件的接口与机制测试映射保存在机读清单。构建安装后的 [双翻译单元消费者](../tests/consumer/simulation_tasks.cpp) 实际组合图、扩频、转换和参数任务。详细调用契约见 [C++ 仿真 API](cpp-simulation-api.md)。

## 回归证据与判定

本次新增回归以独立物理公式覆盖迁移职责：

- `coherent_system_test`：分支合成、网络多输出仅一次构建、混频后的独立 Fourier 乘积、来源相消、冻结计划、跨图句柄、取消和批次失败。`coherent_pipeline_allocation_test` 对多输出网络和混频组逐分配注入失败，检查所有状态回滚及确定性恢复。
- `analysis_task_test`：两电阻解析最优值、一变量多槽位、2,048 个相关容差样本的独立物理公式、联合门限、重放、整数绑定边界、依赖修改、取消及异常。
- `conversion_simulation_test`：端口/频点重排与 DC/RF C/P 映射、热边界、跨器件噪声；四相位独立集合校验共享/独立相位组在三种复参考尺度下的完整 C/P；解析 NF、V/I、功率、通道积分；非线性反馈及径向/切向导数噪声，确保后处理不重算工作点。
- 原有原生核心、C ABI、CLI、安装消费者和显式原生高阶重放继续运行。

**退役的 503 个 Python 方法不计作本次通过，也未声称逐条迁移了 JSON/绑定专有夹具。** 功能映射、独立机制回归与已退役语言契约分别记录。当前执行结果写入 [验证记录](../validation/cpp-api-migration-validation-20261013.json)。

## 开发工具保留范围

`scripts/reference/native_replay.py --probe ... --report ...` 与五个原生高阶归档套件继续使用独立 C++ 探针。后续已增加第六个 AM/PM 诊断套件，比较器接受 `--native-probe`，继续保留厂商不匹配结论，见 [A01 记录](systemvue-am-pm-existing-reference.md)。采集解析、评分与元数据工具的标准库导入在主动禁止 `rfmodel` 导入时检查；独立 DOM 全量审计仍有其既有 lxml 开发依赖。

未迁移的历史数值比较器曾通过 `Library(...)` 或 Python 图执行：例如旧单音/双音压缩、来源级联。其纯数据评分逻辑及归档保留；数值调用现在由 `retired_binding.py` 明确拒绝，提示使用原生 API/支持的探针或固定基线源。它们不会静默跳过后报告通过。本次未宣称把所有开发比较器迁成新的原生协议。

原始文档和 JSON 示例作为历史模型/采集说明保留，并标明退役入口。SystemVue 名义失败、缺项、校准与 holdout 分离、diagnostic-only 状态均保留。本次未重新执行 SystemVue，也未完成完整厂商兼容。

## 本机执行结果（2026-10-13）

MSVC 19.44 / C++17：Debug 和 Release 完整 CTest 各 **126/126**，包含 C ABI、原生 CLI 与七项显式开发参考重放；重定位纯 C++ 安装包消费者各 **7/7**。十个独立开发脚本套件全部通过。四个新增公共头文件单独以 `/permissive- /W4 /WX /Zs` 编译通过；204 个 C/C++ 文件格式检查通过。

首轮 Debug 有两项归档哈希失败，定位为旧本地检出残留 CRLF；恢复 Git 提交原始字节后通过，未改预期哈希或数据判定。三平台远程 CI 状态以关联提交的工作流结果为准，本机证据不替代远程结果。

提交 `727db06` 的[三平台 CI](https://github.com/minexin/RFSim/actions/runs/38384762960) 已完成：Windows、Ubuntu、macOS 的 Debug/Release，核心、纯 C++ SDK 和原生参考重放共 **18/18** 个作业成功。此结果绑定该功能迁移提交；后续目录调整另行验证。
