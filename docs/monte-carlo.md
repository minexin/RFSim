# 原生 Monte Carlo 容差分析

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

`include/rfmodel/analysis/monte_carlo.hpp` 提供 C++17 `run_monte_carlo(parameters, metric_count, evaluate, options, cancelled)`。每次完整参数抽样调用一个返回有限实向量的同步求值函数；调用者可据此重建 RF 网络、噪声或器件模型。分布与选项在入口复制，回调修改原请求不影响本次运行。

## 输入和随机流

参数与指标各允许 1..64 项；samples 默认 1000，允许 1..100000。seed 为 uint64，默认 0。参数按声明顺序抽样，所有分布参数必须有限。

| distribution | first / second | 约束和含义 |
|---|---|---|
| Fixed | 值 / 0 | second 必须为 0 |
| Uniform | 下界 / 上界 | 下界不大于上界；有限极端端点也可使用 |
| LogUniform | 下界 / 上界 | 正上下界；自然对数坐标均匀 |
| Normal | 均值 / 标准差 | 标准差不小于 0；无隐式截断 |
| LogNormal | 对数均值 / 对数标准差 | 对数标准差不小于 0；实际值为 exp(Normal) |
| DiscreteUniform | 整数下界 / 整数上界 | 两端包含，精确整数绝对值不超过 2^53-1 |

默认参数使用独立边缘分布；原生 C++ 的显式高斯 copula 模式见文末。固定值、相等上下界、零标准差不消耗随机字；常量 LogNormal 的结果必须为可表示的有限正数。

随机算法标识为 `mt19937_64-box-muller52-v1`。引擎为 C++ 标准 `std::mt19937_64`，直接以 seed 初始化。Uniform 每次取一个 64 位字的高 53 位乘 2^-53。Normal 使用两个字，各取高 52 位加 0.5 后乘 2^-52，形成严格位于 (0,1) 的数 u、v，再计算 `sqrt(-2*log(u))*cos(2*pi*v)`；不缓存正弦配对值。LogNormal 复用这一正态生成器。离散整数采用 64 位拒绝映射：宽度 w，上界前缀门槛 `(uint64(0)-w)%w`，拒绝小于门槛的字，再取模 w。random_words 包含拒绝消耗。

同一二进制及浮点环境、相同输入和确定性回调可逐样本重放；record_history 不影响随机流与统计结果。原始整数引擎序列由标准规定，但 log、exp、cos、sqrt 等跨平台末位不承诺一致。Uniform 在浮点舍入下可能得到任一边界。LogUniform 的指数舍入结果限制在声明区间；Normal/LogNormal 不裁剪无界尾部，可表示性失败按失败样本处理。

