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

本次 wheel 需要包含上述新导出符号的配套原生库；仅替换 Python 包不能使旧 SDK 获得新能力。Monte Carlo 的声明式任务与参数绑定、相关输入/协方差及厂商对照仍待完成，A09 整项继续保持开放。


## C/Python 阶段验证

[跨语言机读报告](../validation/monte-carlo-api-validation-20261012.json)分别记录 Windows/Linux Debug/Release 的全量 CTest、C/C++ 安装消费端、安装 wheel 与源文件指纹。新增纯 C 回归覆盖输出失败原子性和所有状态，17 项 Python 测试覆盖回调、句柄释放、统计边界与实际 RF 网络；这些测试通过安装 wheel 验证工具自动纳入三平台 CI。

[安装后 Python 完整轨迹](../validation/monte-carlo-python-resistor-example-20261012.json)保存 4096 个样本及全部输入选项。每个配置均与本配置的 C++ 示例逐点比较参数和指标，并独立计算电路与统计参考。原生随机流/统计实现的源码指纹保持不变，前一阶段远程验证见 [8fc3710 的 CI 记录](../validation/monte-carlo-native-ci-validation-20261012.json)。

四个本地配置实际结果为：各 141/141 核心测试、2/2 安装消费端、427/427 安装 wheel 回归。完整 C++/Python 参数轨迹相同，最大指标差 5.56e-17，独立电路误差不超过 2.78e-16。Linux 首次 wheel 安装因系统 Python 缺少 pip 停止，随后使用工作区既有环境和独立安装目录完成全部验证；报告保留首次失败和成功重跑的证据。
