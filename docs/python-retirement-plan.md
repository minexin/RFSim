# C++ 优先交付与 Python 接口后续退役计划

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

审计日期：2026-10-13。范围为当前工作树；本轮只补强 C++ SDK 安装消费与七通道混频—热电阻的原生独立物理回归，不删除 Python/C 接口，不实现通用 C++ JSON 解析器，不扩大 SystemVue 兼容声明。

## 状态更新：2026-10-13 后续增量

下文详细表格保留首次审计时点的差距，当前状态以本节及各增量验证记录为准：

- SDK 提交 `ceded246` 与第一批原生物理迁移提交 `cabbbced` 已推送，各自三平台两配置的完整/纯 C++ 共 12 个 CI job 全通过：[SDK CI](https://github.com/minexin/RFSim/actions/runs/38342174081)、[原生迁移 CI](https://github.com/minexin/RFSim/actions/runs/38342751482)。首次审计中的“未提交/未跑远端”仅是当时记录。
- Chebyshev 四类映射与极端数值、AM-PM/高阶收敛快照噪声的直接 C++ 回归已迁移，见[第一批物理迁移](native-physics-migration-20261013.md)。
- 四个精确变频/噪声 fixture 现已建立直接 C++ 回归：75 Ω 非匹配器件与 45+3j Ω 源的极端参考；部分外部相关 C/P 与严格零内部行列；不同实 DC 参考的完整热噪声升维；75 Ω Touchstone 内嵌噪声至复参考变频与物理 50 Ω 负载的端到端链。见[边界回归及独立预期](native-conversion-boundary-fixtures-20261013.md)。
- 上述六组有限回归迁移不等于完整 Python 退役或厂商兼容完成。下列相干流仅覆盖选定子集；参考比较器仍依赖产品包，实际删除尚未执行。

本次整合后的实际检查及后续远端结果必须对应本次提交，不能借用上面两个旧提交的 CI 结论。

## 结论

RFModel 的数值内核已经是 C++17 header-only 库，交付 `RFModel::rfmodel` 不需要 Python、pip、共享 C ABI 或 Python CLI。可以立即把 C++ 函数接口、安装后的真实调用和独立物理参考作为主验收入口。Python 产品接口可以在后续阶段退役，但不能以“删除 `python/` 后原生测试仍通过”作为无功能损失的证据。

当前 Python 同时承担三种不同责任：

1. ctypes 绑定、对象生命周期、类型转换和 Python 专有兼容契约。
2. JSON/CLI 解析及实质性的跨阶段执行逻辑，包括相干来源历史、参数绑定、频点扩展后的索引重映射。
3. 独立数学/物理参考、SystemVue 数据采集与比较、历史证据核验。

只有第一类可以随对应产品接口一起取消。第二类必须逐项决定迁移为 C++ 行为、由 C++ 调用者显式组合，或明确退出交付范围；第三类应保留或先迁移验证能力。停止交付 Python 包，不等于禁止研发机器用 Python 检查结果。

## 后续增量：有类型相干流子集

`coherent_pipeline.hpp` 现提供 C++17 函数入口，用同一上下文的不可变流组合 seed、mix、单输出 linear_network、polynomial 和仅 source-kind 的共同驱动压缩。迁移的是物理/历史根注册、流局部表达式、相消保留、全历史共轭、规范支持宽度、临时身份恢复、精确来源阶数过滤计数与上下文资源/失败语义；数值计算继续调用既有内核。

直接回归用 long-double 有符号电压谱有序卷积和手算历史/路径/功率预期，覆盖多项式→混频→多项式、跨流复权合路、重复共轭、正频 IF 总幅度相消为零时仍保留的非零交叉历史、独立源功率、共同驱动及资源边界。安装消费者在独立翻译单元中调用同一公开 API。接口与限制见[相干流指南](coherent-pipeline.md)；实际执行结果见[增量验证](../validation/coherent-pipeline-validation-20261013.json)。

这是阶段 2 的选定子集，不表示阶段 2 整体完成。完整图/DAG、JSON/CLI、multi-output 共享提取、其他收敛点噪声和频点重映射仍开放。limited/highorder 及传播模式由下述后续适配增量补充。无 Python/C 文件删除、无新绑定、无 SystemVue 兼容等级提升。下文审计表保留审计时点含义，不能因这个子集通过就删除其余独立参考。

## 后续增量：有界模型放大器适配

`CoherentPipeline` 新增模型接收式 `limited_amplifier`、`highorder_amplifier` 与显式 `AmplifierInputMode`。限幅不做语义来源阶数裁剪，高阶显式 cutoff；两者均用完整物理输入一次求共同工作点，只从 source 历史生成新项，保持相消来源和同源级联身份。API、固定资源边界及安装双翻译单元消费见[放大器适配器](coherent-pipeline-amplifiers.md)。

这不替换阶段 3A 探针或级联评分器，也不解除级联比较器对 Python 图的依赖。已有级联归档 230 项检查、22 项失败与两个 0 dB 差异不因新增 API 而改变。完整图迁移、评分路径迁移与 Python 产品包删除仍是独立的后续工作；本增量没有新的厂商兼容声明。

## 后续增量：阶段 3A 原生高阶证据重放

新增开发专用的四操作 C++ 探针与标准库 Python 传输适配，使五个既有高阶参考套件可以在主动禁止 `rfmodel` 导入的独立进程及无产品包的暂存目录重放。冻结的 1,413 个实测输入 holdout、名义八/十阶失败、缺项、校准/holdout 分离和 `diagnostic_only` 状态继续由原有评分逻辑验收。归档字节不改写。

`RFMODEL_BUILD_REFERENCE_TESTS=ON` 显式启用这一开发依赖，独立于 C ABI 与绑定测试；默认 OFF 的纯 SDK 不查找 Python，不安装探针或适配脚本。构建/执行身份、故障协议、安装消费检查及仍未迁移的级联来源图边界见[原生参考重放](native-reference-replay.md)。这是阶段 3 的有限子集；下文首次审计依赖清单属于历史时点，其余比较器仍未整体脱离产品包，不能据此删除 Python/C 产品接口。

## 一、依赖边界

| 部分 | 当前依赖与责任 | C++ 优先时的处理 |
|---|---|---|
| `include/rfmodel/*.hpp`、`RFModel::rfmodel` | C++17 标准库；核心网络、转换、噪声、非线性局部模型、优化和 Monte Carlo | 主交付；无 Python 运行时依赖 |
| `src/api/c_api.cpp`、`RFModel::c_api` | 包装 C++；共享库供 C 和 ctypes 使用；C 测试另需 C 编译器 | 可关构建；是否退役 C ABI 是独立决定，不能随 Python 自动删除 |
| `python/rfmodel/__init__.py` 等绑定 | 通过 ctypes 加载独立提供的原生库 | 保留现有兼容验证，停止新增包装层优先级 |
| `python/rfmodel/*_file.py`、`expression_system.py` 等 | 解析、图级执行、调度、结果编码；部分行为仅在此层实现 | 先盘点行为，不能当成纯粹的序列化胶水 |
| `python -m rfmodel` | `__main__.py` 分派九种分析和优化/Monte Carlo 任务；保护输入/库/Touchstone 路径 | 属于 Python 产品 CLI；C++ API 不自动提供它的替代品 |
| `src/cli/rfmodel_cli.cpp` / `rfmodel_cli` | 原生 Touchstone 单模型 S 参数 CSV 导出 | 与 Python CLI 不同；无网络 JSON、转换噪声或通用任务输入能力 |
| 四个原生示例 | 混合参考转换、LC 匹配、独立/相关电阻容差，输出 JSON | JSON 输出不代表具有通用 JSON 输入解析器 |
| `scripts/verify-*-example.py` | 读取原生示例 JSON、检查解析物理公式、重放及损坏样本 | 可继续作开发验证工具；不需要作为 SDK 运行时依赖 |
| `scripts/reference/` | 77 个 Python 文件、6 个 PowerShell 文件；厂商元数据、采集、诊断、比较及其测试 | 保留；其中部分比较器依赖 Python 包，应另行适配原生探针 |
| `validation/`、`examples/*.json`、Touchstone 数据 | 历史证据、输入/输出模式、物理/厂商参考 | 不随语言接口删除；保持原始哈希、格式/version 和验收含义 |

包元数据 `python/pyproject.toml` 使用 setuptools，要求 Python ≥3.9，wheel 不捆绑共享库。CMake 安装只安装头文件、CMake 包和已启用的原生目标，并不安装 Python 包。安装的头文件目录仍可能包含 `c_api.h`；头文件存在不意味着消费者需要 C ABI 动态库。

### SystemVue 工具不能按扩展名删除

`run-systemvue-reference.py` 用子进程编排 PowerShell/COM 采集；`compare-attenuator.py`、`compare-antenna.py` 可以调用原生参考可执行文件。这些工具的 Python 语言本身与 `rfmodel` Python 产品包无关。

以下十个参考脚本直接导入 `rfmodel`：

- `compare-antenna-compression.py`、`compare-single-compression.py`
- `compare-cascade-amplifier.py`、`compare-coherent-amplifier.py`
- `compare-coherent-carriers.py`、`compare-coherent-network.py`
- `compare-shared-compression.py`
- `diagnose-am-pm-reference.py`、`diagnose-highorder-amplifier.py`、`diagnose-total-drive.py`

尤其 `scripts/reference/test_highorder_model_reference.py` 经间接导入的 `Library` 重放 1,413 个独立保存的实测输入 holdout，断言相对误差不超过 1e−7，并核对缺失厂商谱项。只留下采集 JSON 而删除可执行比较路径，会失去这部分证据的可重放性。迁移时必须保留校准点与 holdout 的区分、既有 diagnostic/compatibility 状态及未知项。

还存在 `importlib` 间接依赖：例如 `compare-single-harmonics.py` → `compare-single-compression.py`，以及 `compare-multitone-amplifier.py` / `compare-amplifier-terms.py` → `compare-two-tone.py` → `compare-single-compression.py`。只搜索 `import rfmodel` 不能得出安全删除清单。

退役绑定前应把这些比较器的“运行 RFModel”步骤改为调用有版本约定的 C++ 探针或保存的原生结果；保留原来的独立校验、厂商原始数据、单位、时间戳、参数身份、失败分类和容差。采集流程继续遵守 [SystemVue 脚本边界](systemvue-scripting.md)，不能把旧测试通过当作新仿真成功。

## 二、本轮 CMake 行为

本轮已将所有原生测试目标收回 `BUILD_TESTING` 控制下，四个真实示例由新增的 `RFMODEL_BUILD_EXAMPLES` 控制。新增 `RFMODEL_BUILD_PYTHON_TESTS` 允许显式禁用解释器发现和 Python 测试注册。

| 开关 | 默认值 | 实际含义 |
|---|---|---|
| `BUILD_TESTING` | ON | 原生/C/Python 测试目标及 CTest 注册；OFF 不再构建测试二进制 |
| `RFMODEL_BUILD_C_API` | ON | 共享 C ABI 与其 C/C++ ABI 测试；OFF 不启用 C 语言 |
| `RFMODEL_BUILD_CLI` | ON | 原生 S 参数 CSV CLI；其测试另外要求 `BUILD_TESTING` |
| `RFMODEL_BUILD_EXAMPLES` | ON | 四个 C++ 示例；OFF 时不构建、不注册这些示例 |
| `RFMODEL_BUILD_PYTHON_TESTS` | ON | 只有同时启用测试和 C API 才查找 Python 并注册绑定/参考脚本测试；OFF 禁用此路径的 Python 发现；独立的显式参考选项除外 |
| `RFMODEL_BUILD_REFERENCE_TESTS` | OFF | 显式启用开发用原生高阶参考重放；需要测试与 Python ≥3.9，不需要 C ABI 或绑定测试；默认 OFF 不触发该 Python 发现 |
| `RFMODEL_REQUIRE_PYTHON_TESTS` | OFF | ON 时要求 Python ≥3.9；若测试、C API、Python 测试任何一项关闭，配置明确失败 |

`RFMODEL_REQUIRE_PYTHON_TESTS=OFF` 只表示 Python 缺失时允许继续，不能用来禁用已经可发现的 Python。纯 C++ 配置应明确设置 `RFMODEL_BUILD_PYTHON_TESTS=OFF`。

最小 SDK 安装：

```sh
cmake -S . -B build-sdk -DBUILD_TESTING=OFF \
  -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF \
  -DRFMODEL_BUILD_EXAMPLES=OFF -DRFMODEL_BUILD_PYTHON_TESTS=OFF
cmake --build build-sdk --config Release
cmake --install build-sdk --config Release --prefix /your/install/path
```

只运行 C++ 数值测试和示例：

```sh
cmake -S . -B build-cxx -DCMAKE_BUILD_TYPE=Release \
  -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF \
  -DRFMODEL_BUILD_PYTHON_TESTS=OFF
cmake --build build-cxx --config Release --parallel 2
ctest --test-dir build-cxx -C Release --output-on-failure
```

原生测试可以不构建 C ABI；因此 C API 测试通过不能替代上述纯 C++ 配置的覆盖。新增 `tests/consumer/conversion_api.cpp` 与 `conversion_api_probe.cpp` 构成多翻译单元消费端，验证头文件函数跨翻译单元使用。安装消费端必须在独立构建目录中只通过 `find_package(RFModel 0.1.0 EXACT CONFIG REQUIRED)` 和 `RFModel::rfmodel` 使用 SDK，不引用源树头文件或 DLL。具体执行结果按本轮验证记录列出，配置成功不等于测试已经通过。

审计时曾确认的旧行为：C API/CLI OFF 时注册 89 项原生测试；`BUILD_TESTING=OFF` 仍留下 29 个可执行目标（25 个测试、4 个示例）；C API OFF 时 `REQUIRE_PYTHON_TESTS=ON` 被忽略并警告未使用。本轮已修复后两项。最终工作树已通过配置和 `ctest --show-only=json-v1` 核对：上述纯 C++ 配置注册 91 项（原 89 项 + 七通道物理测试 + SDK 配置回归），缓存中没有 Python3 发现项或 C 编译器项；默认全量配置注册 158 项（102 项原生/C/CLI/CMake，56 项解释器驱动）。最小五开关 OFF 配置的目标清单只有标准构建/安装目标，没有可执行文件；矛盾的 `REQUIRE_PYTHON_TESTS=ON` 请求已实测配置失败。这里报告的是配置与清单核对，实际编译/CTest 结果由本轮主验证记录给出。

## 三、现有 CI 与计数口径

现有 `.github/workflows/build.yml` 仍把 Python 与主构建绑定：三平台 × Debug/Release 均 setup Python 3.12，先跑 Python 工具测试，再 `REQUIRE_PYTHON_TESTS=ON` 配置、构建 wheel、运行全量 CTest、安装 SDK、验证独立消费者，最后隔离安装 wheel 并跑绑定套件。它能验证兼容包，但尚不能证明构建机器完全没有 Python 时 C++ 交付仍独立。

本轮修改前最近的全量构建清单有 156 项 CTest：100 项原生/C/CLI，56 项解释器驱动。其中 32 项对应 `tests/test_*.py`，其 unittest 方法总数为 503；另 24 项属于参考比较或示例核验。503 并不是 503 个独立物理模型，也不是 CTest 数量。新增 10 项 API 和 14 项 JSON/CLI 测试，正好使旧 479 项绑定测试变为 503 项。

本轮已另加独立 `cpp-sdk` 三平台 × Debug/Release job：不执行 Python setup，关闭 C ABI/CLI/Python 测试，运行原生回归、最小 SDK 安装及重定位多翻译单元消费者。原全量 job 保留。该工作流只完成本地编辑/语法检查，尚未提交运行，不能计为远端六配置通过。

建议 CI 分开报告：

1. 必需的 C++ SDK 矩阵：禁用 C API/Python 测试；全量原生测试、最小 SDK 安装、独立 C++ 消费端；包含 Debug 和 Release，不能依赖会被 `NDEBUG` 关闭的断言。
2. 保留的 C ABI 兼容矩阵：C ABI 开启但 Python 测试关闭，验证 C ABI、失败原子性和旧消费者；只在明确决定退役 C ABI 后另行调整。
3. 暂时保留的 Python/JSON/旧 wheel 兼容矩阵：继续测试仍被保留的接口；不再作为 C++ 用户安装的前置条件。
4. 独立参考工具矩阵：Python 可以作为开发工具存在；不要求交付 Python 包。依赖绑定的比较器迁移后，应能在无 `PYTHONPATH=python` 的情况下工作。

不能直接删除 CI 的 Python setup 步骤而保留后续 `pip wheel`、验证器和参考工具调用；也不能只给 CTest 加名称过滤就声称未依赖 Python。应检查配置日志、缓存、导出目标和安装消费者实际链接。

## 四、32 个 Python 套件的功能分类

这里按行为分类，不按文件名判定是否可删。一个文件可能同时含 Python 专用契约和独立物理参考。

| 套件（`tests/`） | 方法数 | 已有原生能力 | 删除前应保留/迁移的部分 |
|---|---:|---|---|
| `test_python_api.py`、`test_mixed_reference.py`、`test_power_wave_stages.py`、`test_linear_noise_analysis.py` | 124 | 线性网络、混合参考、噪声、功率波快照、相干/非线性局部原语 | 独立 ABCD/阻抗/噪声参考、Touchstone 到跨阶段分析、来源保存；tuple/句柄/finalizer/ctypes 故障注入可随绑定退役 |
| `test_passive_model_file.py`、`test_butterworth_filter.py`、`test_chebyshev_filter.py` | 30 | 无源器件与滤波器头文件及各自 C++ 测试 | 解析极点、显式 LC/ABCD 对照和极端量级；JSON 字段/CLI 保护仅在继续支持该文件格式时迁移 |
| `test_frequency_conversion.py`、`test_conversion_network.py`、`test_conversion_loaded_noise.py`、`test_conversion_noise.py` | 45 | 固定泵、转换网络、加载 C/P、参考温度实验 | 独立共轭反馈闭式解、边带折叠、热平衡、端口频点映射及连线全局相关性 |
| `test_phase_noise.py`、`test_shared_phase_noise.py`、`test_channel_noise.py` | 37 | 相噪及共享相噪、信道噪声测量 | 独立四相位/潜变量集合、相位消除、边带关联和积分参考；JSON 来源/测量编排需逐项映射 |
| `test_mixer_linearization.py`、`test_affine_conversion.py` | 24 | 混频雅可比、仿射偏置和原生网络 | 独立 Fourier 投影、二阶残差比例、工作点/增量波隔离及反馈物理方程 |
| `test_operating_point.py`、`test_amplifier_operating_point.py`、`test_polynomial_operating_point.py`、`test_highorder_operating_point.py`、`test_am_pm_operating_point.py` | 71 | 原生非线性求解器及各类局部线性化 | 独立时域卷积/标量根、混合模型、收敛点重建噪声和参考实验；JSON 自动构图与覆盖优先级另有责任 |
| `test_conversion_frequency_plan.py`、`test_highorder_frequency_plan.py` | 18 | C++ 频率闭包和规则 | Python 扩展后重映射全部源、噪声、偏置、初值、AM/PM、端口及显式边界，不由单独的原生规划测试自动证明 |
| `test_coherent_network_file.py`、`test_coherent_system_file.py` | 60 | 相干归并、网络、多项式、混频、来源表达式局部原语 | 图级来源/路由/共同驱动是主要未迁移区，见下一节；JSON 解析只占其中一部分 |
| `test_optimization.py`、`test_optimization_file.py` | 18 | C++ 有界优化器与独立 LC 匹配示例 | 参数绑定、目标提取/权重、预算及无额外求值、九类分析分派和文件依赖；Python 异常/回调包装属绑定契约 |
| `test_monte_carlo.py`、`test_monte_carlo_file.py`、`test_monte_carlo_extended.py` | 52 | C++ 抽样、六分布、相关输入、联合统计、重放及 RF 电阻示例 | 保留独立总体统计/物理参考；任务字段、绑定、依赖指纹和结果版本位于 Python；不要重写已在 C++ 中的随机引擎 |
| 新增 `test_conversion_power_wave_api.py`、`test_conversion_power_wave_file.py` | 24 | 大部分核心坐标/物理能力已有原生测试；本轮补七通道独立参考 | 逐项对照如下；不能笼统把 24 项都视为新物理能力或都视为包装测试 |

### 明确存在的滤波器与收敛点组合缺口

`test_chebyshev_filter.py` 的下列独立物理检查不能因存在 `chebyshev_filter_test.cpp` 就全部删除。当前原生套件主要核对低通的递推、多阶模拟极点和 LC 实现，Python 还独立检查高通/带通/带阻的极点频率映射、不同边缘衰减与阶数奇偶性、open/short 对偶、1e−200 与 1e200 附近相邻浮点带边、6000 dB 和 subnormal DC 行为（含频率比本身下溢但反射仍可表示的案例）。这些需先迁入直接 C++ 测试；本轮七通道转换 oracle 不覆盖滤波器。

AM/PM 与高阶放大器在收敛工作点后重建噪声及参考温度实验也是需保留的组合层行为。局部导数原生测试不自动证明 Python 工作点编排使用了最后收敛的模型/雅可比；删除前须以收敛快照做直接 C++ 组合回归。

### 相干图不是包装层

`expression_system.py`、`coherent_origins.py`、`coherent_system_file.py` 的现有职责包括：

- 物理相干身份与流局部来源历史分离；相同物理组在不同流中可有不同贡献，不能用全局组号表覆盖。
- root registry、规范频率/带宽、重复及共轭因子、源阶数上限，以及实际丢弃路径计数。
- 混频差频时对整个历史的幅度和全部因子共同取共轭。
- 临时 native group 只用于路由；恢复实际相干身份后算功率。
- 即使总波相消为零，仍保存非零来源贡献及其后续多项式交叉项，不能除以总波来分摊贡献。
- 压缩/限幅的驱动来自真实相干总功率，再对每条来源施加同一个响应；不能把展开后的各来源当作独立功率求和。
- 全图资源上限、分数带宽的规范化和按实际生成频点执行线性网络。

原生 `origin_expression_test`、`mixing_origin_test`、`coherent_polynomial_test` 等证明局部代数，但不等价于已经实现上述完整图级执行器。删除前至少应把现有独立稀疏谱参考及以下场景移入 C++ 调用链/有类型的执行器测试：polynomial→mixer→polynomial，加入线性复权合路，再次混频的全历史共轭，跨流不同历史，IF 相消后非零来源交叉项，独立源功率合计，source-order 截断计数，以及共同驱动的压缩/高阶放大器。详见 [混合来源图](mixed-polynomial-graphs.md)、[递归来源](recursive-mixing-origins.md)。本轮不实现这一迁移。

### 优化与 Monte Carlo 的边界

`optimization.hpp` 和 `monte_carlo.hpp` 已实现算法，不需要为了移除 Python 再写一套算法。C++ 调用者可直接提供目标/求值回调，配合原生网络 API 完成设计或容差分析。

`parameter_binding.py`、`optimization_file.py`、`monte_carlo_file.py` 实现的却是声明式任务：唯一 selector/path、一个变量绑定多个位置、仿射与整数映射、复指标提取、目标组合、九种分析分派、模板冻结、Touchstone 前后指纹、结果快照与不额外求值。如果最终仅交付回调 API，可明确不交付这些 JSON 任务契约，但相关 RF 物理/统计参考仍应转为 C++ 回调测试。`MemoryError`/`KeyboardInterrupt` 传播、Python tuple 和 ctypes 回调清理则无需伪造为 C++ 功能。

## 五、新增 10 项 Python API 回归逐项对照

下表中的“已有”指物理机制已有直接 C++ 回归，不表示同一 fixture、同一接口形状已经逐字覆盖。C ABI/Python 间相互比较不能作为独立物理参考。

| `test_conversion_power_wave_api.py` 方法 | 现有 C++ 证据 | 退役判断 |
|---|---|---|
| `test_default_tuple_compatibility_and_explicit_scalar_precedence` | `conversion_power_wave_test::test_physical_network` 区分默认参考与器件坐标 | tuple 类型、None、元数据和向量优先级属绑定契约；局部矩阵不二次转换仍保留原生测试 |
| `test_independent_norton_network_full_loaded_cp_and_offsets` | `test_physical_network` 的十二通道独立 V/I 消元，DC/RF、双器件、Norton 偏置、全局相关源和六类加载 C/P；`test_coordinate_change` | 物理机制已有；六通道 Python fixture 可不原样复制，但保留现有独立消元器及全矩阵断言 |
| `test_boundary_matches_thevenin_full_cp_and_retains_internal_zeros` | `test_boundary_change` 已覆盖相关边界及空选择、非法选择 | 正常“部分外部通道、内部行列保持零”的非零 C/P 独立 Thevenin fixture 尚应直接迁移；不能用空选择代替 |
| `test_dc_physical_round_trip_and_identity_representation` | `test_coordinate_change` 精确恒等及 C/P 往返；`test_mixer_dc_coordinates` 的 DC/RF 物理往返 | 基本物理已覆盖；保留 DC A/B 分解不唯一、只保证物理关系的契约 |
| `test_invalid_references_dimensions_dc_and_nonfinite` | `test_linear_lift_and_failures`、`frequency_conversion_test` 等原生输入校验 | Python 的异常类型/数组形状可退出；原生维度、有限数、正实部、DC 及合法 C/P 校验不能删除 |
| `test_singular_transforms_and_complex_dc_boundaries_fail_without_mutation` | `test_boundary_relative_singularity` 的五尺度极点与可解邻例；原生无效坐标/DC 约束 | 极点修正已有直接 C++ 保护；保留不可变输入及 DC 物理限制，Python deepcopy 形状无需迁移 |
| `test_extreme_references_follow_independent_norton_thevenin_formulas` | `test_wide_reference_precision` 已覆盖 1 Ω 匹配器件/源、1e−24…1e24 的信号/偏置/C/P 相对精度 | 新 Python 的 75 Ω 非匹配器件及 45+3j Ω 源是额外独立 fixture；若删此套件，应补到纯 C++，避免只剩匹配特例 |
| `test_ideal_through_needs_no_invertible_physical_z` | 原生转换理想直通；`power_wave_reference_test`/`power_wave_network_test` 的复参考直通和奇异 Z | 机制已覆盖；转换 API 的同一复参考往返可作小型强化，不需要为此构造物理 Z 的逆 |
| `test_explicit_vectors_rejected_for_automatic_operating_point` | `test_linear_lift_and_failures` 保留既有公共实参考工作点入口的拒绝行为；原生另有[混合参考新入口](conversion-operating-point-power-wave.md) | Python 的显式向量（包括全50 Ω）拒绝仍是包装策略；新增原生入口不改变 C/Python/JSON 自动工作点的限制 |
| `test_noise_reference_experiment_coordinate_invariance` | `test_linear_lift_and_failures` 比較参考实验；新增七通道原生参考检查 gain/NF/温度 | 物理主要覆盖；Python 返回字段数及附加参考元数据属绑定契约 |

## 六、新增 14 项 JSON/CLI 回归逐项对照

| `test_conversion_power_wave_file.py` 方法 | 分类与直接原生覆盖 | 保留的差距/处置 |
|---|---|---|
| `test_mixer_resistor_full_physical_waves_and_noise` | 物理 oracle；本轮 `conversion_mixer_physics_test` 补七通道、三个参考尺度、完整局部波与加载 C/P | 必须以独立 Fourier、串联电阻 V/I、实随机基算预期值；不能只对比新旧参考下同一个求解器 |
| `test_example_json_reproduces_independent_physics` | 同一物理案例 + JSON 文档/生成器一致性 | 原生物理可接替；若退役 JSON 输入，文本模式相等无需模拟，但保留示例与历史输入 |
| `test_single_parameter_mixer_transforms_signal_and_preserves_physical_boundary` | `test_mixer_dc_coordinates` 覆盖固定泵变换、物理边界及 DC/RF；七通道新测试覆盖折叠边带 C/P | 参数模型的 JSON 自动变换是适配器责任；直接 C++ 调用显式变换，不应重复变换边界 |
| `test_single_matrix_affine_and_explicit_c_p_are_already_local` | `affine_conversion_test` 闭式共轭反馈；十二通道独立 V/I/噪声覆盖 | “显式矩阵/偏置/C/P 已在局部坐标”是可保留的 C++ 输入契约；JSON 分派不属于新物理 |
| `test_parameter_mixer_explicit_noise_and_offsets_stay_local` | 原生 API 对局部噪声/偏置的处理已有；测试重点是参数生成模型不能二次变换用户显式噪声 | Python-only 生成器优先级；若将来新增 C++ builder，应迁移此断言 |
| `test_linear_static_matrix_is_local_parameter_model_transforms` | 原生复参考电阻、线性重归一化和升维已覆盖其数学 | 显式 S 与参数生成 S 走不同适配路径；若不保留文件适配器，无需实现相同解析器，但建议保留直接电阻闭式参考 |
| `test_dc_lift_noise_and_frequency_major_reference_order` | `test_linear_lift_and_failures` 已检查频点优先顺序、DC P=C、RF P=0 | 新 JSON 还检查两个不同实 DC 参考及完整热噪声矩阵；精确 fixture 属后续小型原生补强，本轮七通道只有正频率 |
| `test_touchstone_embedded_noise_preserves_physical_loads` | Touchstone 噪声导入、线性噪声重归一化分别有原生测试 | 尚缺该完整直接 C++ 链：75 Ω 文件内嵌噪声→逐通道复参考转换提升→物理50 Ω负载；应保留独立矩阵 `kT*[[.96,−.384],[−.384,3.9936]]` 的端到端核对 |
| `test_partial_vectors_report_default_refs_without_changing_local_matrices` | 主要为缺省器件回落与结果元数据/输入语义 | C++ 已要求每个显式模型自行声明参考；没有 JSON 回落时这不是缺失物理功能 |
| `test_global_correlated_source_noise_stays_local` | 十二通道独立 V/I 回归已有跨器件 C/P 和入射—出射互统计 | 保留全局相关块的原生测试；JSON 字段不得二次转换属于适配契约 |
| `test_legacy_output_shape_unchanged` | 纯旧 JSON 结果字段兼容 | 仅在继续维护该 JSON 接口时保留；无需变成 C++ tuple/字段形状测试 |
| `test_invalid_vectors_and_complex_dc_are_rejected` | 原生参考/DC 校验已有；JSON bool/null/编码拒绝属于解析器 | 物理边界保留；格式专用拒绝可随格式正式退役 |
| `test_unsupported_automatic_paths_fail_explicitly` | 原生非线性混合参考限制依然存在 | JSON 禁止 frequency_plan/phase_noise 等组合不表示已有相应组合物理能力；删除解析器不能消除这些未完成项 |
| `test_cli_replay_and_invalid_reference_preserves_output` | CLI 与文件写入保护；成功分支复用七通道物理案例 | 原生函数接口无需输出文件协议；若未来另交付 CLI/序列化工具，需保留确定性重放、输入不覆盖与失败不改旧结果 |

`verify-conversion-power-wave-example.py` 还检查原生示例输出的字节重放、非法缩放以及被篡改的波/互补噪声/电压/净噪声。新的 C++ 数值测试不能自动代替输出模式及故障注入验证；可把该脚本保留为无需 `rfmodel` 包的开发工具，或再移植为独立原生输出测试。本轮不删除它。

## 七、分阶段安全退役

### 阶段 0：建立 C++ 主交付路径（本轮范围）

- 提供可关闭 C ABI、CLI、示例和 Python 测试的最小 SDK 配置；`BUILD_TESTING=OFF` 不残留测试目标。
- 安装后独立 C++ 消费项目真实调用转换参考、物理边界、仿射偏置和加载 C/P，不依赖源码路径或共享库。
- 七通道混频—热电阻独立原生 oracle 成为 C++-only CTest 的一部分，完整检查三个参考尺度的波、六类 C/P、净噪声及参考实验。
- 保留已有 C/Python/JSON 文件，冻结其已完成验证；本轮未执行删除，完整图迁移尚未完成。

出口条件：最小安装与独立消费端通过；C++-only Debug/Release 完整套件通过；结果与历史 156 项/503 项兼容验证分开记录。跨平台结果必须对应本轮代码，不能借用旧提交 CI。

### 阶段 1：冻结产品接口并建立逐项覆盖账本

- 将本页表格细化到每个仍有价值的物理/数学 oracle，记录直接 C++ 入口、独立预期方法、fixture、容差、故障检查及实际执行结果。
- 优先补本页明确列出的有限空缺：非匹配极端参考、部分外部边界 C/P、不同 DC 参考的完整升维热噪声、Touchstone 噪声的完整转换链，以及 Chebyshev 的非低通/极端量级独立参考。
- 明确哪些旧 JSON 格式/高层图执行属于最终交付范围；不因选择 C++ API 而隐含承诺重做所有旧文件格式。
- 保留不导入 `rfmodel` 的独立 Python 工具；同时跟踪会间接导入包的脚本，避免“产品包退役”破坏参考验证。

出口条件：每个准备移除的测试均有“语言专用随接口退役”“直接 C++ 等价覆盖”“明确不再交付的高层契约”之一；独立物理参考不能只标记为包装测试。

### 阶段 2：迁移必要的跨阶段行为

- 对仍需要的相干系统行为，设计有类型的 C++ 流/来源结构或明确的调用组合，移植物理身份与历史身份、来源阶数、全历史共轭、相消保留、共同驱动及资源边界。
- 在原有独立稀疏频谱/时域参考下比较多阶段 polynomial→mixer→polynomial/linear/压缩链；先保留并行结果，不能仅用 Python 旧实现作为唯一真值。
- AM/PM/高阶收敛快照的噪声重建与 NF 组合须补直接 C++ 回归。频率规划与相噪/工作点的重映射、参数任务绑定如仍需交付，则同样用有类型的 C++ 层承接；否则在能力清单明确退出这些高层输入契约。
- 优化/Monte Carlo 复用现有原生算法，以 C++ 回调重现 RF/统计参考，避免改变随机流、计数、预算和失败语义。

出口条件：所需高层能力已有 C++ 入口和独立端到端回归，或用户明确选择不交付该能力。此阶段不在本轮 SDK/oracle 补强范围内。

### 阶段 3：去掉参考工具对产品绑定的依赖

- 用原生参考探针/稳定结果文件替换比较器中对 `Library` 和 Python 图层的调用；输出包含输入、单位、模型版本及来源指纹。
- 在移除 `PYTHONPATH`、不可导入 `rfmodel` 的隔离环境运行参考工具；包括间接 importlib 链。
- 用已有保存采集验证比较器结果及失败分类没有变化；如需新的 SystemVue 数值证据，另行受控采集，不能为了迁移验证擅自改写旧采集。

出口条件：仍需维护的厂商比较、离线诊断和独立示例核验不依赖待退役产品包，且保留失败/未知项。

### 阶段 4：明确确认后实际移除

- 明确版本边界及取消的 Python/JSON/CLI 契约，再删除包、wheel 构建和相应专用测试；不顺带删除 C ABI、原生 CSV CLI、SystemVue 工具、JSON/Touchstone 数据或历史报告。
- 更新 README/安装/接口/示例命令及 CI；审查脚本的直接和间接导入、文档链接及结果格式引用。
- 对源码/已安装 SDK 两种路径执行无 Python 的三平台 Debug/Release 验证；必要时保留一个只读旧兼容环境用于历史结果重放。
- 以本轮之后实际通过的数值/工程证据报告交付状态；Python 退役本身不增加器件实现范围或厂商兼容通过项。

最终删除是后续明确决定，不是这份审计文档执行的动作。当前所有 Python 接口、数据与参考工具均保留。

## 八、仍然开放的能力边界

- 显式复参考与自动非线性工作点的组合，以及 frequency_plan/phase_noise 自动生成与重排后的完整参考映射，未由本轮解决。
- 完整相干图层、JSON 通用输入、声明式优化/Monte Carlo 参数任务尚未迁移为 C++ 高层执行器。
- 本页指出的若干精确物理 fixture 仍需删除前迁移；七通道原生 oracle 不覆盖所有 Touchstone/DC/极端非匹配场景。
- SystemVue 厂商 ZPORT/CS、真实 LO 工作点、完整器件语义和现有数值差异仍保持各自开放状态。

历史证据与本轮范围参见 [转换功率波契约](conversion-power-wave.md)、[验收审计](acceptance-audit-20261012.md)、[安装说明](installation.md)、[旧跨语言验证](../validation/conversion-power-wave-api-validation-20261013.json)。

实际调用和构建命令见 [C++ SDK 指南](cpp-sdk.md)；本轮 Linux 工程结果见 [C++ 优先验证](../validation/cpp-sdk-priority-validation-20261013.json)。