标准引擎第 10000 个默认种子输出固定为 9981545732273789042；回归据 [C++ 标准草案预定义引擎](https://eel.is/c++draft/rand.predef) 的数值检查公开抽样入口。

## 失败、计数与取消

每个样本先生成完整参数向量，即便前面的某个参数不可表示，仍消耗后续参数的抽样字。随后才决定是否调用求值函数。样本不会被隐式替换，samples 是尝试预算。

- samples_attempted：已生成的参数向量数。
- evaluations：实际求值调用数，包括失败调用。
- accepted_samples：维数正确且每项有限的求值结果数。
- rejected_samples：采样或求值失败数；两类样本之和等于 samples_attempted。
- 默认 Stop：遇首个失败，分别以 sampling_failure 或 evaluation_failure 停止。
- 显式 Reject：失败计入预算，继续尝试；completed 仅表示用完尝试预算，可能没有有效统计。

history 默认启用，每条记录 index、parameters、values、status、error、passed。状态区分 Accepted、SamplingFailure、NonFiniteMeasurement、InvalidMeasurementCount、Exception。失败条目的 values 为空，last_failed_parameters/last_error 保留最后一次失败；参数采样溢出时参数向量本身可以含非有限数。异常信息最多保留 512 字节。一般 C++ 与非标准异常记为样本失败，bad_alloc 直接传播。取消回调的异常直接传播。

取消在生成每个向量前检查，不中断正在执行的求值函数。首次取消时没有抽样、没有求值，也没有统计值；之前已完成的有效样本仍返回统计。回调可以重入 RFModel，不能使本接口并行执行或自动重试。

## 统计与合格比例

每个指标返回 count、mean、minimum、maximum、sample_variance、sample_standard_deviation、standard_error 以及指定分位数。统计仅使用有效样本。没有有效样本时 has_statistics=false，均值/极值为 NaN，方差/标准差/标准误为 nullopt，分位数为空。仅一个样本时后三项同样为 nullopt。

样本方差分母为 n-1，标准误为样本标准差除以 sqrt(n)。实现保留每列有效值，以极值中点中心化并缩放，补偿求和后做第二遍离差计算，避免大偏置小容差导致的灾难性抵消。方差、标准差、标准误分别判断可表示性；某一统计量溢出仅使该项 nullopt，不丢弃有限观测。极小结果按 binary64 舍入，可能下溢到零。标准误用于描述均值估计，不自动构造置信区间或声称任意分布的覆盖率。样本方差定义参见 [NIST 离散程度说明](https://www.itl.nist.gov/div898/handbook/eda/section3/eda356.htm)。

quantile_levels 默认 [0.05,0.5,0.95]，允许空列表或最多 256 个严格递增的 [0,1] 有限数。采用排序后 h=(n-1)p 的相邻点线性插值（type 7），端点为样本极值。关闭 history 仍需 O(samples*metric_count) 的指标存储，以保证统计与精确分位数一致；不是流式常数内存接口。

criteria 为空时不计算合格比例，否则必须与指标数相同，至少有一个上下界。每个存在的界必须有限，同一指标 lower<=upper。所有指标的界共同成立才合格，边界包含。passed 仅在 criteria_enabled=true 时有意义。

- passed_fraction = passed_samples / samples_attempted；失败样本不计为通过。
- valid_pass_fraction = passed_samples / accepted_samples；只在有效样本中计算。

相应分母为零或未启用 criteria 时返回 nullopt。这两种操作性比例不能不加说明地称为实际物理良率；Reject 的条件统计可能因无效样本的排除而偏置。

## 可运行 RF 参考

构建目标 `rfmodel_tolerance_analysis` 使用 R1~Uniform(45,55) 欧姆、R2=50 欧姆，50 欧姆端口、1 GHz，串联两只电阻，seed=20261012，4096 次尝试。输出完整 JSON 输入、统计、合格计数及所有轨迹。这是示例输出格式，尚非通用任务输入。

独立闭式参考为 `|S21|^2=10000/(150+R1)^2`，`|S11|^2=((R1+50)/(R1+150))^2`。每个网络求解结果均与闭式结果核对，允许绝对误差 1e-12。要求传输功率不小于 0.25 且反射功率不大于 0.25 等价于 R1<=50，故总体合格概率为 0.5。

传输功率总体均值为 `1000*(1/195-1/205)`，二阶矩为 `1e7/3*(195^-3-205^-3)`。测试在观察结果之前固定均值及通过比例容差为六倍总体标准误，并以预设 alpha=1e-6 的 DKW 概率坐标包络检查分位数，另加 1/n 插值余量。完整样本再次独立核对样本均值及方差。分布单元测试另以 50000 个样本检查六种边缘分布的解析均值和方差。

原生阶段的范围是 C++ 接口、安装消费端和解析 RF 示例；后续 C ABI/Python 接入见下文。声明式任务与参数绑定、相关输入/协方差、厂商随机算法及 SystemVue 容差分析对照仍未完成，A09 保持开放。

## 原生阶段验证（8fc3710）

验证入口为 `monte_carlo_test`、`monte_carlo_resistor` 两个 CTest 用例及安装后的消费端测试。可用 `ctest --test-dir build -C Release -R monte_carlo --output-on-failure` 重跑新增用例；`rfmodel_tolerance_analysis` 的标准输出即完整 JSON 轨迹。示例源码为 [monte_carlo_resistor.cpp](../examples/analysis/monte_carlo_resistor.cpp)。

[机读验证报告](../validation/monte-carlo-native-validation-20261012.json)记录四个本地配置的核心、安装、同环境重放、独立 Python 统计核对和源文件指纹；[完整电阻容差轨迹](../validation/monte-carlo-resistor-example-20261012.json)保留全部 4096 个样本。原生阶段 C ABI 与 Python 源码未改动，本地完整 CTest 包含原有绑定测试，没有另行构建安装新 wheel；干净检出的三平台 wheel 回归随阶段推送执行。报告中发布状态保留生成时刻的含义。

## C ABI 与 Python 同步回调

`rfmodel_run_monte_carlo` 现提供与原生 C++ 相同的采样/统计语义，不改变已有 ABI 布局。新结构体、枚举与函数声明见 [c_api.h](../include/rfmodel/api/c_api.h)。结果是独立、只读的 `rfmodel_monte_carlo_result`，由 `rfmodel_monte_carlo_destroy` 释放；输入和 options 指向的分位数/判据数组在首次回调前复制。

C 求值回调接收只读参数、指标输出缓冲区、其容量以及 value_count 输出指针。缓冲区容量就是 metric_count，初值为 NaN，value_count 初值为零。成功时写满指标并设置实际个数，再返回 0；个数不符记为 InvalidMeasurementCount，未写满或非有限值记为 NonFiniteMeasurement。不得越界写入。非零返回记为 Exception，可写最多 512 字节诊断；库保证第 513 字节为 NUL。取消回调返回零继续，正值取消，负值表示 API 失败。回调可重入 RFModel，但不能跨 C 边界抛异常，也不能修改调用方 out 槽。

out 初始必须为 NULL，且不与请求及其数组重叠。NULL options 使用原生默认值；非 NULL options 中 quantile_count=0 关闭分位数，criteria_count=0 关闭判定，否则判据数必须等于 metric_count。所有布尔字段要求 0/1，不存在的上下界值必须为零。API 失败不写入 out；正常返回的句柄也可能报告采样失败、求值失败或取消，调用方必须检查 stop。

读取接口包括 summary、quantile_levels、逐指标 statistics、last_failure 和逐条 sample。statistics 在分位数缓冲区 NULL/0 时只查询元数据；sample 在三个数据缓冲区均 NULL/0 时只查询元数据。失败样本的 value_count 为零，可用 NULL/0 读取其空指标。所有查询均先完成全部容量、溢出和输出重叠检查，再写入结果；错误不改变输出，成功不触及缓冲区剩余区域。查询不会重新执行回调。调用方不得在读取期间销毁同一句柄。

C 统计结构用 has_sample_variance、has_sample_standard_deviation、has_standard_error 显式表示可用性，不可用时对应数值置零。没有有效样本时 mean/minimum/maximum 为 NaN。两个比例分别有可用性标志，不可用时数值置零。采样失败参数可含非有限值，不能不加处理直接输出为标准 JSON。

Python 的 `Library.run_monte_carlo` 提供相同入口，参数为 `MonteCarloParameter(distribution, first, second=0)` 或同字段 dict，分布名为 fixed、uniform、log_uniform、normal、log_normal、discrete_uniform。判据为 `MonteCarloLimits(lower=None, upper=None)` 或同字段 dict。seed 只接受 0..2^64-1 的 Python int，布尔值和浮点数不作为种子。其余选项沿用原生命名和默认值。

```python
import rfmodel
lib = rfmodel.Library('path/to/rfmodel_c.dll')
result = lib.run_monte_carlo(
    [rfmodel.MonteCarloParameter('uniform', 45, 55)],
    1,
    lambda x: [10000 / (150 + x[0])**2],
    samples=4096, seed=20261012,
    criteria=[rfmodel.MonteCarloLimits(lower=0.25)],
)
print(result.statistics[0].mean, result.passed_fraction)
```

回调收到不可变 tuple，返回指标的可迭代对象；最多读取 metric_count+1 项以判断维数错误，不耗尽任意长生成器。参数、分位数及判据输入也受长度上限约束。结果及其统计、历史为不可变 NamedTuple/tuple，原生句柄在结果复制后释放，读取过程不调用求值函数。没有统计量和失败参数中的非有限值映射为 None；下溢到零等有限失败参数保留数值。未启用判据时每条样本的 passed 为 None。因而快照可用 `json.dumps(result._asdict(), allow_nan=False)` 序列化，但深层 NamedTuple 将按数组输出；命名字段 JSON 可参见完整 Python 示例的显式转换。

普通 Python 求值异常成为样本失败；MemoryError、KeyboardInterrupt、SystemExit 和其他非 Exception 的 BaseException 在安全返回后重新抛出，Reject 策略也不会继续调用下一次求值。取消回调异常或非 bool 返回同样重新抛出。结果提取失败仍释放句柄；诊断截断保持有效 UTF-8，异常文本格式化本身出错也会重新抛出。

[Python 电阻容差示例](../examples/monte_carlo_resistor.py)重建与 C++ 示例相同的两电阻网络，通过 C ABI 逐样本求解，再核对解析电路和统计值。运行方式为 `python examples/monte_carlo_resistor.py path/to/native/library`；加 `--installed` 时使用已安装包，配合隔离安装环境验证。此示例不构成通用 Monte Carlo 任务格式。

本次 wheel 需要包含上述新导出符号的配套原生库；仅替换 Python 包不能使旧 SDK 获得新能力。C/Python 阶段后的声明式任务与参数绑定见下文；相关随机输入/协方差及厂商对照仍待完成，A09 整项继续保持开放。


## C/Python 阶段验证

[跨语言机读报告](../validation/monte-carlo-api-validation-20261012.json)分别记录 Windows/Linux Debug/Release 的全量 CTest、C/C++ 安装消费端、安装 wheel 与源文件指纹。新增纯 C 回归覆盖输出失败原子性和所有状态，17 项 Python 测试覆盖回调、句柄释放、统计边界与实际 RF 网络；这些测试通过安装 wheel 验证工具自动纳入三平台 CI。

[安装后 Python 完整轨迹](../validation/monte-carlo-python-resistor-example-20261012.json)保存 4096 个样本及全部输入选项。每个配置均与本配置的 C++ 示例逐点比较参数和指标，并独立计算电路与统计参考。原生随机流/统计实现的源码指纹保持不变，前一阶段远程验证见 [8fc3710 的 CI 记录](../validation/monte-carlo-native-ci-validation-20261012.json)。

四个本地配置实际结果为：各 141/141 核心测试、2/2 安装消费端、427/427 安装 wheel 回归。完整 C++/Python 参数轨迹相同，最大指标差 5.56e-17，独立电路误差不超过 2.78e-16。Linux 首次 wheel 安装因系统 Python 缺少 pip 停止，随后使用工作区既有环境和独立安装目录完成全部验证；报告保留首次失败和成功重跑的证据。

## 声明式 Monte Carlo 任务与 CLI

`rfmodel.monte-carlo` version 1 把参数抽样、绑定、分析、测量和合格判定连成一个可保存任务。Python 入口为 `rfmodel.monte_carlo_file.analyze_monte_carlo(library, document, base_directory=..., cancelled=...)`；CLI 自动按 format 分派：

```text
python -m rfmodel examples/monte-carlo-resistor-tolerance.json --library path/to/native/library --output result.json
```

必需字段是 format、version、analysis、parameters、metrics，可选 options。analysis 使用现有九类格式之一：linear-network、mixer-linearization、conversion-network、frequency-conversion、coherence、coherent-network、coherent-system、spectrum-chain、amplifier-components，完整前缀均为 `rfmodel.`。不支持在任务中嵌套优化或 Monte Carlo 任务。直接 Python 调用也要求任务可序列化为有限数值的 JSON；样本失败的结果因此能保留完整原输入并严格序列化。

参数为 1..64 项，每项含唯一 id、distribution、bindings，可选说明性 unit。distribution 按下表填写具名字段，不接收其他字段；这些字段转换为原生分布参数，不在 Python 重做随机生成。

| kind | 必需数值字段 | 含义 |
|---|---|---|
| fixed | value | 固定实值 |
| uniform | lower、upper | 实数均匀上下界 |
| log_uniform | lower、upper | 正实数均匀对数上下界 |
| discrete_uniform | lower、upper | 两端包含的精确整数上下界 |
| normal | mean、standard_deviation | 正态均值及标准差 |
| log_normal | log_mean、log_standard_deviation | 自然对数坐标的均值及标准差 |

分布合法性与可表示性沿用原生契约。bindings 每项含 path，可选 scale=1、offset=0，把抽样值 x 映射为 `scale*x+offset`；每个参数可绑定最多 256 处，总数不超过 4096。多个目标绑定同一参数时复用同一个样本，不分别抽样，可表达器件参数间的共同偏差或反向偏差；潜在抽样变量本身仍为独立分布，尚未支持任意相关矩阵或协方差输出。

path 沿用优化任务的键、非负数组索引及单字段唯一选择器，例如 `["devices",{"id":"R1"},"model","resistance_ohms"]`。路径在模板中解析为固定位置，只能修改已有数值叶节点，不能增加字段、绑定字符串或改变 format version。同一目标的不同路径表示也判为重叠并拒绝。

scale 必须非零且有限，offset 必须有限。Fixed、Uniform、LogUniform、DiscreteUniform 的支撑端点在运行前检查映射可表示性；Normal/LogNormal 没有有限支撑区间，逐样本检查映射，不能以裁剪或重抽样隐藏无效尾部。数值映射有效不代表电路模型一定有效，例如负电阻仍由模型验证报告为失败样本。

DiscreteUniform 在 scale/offset 都是整数时采用精确整数映射并写入 Python int，系数和结果限制在 ±(2^53-1)。需要固定整数频点或计数时，使用上下界相同的 DiscreteUniform；Fixed 和连续分布仍写实数值，不自动取整以通过整数模型字段验证。非整数的仿射系数按实数映射处理。

metrics 为 1..64 项，每项含唯一 id 和结果 path，可选 component、unit、limits。结果路径每次在实际分析输出中解析，选择器必须唯一。component 默认 value，要求有限实数；还支持 real、imag、magnitude、power、magnitude_db、phase_radians。复数功率为模平方，magnitude_db 为 `20*log10(abs(z))`，零幅度不制造有限 dB 地板。unit 只记录单位，不隐式做单位换算。

limits 含有限 lower 和/或 upper；显式空 limits 无效。未写 limits 的指标不限制通过与否。所有指标的界共同成立才合格，包含边界；所有指标均无 limits 时关闭判定。上下界顺序、失败计数及两个通过比例的定义均由原生 Monte Carlo 执行。

options 可写 samples、seed、invalid_sample、record_history、quantile_levels，沿用原生默认值。quantile_levels 必须为 JSON 数组。seed 为 0..2^64-1 的整数，读取 JSON 的客户端须保留其精度。任务不另设目标函数权重或罚函数：各指标独立统计，limits 用于联合通过判定。

结果格式为 `rfmodel.monte-carlo-results` version 1，包含算法标识、parameter_order、参数与绑定、metric_order、指标与判据、补齐默认值的 options、原始 input_analysis、Touchstone 依赖指纹，以及原生 result 的统计、计数、失败诊断和可选完整历史。统计和每条 values 按 metric_order 对齐；抽样参数按 parameter_order 对齐。历史参数是绑定前的潜在变量，实际模型值由记录的仿射映射重建。

新增 analysis_evaluations 表示实际进入所选分析入口的调用数；原生 evaluations 包括进入回调后发生的绑定失败，因此可能更大。采样失败不进入回调；映射失败不进入分析；分析或指标提取失败不计为有效样本。导出不额外调用分析，不保存每个样本的完整模型/分析副本；关闭历史不会改变随机流、预算或统计。取消、Stop/Reject 和不可用值 None 沿用 C/Python 契约。

Touchstone 文件在运行前后核对 SHA-256，变化则拒绝整份任务结果；这不是运行期间字节冻结，不能检测被修改后又还原的中间状态。相对数据路径按输入文件目录解释，单独提取 input_analysis 重跑时应使用同一基准目录。CLI 保护任务文件、原生库和 Touchstone 文件，也检测硬链接别名；schema、依赖变化或序列化错误发生时不写结果文件。正常生成一个带 sampling_failure/evaluation_failure/cancelled 的结果仍返回 CLI 状态 0；调用者必须检查 stop 和计数，不能仅以退出码认定全部样本有效。

### 两个可重放 RF 任务

[电阻容差任务](../examples/monte-carlo-resistor-tolerance.json)复现前述 4096 样本的两电阻网络，逐样本与闭式传输/反射公式核对，合格条件对应 R1≤50 欧姆。

[镜像抑制相位容差任务](../examples/monte-carlo-image-phase-tolerance.json)使用现有相干系统模型，令相位误差 δ~Uniform(-0.2,0.2) rad，并绑定 LO 相位为 π/2+δ，抽样 1024 次。独立参考为镜像功率 `(1-cos(δ))/2` W 和目标边带功率 `(1+cos(δ))/2` W。要求镜像≤0.005 W 且目标≥0.995 W，精确总体通过概率为 `acos(0.99)/0.2`。镜像总体均值为 `(1-sin(b)/b)/2`、方差为 `(1+sin(2b)/(2b))/8-(sin(b)/b)^2/4`，其中 b=0.2。统计一致性在观察结果前固定为六倍总体标准误，同时逐样本检查电路公式并重算样本统计。

测试覆盖全部九类分析的固定任务、六种分布的原生随机流、多处共享绑定、整数频点、全部复数投影、失败和取消、无额外分析、输入冻结以及 CLI/Touchstone 保护。共享路径/测量工具已抽取给优化与 Monte Carlo 共用，原有优化任务回归仍须通过。任意相关随机输入/协方差、自动置信区间和 SystemVue 容差/优化算法对照仍未完成，A09 保持开放。

[本阶段机读验证](../validation/monte-carlo-task-validation-20261012.json)记录各平台的构建、完整 CTest、安装消费端、隔离 wheel、原生文件未变指纹和独立数值核对。[电阻任务完整结果](../validation/monte-carlo-task-resistor-tolerance-example-20261012.json)与[相位任务完整结果](../validation/monte-carlo-task-image-phase-tolerance-example-20261012.json)保存输入、种子、绑定、判据、统计和全部样本。报告中的发布状态是生成时快照，远程 CI 随随后提交推送核对。


## 原生相关输入与联合统计

C++ `MonteCarloOptions` 新增 `latent_correlation` 和 `joint_statistics=false`。相关矩阵为空时保持原有独立随机算法及消耗顺序；只打开联合统计也不改变随机流、回调次数或任何边缘统计。本原生阶段的 C ABI、Python 和 JSON 任务仍使用独立模式；后续扩展见下节。它们的既有结构布局未改变。C++ 头文件使用者需重新编译，不能混用新旧 C++ 类型布局。

```cpp
rfmodel::MonteCarloOptions options;
options.seed = 20261012;
options.samples = 4096;
options.latent_correlation = {1., .8, .8, 1.};
options.joint_statistics = true;
auto result = rfmodel::run_monte_carlo(
    {{rfmodel::MonteCarloDistribution::Normal, 50., 2.},
     {rfmodel::MonteCarloDistribution::Normal, 50., 2.}},
    2, [](const auto& values) { return values; }, options);
// result.parameter_joint.sample_covariance[1] estimates 3.2 ohm^2.
```

### 相关矩阵和数值分解

`latent_correlation` 是按参数声明顺序排列的 N×N 行优先实矩阵，包括常量参数对应的潜在坐标。元素必须有限、位于 [-1,1]、精确对称，主对角必须精确为 1；尺寸和半正定性在任何求值或取消回调前检查。常量实际参数没有定义 Pearson 相关，但其潜在坐标仍参与矩阵检查及随机字预算。

分解采用确定性的对角主元 Cholesky，相等最大对角选择当前顺序中第一个。每一步对剩余矩阵作对称置换，最终把因子行恢复为参数声明顺序。允许半正定奇异矩阵，因此支持完全相关、完全反相关和低秩共同偏差。

数值阈值固定为 `t=64*N*epsilon(double)`。当最大剩余对角不超过 t 时，仅当整个剩余块各项绝对值都不超过 t 才丢弃该块；否则拒绝。因子每行归一化以保持单位潜在方差，重建值与输入各项之差必须不超过 4t。结果公开 `latent_rank` 与 `effective_latent_correlation`，记录实际因子的 Gram 矩阵。接近机器精度的残差可能被截断，这不是接受任意非半正定矩阵的最近相关矩阵拟合器；极接近奇异边界的秩可能受浮点环境影响。

### 分布、随机流与重放

相关模式的 `result.algorithm` 为 `mt19937_64-box-muller52-gaussian-copula-pivoted-v1`。每次尝试先按潜在列顺序生成 N 个原有 Box–Muller 正态值，即固定消耗 2N 个随机字，再乘上述因子。即使矩阵秩小于 N 或含常量参数，也完整消耗此预算。取消发生在本次生成之前；某个边缘变换失败仍生成完整向量，不重抽或裁剪无界分布。

Normal 直接使用 `mean + standard_deviation*z`，LogNormal 再取指数；其他连续分布先计算 `u=Phi(z)=0.5*erfc(-z/sqrt(2))`，再作对应均匀或对数均匀逆变换。DiscreteUniform 使用 `lower + min(floor(u*width), width-1)` 的整数结果；相等上下界直接返回常量。Fixed 不受 z 影响。正态 CDF 变换可参考 [NIST 相关均匀随机数说明](https://www.itl.nist.gov/div898/software/dataplot/refman2/auxillar/mvuran.htm)，erfc 表达式见 [Boost 正态分布实现说明](https://www.boost.org/doc/libs/1_49_0/libs/math/doc/sf_and_dist/html/math_toolkit/dist/dist_ref/dists/normal_dist.html)。实现无需依赖 Boost。

输入描述的是潜在正态相关；除非边缘为非退化 Normal，不能把它解释为输出 Pearson 相关。非正态关联由高斯 copula 决定，并不覆盖任意联合分布或尾部依赖，也不反求给定物理 Pearson 相关的潜在矩阵。CDF 和逆变换使用 binary64：尾部可能舍入到 0/1，离散宽度很大时分箱精度受限；此模式不提供独立整数模式中原始 64 位拒绝映射的精确等概率保证。所有整数仍在声明的安全整数支撑内。显式单位矩阵也选择新的 copula 随机流，不能要求它与旧 Uniform/DiscreteUniform 逐点相同；非退化 Normal 的单位矩阵则保持原正态流。跨平台超越函数末位不承诺一致。

### 联合统计与有效样本

`joint_statistics=true` 返回 `parameter_joint` 和 `metric_joint`，各含 dimension、count、行优先 `sample_covariance` 与 `sample_correlation`。二者严格使用同一批有效求值样本；参数统计同样排除采样/求值失败行，因此 Reject 下可能是条件分布统计。未启用时 dimension/count 为 0，两个数组为空；启用但无有效样本时维度仍保留，各元素不可用。

协方差分母为 n-1；n<2 时所有元素为 nullopt。常量列的协方差为 0，但与它相关的 Pearson 系数不可用。对非退化列，相关系数主对角为 1；矩阵上下三角逐位相同。某个协方差溢出时仅该项不可用，相关系数仍可能可用；方差或协方差下溢到零也不自动使其数学上非退化的相关系数不可用。

计算沿用极值中点中心化、尺度归一化和补偿累加，跨尺度恢复使用二进制指数分解，避免先乘两个大尺度导致不必要溢出。所有联合矩在分位数排序前计算，保留行配对。开启后额外保留 O(samples*parameter_count) 参数数据，矩阵计算代价 O(samples*(parameter_count²+metric_count²))；没有参数与指标之间的交叉矩阵，也不自动提供置信区间。

### 相关电阻网络验收

新增目标 `rfmodel_correlated_tolerance`：两只串联电阻分别为 Normal(50,2) 欧姆，潜在相关 0.8，50 欧姆参考、1 GHz、seed=20261012、4096 次。每次调用网络求解器，记录传输/反射功率和由实际 S21 反推的总电阻。独立电路参考逐点为 `(100/(100+R1+R2))²`、`((R1+R2)/(100+R1+R2))²` 和 `R1+R2`。

总体总电阻均值为 100、方差为 14.4，两个电阻协方差为 3.2；传输≥0.25 且反射≤0.25 对应总电阻≤100，未排除极端无效电阻的总电阻正态模型给出解析通过概率 0.5。正态存在无界负尾，器件拒绝负电阻时仍按既有失败策略处理，不能把未观察到极端失败当作分布截断。预先设定均值、方差、协方差和通过比例各六倍总体标准误的一致性门槛；另独立重算全部轨迹的样本矩与分位数。

[本阶段机器验证](../validation/monte-carlo-joint-validation-20261012.json)和[相关电阻完整轨迹](../validation/monte-carlo-correlated-resistors-example-20261012.json)记录实际结果。前一任务层提交的六配置结果见 [9dc33fa CI 记录](../validation/monte-carlo-task-ci-validation-20261012.json)。本阶段不增加 SystemVue 厂商验收项；本原生阶段留下的跨语言扩展见下节；厂商 Monte Carlo/优化对照继续开放。

本地 Windows/Linux Debug/Release 均完成 clean-first 构建，各通过 144/144 CTest、2/2 安装消费者、444/444 安装 wheel 回归。四组相关电阻示例各 4096 样本，2066 个通过；功率逐点误差不超过 2.78e-16，总电阻误差不超过 1.14e-13 欧姆。四项总体一致性偏差最大 0.5625 个标准误，同环境完整输出重放逐字节相同。报告中的发布状态为生成时快照，当前提交的远端结果在推送后独立核查。


## 相关采样的 C/Python 接口与 JSON v2

相关采样、矩阵验证与联合统计继续只由原生 C++ 实现；适配层负责输入复制、回调及只读结果提取。旧 C 结构体和旧函数签名、Python 原 19 字段 `MonteCarloResult`、JSON v1 的选项和输出形状均保留。扩展使用新增符号，C ABI 主版本仍为 1；新的 wheel 必须配套含新增符号的库，不能据相同 ABI 主版本推断旧动态库已有这些功能。

### C ABI 扩展

`rfmodel_run_monte_carlo_extended` 在原 options 之后增加可空 `rfmodel_monte_carlo_extension*`，其余参数、回调及所有权规则与原函数一致。extension 包含 latent_correlation 指针、correlation_count 和 0/1 joint_statistics。correlation_count 为 0 时保留独立随机流，否则必须等于参数数目的平方，并指向完整行优先矩阵。空 extension 等价于旧入口；原函数本身继续返回独立采样结果。

新请求结构和矩阵均不能与 out 槽重叠，数据在任何回调前复制。矩阵数值错误在求值和取消回调前返回；API 失败保持 out 不变。扩展没有绕过原有失败策略或重新执行分析。

`rfmodel_monte_carlo_get_dependence(result, info, effective_correlation, capacity)` 返回 correlation_count、latent_rank、algorithm 和 joint_statistics 开关。算法枚举 0 为原 `mt19937_64-box-muller52-v1`，1 为 `mt19937_64-box-muller52-gaussian-copula-pivoted-v1`。effective_correlation 是实际因子的行优先相关矩阵；独立模式长度为零。NULL/0 缓冲区仅查询元数据。

`rfmodel_monte_carlo_get_joint_statistics(result, parameters, info, values, capacity)` 的 parameters 只能为 1（参数）或 0（指标）。info 返回 dimension/count，values 为 dimension² 项行优先 `rfmodel_monte_carlo_joint_value`。每项包含 has_covariance、has_correlation 和两个值；不可用值的标志为 0、数值为 0。NULL/0 只查询元数据，非空缓冲区须容纳完整矩阵。关闭联合统计时维度、计数为 0；开启但无有效样本时仍保留维度，所有元素不可用。

两个读取接口先检查全部容量、字节溢出和输出重叠，再写入。错误不修改任何输出，成功不触及缓冲区尾部；输出不可与彼此或 opaque result 对象重叠。查询旧函数生成的结果也有效，会得到独立算法与禁用的联合统计。所有查询均不调用求值函数；句柄生命周期仍由 destroy 管理。

### Python 扩展快照

`Library.run_monte_carlo_extended` 接收原入口的全部选项，另加 `latent_correlation=()` 和 `joint_statistics=False`。矩阵为扁平的可迭代对象，顺序与 parameters 一致；长度必须为 0 或 N²，迭代读取有界，布尔值、字符串和嵌套行列表不作为矩阵元素。joint_statistics 必须为 bool。

返回新的不可变 `MonteCarloExtendedResult`，字段为 result、latent_rank、effective_latent_correlation、parameter_joint、metric_joint。result 是原 19 字段 `MonteCarloResult`，其中 algorithm 来自实际原生模式。两个 joint 对象为不可变 `MonteCarloJointStatistics(dimension, count, sample_covariance, sample_correlation)`，矩阵保存为扁平 tuple，不可用元素为 None。例：

```python
extended = lib.run_monte_carlo_extended(
    [rfmodel.MonteCarloParameter('normal', 50, 2)] * 2,
    2, lambda values: values, samples=4096, seed=20261012,
    latent_correlation=[1, .8, .8, 1], joint_statistics=True,
)
print(extended.result.algorithm, extended.parameter_joint.sample_covariance)
```

不带新选项的 `Library.run_monte_carlo` 保留原返回类型、字段数和独立流。扩展入口不自动把非正态输出 Pearson 相关解释为输入矩阵。普通求值异常、致命异常、取消和读取失败继续遵循原契约；任何结果提取失败都会释放已创建的原生句柄。快照无借用指针，可严格 JSON 序列化，但 NamedTuple 的嵌套命名字段需显式 `_asdict()`，如下面的任务层结果。

### JSON v2 与可重放任务

输入仍为 `format: "rfmodel.monte-carlo"`，显式 `version: 2` 开放 options.latent_correlation（扁平 JSON 数组，默认 []）和 options.joint_statistics（bool，默认 false）。其余参数绑定、九类分析、指标、失败策略和文件保护继承 v1。v1 拒绝这两个新字段；未知版本和布尔/浮点版本号继续拒绝。没有显式矩阵时，v2 也保持独立随机流。

输出为 `rfmodel.monte-carlo-results` version 2。result 保留原字段并增加 latent_rank、effective_latent_correlation、parameter_joint、metric_joint，后两者为具名对象及行优先数组。矩阵坐标分别对应 parameter_order 与 metric_order，参数单位的乘积就是参数协方差单位；metric covariance 同理。输入 options 完整记录请求矩阵，结果单独保留实际有效矩阵。版本号、模板和选项在回调前冻结；输出不增加预算外分析。

[相关电阻 JSON v2 任务](../examples/monte-carlo-correlated-resistors.json)复现原生例子的两个电阻和功率指标。执行：

```text
python -m rfmodel examples/monte-carlo-correlated-resistors.json --library path/to/native/library --output result.json
```

该任务逐点与原生 C++ 的参数及前两个指标比较，同时独立计算闭式电路、全部样本联合矩和分位数；不是用另一个封装的同一输出替代数学核对。v1 示例继续通过原有回归，v2 对九类分析各有实际调用验证。机读证据见 [跨语言相关采样验证](../validation/monte-carlo-joint-api-validation-20261012.json)，[v2 完整结果](../validation/monte-carlo-correlated-task-example-20261012.json)保留全部 4096 行输入/输出。前一原生提交的六组 CI 见 [d888587 CI 记录](../validation/monte-carlo-joint-ci-validation-20261012.json)。

相关矩阵含义、奇异容差、有限精度离散变换、仅有效样本的条件统计，以及没有输入-输出交叉块或自动置信区间等边界不因适配层而改变。SystemVue 容差/优化行为对照仍未完成，A09 不据此整体关闭。

本跨语言阶段的 Windows/Linux Debug/Release 各完成 clean-first 构建并通过 146/146 CTest、2/2 安装消费端、462/462 安装 wheel 回归。四组 v2 电阻任务与原生参数流逐点相同，功率跨语言误差不超过 5.56e-17、独立电路误差不超过 2.78e-16，完整 JSON 重放逐字节一致。72 个既有 C 结构声明保持不变；Windows 直接运行先前二进制，Linux 因原临时文件已不存在，使用先前提交源码与保留旧 SDK 重建消费端并分别加载旧、新库验证，报告明确区分这两类证据。当前提交远端 CI 随推送单独核查。
