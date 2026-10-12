# 原生 Monte Carlo 容差分析

`include/rfmodel/monte_carlo.hpp` 提供 C++17 `run_monte_carlo(parameters, metric_count, evaluate, options, cancelled)`。每次完整参数抽样调用一个返回有限实向量的同步求值函数；调用者可据此重建 RF 网络、噪声或器件模型。分布与选项在入口复制，回调修改原请求不影响本次运行。

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

不同参数使用独立边缘分布。固定值、相等上下界、零标准差不消耗随机字；常量 LogNormal 的结果必须为可表示的有限正数。

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

验证入口为 `monte_carlo_test`、`monte_carlo_resistor` 两个 CTest 用例及安装后的消费端测试。可用 `ctest --test-dir build -C Release -R monte_carlo --output-on-failure` 重跑新增用例；`rfmodel_tolerance_analysis` 的标准输出即完整 JSON 轨迹。示例源码为 [monte_carlo_resistor.cpp](../examples/monte_carlo_resistor.cpp)。

[机读验证报告](../validation/monte-carlo-native-validation-20261012.json)记录四个本地配置的核心、安装、同环境重放、独立 Python 统计核对和源文件指纹；[完整电阻容差轨迹](../validation/monte-carlo-resistor-example-20261012.json)保留全部 4096 个样本。原生阶段 C ABI 与 Python 源码未改动，本地完整 CTest 包含原有绑定测试，没有另行构建安装新 wheel；干净检出的三平台 wheel 回归随阶段推送执行。报告中发布状态保留生成时刻的含义。

## C ABI 与 Python 同步回调

`rfmodel_run_monte_carlo` 现提供与原生 C++ 相同的采样/统计语义，不改变已有 ABI 布局。新结构体、枚举与函数声明见 [c_api.h](../include/rfmodel/c_api.h)。结果是独立、只读的 `rfmodel_monte_carlo_result`，由 `rfmodel_monte_carlo_destroy` 释放；输入和 options 指向的分位数/判据数组在首次回调前复制。

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
