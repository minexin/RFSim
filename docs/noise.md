# 噪声接口与验证范围

`NoiseProvider` 提供频点对应的标量噪声参数。`NoiseParameters` 支持非负有限噪声系数、正有限参考温度和等效温度换算，并拒绝溢出。当前接口约束不覆盖负噪声系数的特殊模型。

`cascade_noise_figure_db` 适用于使用一致噪声参考条件、以相应线性功率增益描述的单向级联。输入增益和噪声系数单位为 dB，内部转换为线性值计算 Friis 公式；它不是含反射、相关噪声或变频的通用网络噪声求解器。

2026-09-22：MSVC Debug/Release 全套 CTest 各 8/8 通过，包含噪声测试。解析基线：F1=2、F2=3、G1=10 得到总 F=2.2；10 dB 常温衰减器前置于 F=2 放大器得到总 F=20；F=2、T0=290 K 对应 Te=290 K。

测试还包含空链、零噪声系数、NaN、负噪声系数、无效温度和极端溢出。此前仅执行测量测试不能证明噪声模块通过；本次首次完成 noise_test 的双配置验证。

Fmin/Rn/GammaOpt、网络反馈噪声求解和变频噪声仍未实现；SystemVue 2023 差异测试仍待执行。相关矩阵基础接口见下文。

## 相关噪声矩阵基础接口

`noise_matrix.hpp` 定义 `NoiseCorrelation`，其 `watts_per_hz` 存储 Cij=E[ci*conj(cj)]，单位 W/Hz，使用功率归一化波及正实数参考阻抗。它与现有标量 NoiseProvider 并存；还没有把两者相互转换。

`passive_thermal_noise(S,T)` 在经典热平衡近似下计算 kB*T*(I-S*S†)，kB=1.380649e-23 J/K。T 是器件均匀物理温度，可以为零；即使 T=0 也检查无源性。该公式的参考来源：[Caltech 噪声波分析报告](https://www.kiss.caltech.edu/papers/reports/oliver-king-final-report.pdf)。不含量子修正、外部源/负载噪声或器件间温差分布。

`propagate_noise(H,C)` 计算 H*C*H†，目前 H 是与 C 相同阶数的方阵。H 必须由调用方给出；本接口不会从网络连接自动求出 H，也未连接到 LinearAnalysisResult 的 CS 字段。

输入相关矩阵需有限、Hermitian、半正定。使用对角选主元的半正定 Cholesky 分解并重构，归一化门限为 256*n*epsilon。门限内的剩余块视为零，因此会丢弃该尺度以下的微小噪声成分。无源性检查以 I-S*S† 的元素最大幅度或 1（取较大者）作为尺度；普通相关矩阵以自身最大幅度归一化。显著非无源、负定或非 Hermitian 输入会被拒绝。上限 1024 端口，算法为稠密矩阵实现。

新增回归使用除以 kT 后的无量纲比较，避免 W/Hz 很小而让绝对误差检查失效。覆盖匹配衰减器、50 欧姆串联电阻的负相关项、无损电感、复数秩一噪声相位传播、交换端口及非法矩阵。MSVC Debug/Release 全套各 20/20 通过。完整网络 CS 和 SystemVue 对照仍未完成。