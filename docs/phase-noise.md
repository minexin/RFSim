# 小角度相噪源与相关边带

RFModel 新增将单边带相噪采样映射为固定频率通道 C/P 统计量的接口，并接入转换网络 JSON。上下边带由同一个实相位扰动产生，保留互补相关；网络中的滤波、反射反馈和 A/B 混频按既有噪声求解器传播这些统计量。

## 物理定义

载波复功率波为 a，单位 sqrt(W)。小角度近似使用 a exp(j phi) ≈ a + j a phi，其中 phi(t) 为零均值实平稳相位过程。每个正频偏的单边带密度 L 以 dBc/Hz 给出，线性密度 ell=10^(L/10)。用正频偏的 proper 随机系数 u 表示相位过程，E[|u|²]=ell、E[u²]=0，则：

~~~text
z_upper = j a u
z_lower = j a conj(u)
C_upper,upper = C_lower,lower = |a|² ell       [W/Hz]
P_upper,lower = P_lower,upper = -a² ell       [W/Hz]
C_upper,lower = P_upper,upper = P_lower,lower = 0
~~~

不同频偏相互独立。相位方差在已描述的偏移频带内为 2 integral(ell df)，不是单边带积分本身；用户必须确认采用的小角度近似有效。接口不减小确定性载波，也不模拟大角度调制导致的载波衰减和高阶边带。

互补矩阵 P 对相位敏感处理不可省略。例如同频理想实混频器 y=2 x cos(LO*t+theta) 的正差频噪声为 4 |a|² ell sin²(theta-arg(a))。同相时一阶 PM 相消，正交时检出；将边带当作独立噪声会错误地得到 2 |a|² ell，与检测相位无关。该关系由一阶模型解析推导，回归还用四相位随机系数集合独立重建 C/P。

## C++、C 与 Python

C++17 头文件 rfmodel/phase_noise.hpp 提供 PhaseNoiseOffset 和 phase_noise_sidebands(channels, carrier_channel, carrier_wave, offsets)，返回 ConversionNoise。carrier_channel 是 channels 中的索引；offset_bin 是相对载波的正整数频偏。

~~~cpp
auto noise = rfmodel::phase_noise_sidebands(
    {{0, 9}, {0, 10}, {0, 11}}, 1, {1., 2.}, {{1, -100.}});
~~~

本例载波功率 5 W，两个边带各为 5e-10 W/Hz，互补相关为 (3-4j)e-10 W/Hz。通道顺序可以任意；匹配使用物理端口和 bin 身份。

C ABI 为 rfmodel_phase_noise_sidebands，输入通道端口/bin 数组、载波索引/复波及频偏/bin/密度数组，输出两个 N×N 复数矩阵。输出容量按复数元素计，两输出必须与彼此和所有输入数组不重叠；失败保持两输出不变。安装后的 C/C++ consumer 均调用这个新增入口。

~~~python
c, p = library.phase_noise_sidebands(
    [(0, 9), (0, 10), (0, 11)],
    carrier_channel=1, carrier_wave=1+2j, offsets=[(1, -100)],
)
# 将 c/p 传给 conversion_network 或 frequency_conversion 的 source_covariance /
# source_complementary。它们是密度，不乘 bin 间距。
~~~

通道数 1..512、频偏数 1..255；通道身份必须唯一，载波必须是有限非零复波并处于正频率。频偏严格递增，两个边带必须在同一物理端口显式存在且都严格大于 DC。缺失边带、跨 DC、重复频偏、非有限密度和整数范围溢出均报错，不自动截断。这里的 dBc/Hz 必须有限，不能用 -inf 表示零；没有相噪时不调用该源。

密度用对数域计算以避免载波功率的中间溢出。最终绝对密度必须能以有限正规 double 表示；次正规密度会使相位相关项的相对精度失真，因此明确拒绝。极小载波采用缩放归一化，避免将幅度舍入误差引入单位相位。

## 转换网络 JSON

在 rfmodel.conversion-network v1 文档上增加 phase_noise_sources：

