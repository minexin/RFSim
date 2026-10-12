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

本阶段范围是原生 C++ 接口、安装消费端和解析 RF 示例。Monte Carlo 的 C ABI、Python、声明式任务与参数绑定、相关输入/协方差、厂商随机算法及 SystemVue 容差分析对照仍未完成，A09 保持开放。

## 本阶段验证

验证入口为 `monte_carlo_test`、`monte_carlo_resistor` 两个 CTest 用例及安装后的消费端测试。可用 `ctest --test-dir build -C Release -R monte_carlo --output-on-failure` 重跑新增用例；`rfmodel_tolerance_analysis` 的标准输出即完整 JSON 轨迹。示例源码为 [monte_carlo_resistor.cpp](../examples/monte_carlo_resistor.cpp)。

[机读验证报告](../validation/monte-carlo-native-validation-20261012.json)记录四个本地配置的核心、安装、同环境重放、独立 Python 统计核对和源文件指纹；[完整电阻容差轨迹](../validation/monte-carlo-resistor-example-20261012.json)保留全部 4096 个样本。本阶段 C ABI 与 Python 源码未改动，本地完整 CTest 包含原有绑定测试，没有另行构建安装新 wheel；干净检出的三平台 wheel 回归随阶段推送执行。报告中发布状态保留生成时刻的含义。