~~~json
"phase_noise_sources": [{
  "name": "rf_oscillator",
  "carrier_channel": ["detector", 0, 10],
  "offsets": [
    {"offset_bin": 1, "ssb_dbc_per_hz": -100},
    {"offset_bin": 2, "ssb_dbc_per_hz": -110}
  ]
}]
~~~

载波复波直接取对应外部边界的 source。不能指定已连接的内部端口、零驱动边界或同一载波的重复源；名称必须唯一。边带源统计与已有边界热噪声或设备 source_noise 独立相加。多个声明源之间独立，即使它们的边带落在相同通道，噪声也按统计量相加。共享参考时钟的相关源不能使用这个独立源列表来代替。

结果保留 phase_noise_sources、carrier_wave、model=small_angle_paired_sidebands 与 correlation=independent_source，便于识别使用的近似。所有声明最终进入总 C/P；当前不额外输出独立的相噪来源分解。reference_noise_analysis 是另一个参考温度实验，会替换外部源统计，不把本次实际相噪自动计入参考 NF。

[相位检波示例](../examples/phase-noise-detector.json) 使用 10 MHz 载波、1/2 MHz 偏移和正交 LO，保留全部差频、上边带与 DC 通道。1..2 MHz 检波输出噪声按 W/Hz 线性插值积分，预期为 2.2e-4 W。同一示例将 LO 相位改为零，可观察一阶 PM 在差频端相消。

~~~powershell
python -m rfmodel examples/phase-noise-detector.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/phase-noise-detector-result.json
~~~

## 采样、SystemVue 对应与未完成项

该接口定义有限通道上的相噪样本，不自动在未指定频偏插值、外推或扩展网络通道。未由相噪源填充的矩阵项为零，只表示这个有限模型未加入相应噪声。尤其载波自身没有零频偏随机相位分量。不能把跨载波、跨未采样频段的梯形积分当作物理相噪全带积分；通道测量应限制在有描述并经过采样收敛检查的偏移带。示例只积分已给出的 1..2 MHz 偏移段。

SystemVue 2023 本机 Behavioral Phase Noise 帮助确认偏移/dBc/Hz 列表、单侧扩展与线性器件传播，也说明 RF/LO 相噪及参考时钟的关联。其 PNCP 帮助规定通道内两侧相噪积分，并给出从最低偏移到零偏移的延伸处理。这些完整行为当前尚未复现；本阶段不宣称 PNCP 或厂商相噪模型已兼容。[本机帮助来源与散列](../validation/systemvue-2023-phase-noise-help-provenance.json) 可追溯定义。

相关边带物理依据可参见 [Correlation between upper and lower sidebands（IEEE，2000）](https://pubmed.ncbi.nlm.nih.gov/18238557/)。本文 C/P 表达式与检波关系是根据上述明确的一阶模型推导的实现契约，并非从厂商数值拟合得到。

后续仍需：连续频偏曲线/插值规则、近载波行为、自适应采样、PLL 频率相关传递函数、LO 相噪线性化与互易混频、相噪来源分解和 PNCP/完整路径映射、非线性相噪转换及 SystemVue 实测。固定泵 LO 仍然理想无噪声。

## 工程验证记录

2026-10-09：Windows MSVC Debug/Release clean-first 构建成功，CTest 各 103/103 通过；两种配置安装后的独立 C/C++ consumer 各 2/2 通过。134 个 C/C++ 文件格式检查与 git diff --check 通过。

独立 Python 3.12 直接从本阶段 wheel 加载并连接安装后的 Release DLL：相噪 13、通道测量 11、加载噪声 9、变频 NF 12、转换网络 13、单器件变频 11、Python API 85、相干系统 51、线性噪声 9 项测试全部通过。相噪回归包括独立四相位集合重建 C/P、载波相位旋转、通道重排、复反射闭式解、同相 PM 相消与正交检测、独立源重叠、动态范围、输入拒绝、CLI 失败保护；C 测试覆盖输出原子性、容量及缓冲区别名。

以上属于解析和工程回归，SystemVue 相噪实测与完整兼容性验收尚未完成。

[共享参考相噪](shared-phase-noise.md)已增加显式多载波相位增益和跨器件相关统计；本文 phase_noise_sources 独立列表的含义不变。
