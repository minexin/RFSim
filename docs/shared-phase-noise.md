# 跨器件相关源噪声与共享参考相噪

本阶段允许转换网络的外部源在不同器件之间具有完整 C/P 相关统计，并增加一个实参考相位过程同时驱动多个载波的生成器。网络内的线路、滤波和混频继续使用现有 A/B 求解；原来的独立器件噪声接口与 C ABI 保持兼容。

## 全局源噪声

原设备 source_noise 保持按器件独立的块对角结构。新增 additional_source_noise 是与这些设备噪声独立的另一组随机过程；其内部允许跨器件、跨频率相关。全局顺序为设备顺序，随后是各设备局部通道顺序，和求解结果完全相同。

~~~text
C_source = blockdiag(C_device0, C_device1, ...) + C_additional
P_source = blockdiag(P_device0, P_device1, ...) + P_additional
~~~

两组统计分别校验维度、有限性、Hermitian/symmetric、实正交分量表示下的半正定性和 DC 实值约束，再相加。任何已连接端口的源噪声行/列必须严格为零；不能靠两组统计相消来隐藏内部端口的非法边界驱动。输出继续提供出射 C/P，以及可选的入射、自/交叉统计和净噪声流。

C++：FrequencyConversionNetwork::analyze(bool loaded_noise=false, const ConversionNoise *additional_source_noise=nullptr)。指针只在调用期间读取，不修改网络或输入。FrequencyConversionModel::validate_noise 可独立检查一组统计。未提供附加过程时保留原求解路径。

C：新增 rfmodel_conversion_source_noise 描述 count、covariance、complementary，以及 rfmodel_conversion_network_analyze_correlated。C 必需，P 空指针表示零；count 必须等于网络总通道数。普通输出必需，loaded 输出描述可空。所有输出互不重叠，并与全部输入数组及描述结构不重叠，包括新增全局源描述。失败时所有结果保持原值。

Python：Library.conversion_network 增加 additional_source_covariance 和 additional_source_complementary；P 不能脱离 C 单独指定。转换网络 JSON 可增加 additional_source_noise，其结构与已有噪声配置一致，例如 {"covariance": [...], "complementary": [...]}。显式全局统计与下述 phase_noise_groups 二选一；两者均可与设备噪声和独立 phase_noise_sources 并用。

reference_noise_analysis 仍是单独的参考温度实验，会替换原外部源噪声；这个附加过程不自动进入参考 NF 的定义。它也不提供源与器件内生噪声的交叉项。

## 共享参考相位过程

PhaseNoiseCarrier 保存全局通道索引 channel、载波功率波 wave 和实数 phase_gain。每个成员的相位扰动满足 phi_member = phase_gain * phi_reference。该增益可正、可负，也可为零；例如理想倍频比为 m 时可以明确填写 m，并得到 +20 log10(|m|) dB 的相噪密度变化。这里不从载波频率或名称自动猜测关联。

phase_noise_group(channels, carriers, offsets) 生成该共享过程的 C/P。偏移 L 指参考相位过程的 SSB 密度 dBc/Hz；单个成员的边带密度为 |wave|² phase_gain² 10^(L/10)。phase_noise_sidebands 现在复用单成员、增益 1 的同一实现。

每个正偏移对应一个 proper 随机变量 u，不同偏移独立。将所有上/下边带系数分别累加到向量 U/V，令 E[|u|²]=1：

~~~text
z = U*u + V*conj(u)
C = U*U^H + V*V^H
P = U*V^T + V*U^T
~~~

U/V 已包含 sqrt(10^(L/10)) 和 j*wave*phase_gain。相同输出通道上的边带先叠加系数，再形成统计量。例如某个载波的上边带和另一个载波的下边带重叠，会出现非零的 P 对角项。不能用独立功率相加代替。

C ABI 新增 rfmodel_phase_noise_carrier 与 rfmodel_phase_noise_group；输出保护也覆盖载波描述数组。Python 接口：

~~~python
c, p = library.phase_noise_group(
    [(0, 9), (0, 10), (0, 11), (1, 9), (1, 10), (1, 11)],
    [(1, 1+0j, 1), (4, 1+0j, 2)],  # index, carrier wave, phase gain
    offsets=[(1, -100)],
)
~~~

通道数≤512；成员必须非空、索引唯一且有非零有限载波；偏移数为 1..255。沿用单载波的正频偏、有序、成对显式边带、禁止跨 DC 和密度可表示性约束。增益为零仍需合法载波与频点声明。多个组代表相互独立的参考过程；一个载波可在不同组中出现，用于叠加独立相位扰动。

## JSON 和相消示例

~~~json
"phase_noise_groups": [{
  "name": "reference_clock",
  "carriers": [
    {"carrier_channel": ["left", 0, 10], "phase_gain": 1},
    {"carrier_channel": ["right", 0, 10], "phase_gain": 1}
  ],
  "offsets": [{"offset_bin": 1, "ssb_dbc_per_hz": -100}]
}]
~~~

成员载波复波来自对应外部边界的 source，禁止使用连接端口；两成员可以位于不同器件。组名唯一，未知字段和布尔数值均拒绝。输出保留组、成员载波波幅和 correlation=shared_reference_phase；旧 phase_noise_sources 列表仍表示独立相噪，可用于额外残余相噪。

[共享相噪示例](../examples/shared-phase-noise.json) 包含两条独立通过支路与差分合路节点，输出为 (left-right)/sqrt(2)。两路载波都为 1 sqrt(W)，参考偏移密度 ell=1e-10/Hz，则：

- 同一参考、两路 phase_gain=1：确定性载波及一阶共同相噪都相消。
- 同一参考、两路增益为 1 和 g：输出边带密度为 0.5*(1-g)²*ell。
- 改为两个独立参考组、两路增益均为 1：输出边带密度为 ell。
- 共同相噪相消后，再给一条支路加入独立残余相噪，其贡献仍会保留。

这不是 LO 相噪相消的验收案例：示例使用线性差分合路。固定泵混频器的 LO 尚未纳入扰动输入。上述解析关系与跨器件完整矩阵统计均有回归测试。

## 厂商目标与边界

SystemVue 2023 本机 Behavioral Phase Noise 帮助说明，相噪关联取决于参考时钟，可不同于载波相干关系；文中的时钟例子使用频率比缩放及混频和/差处理。[本机来源](../validation/systemvue-2023-phase-noise-help-provenance.json) 保留该帮助页散列。本阶段完成显式恒定实相位增益和跨器件相关源基础，尚未校准厂商 RefClk 参数或复现其完整参考时钟传播。

剩余工作包括 LO 扰动线性化、参考时钟到载波的频率相关 PLL 传递函数、残余相噪模型、相噪来源分解、PNCP 近载波与路径规则及 SystemVue 实测。连续曲线、采样收敛和小角度有效范围继续遵守[相噪源说明](phase-noise.md)，不能把有限样本当作完整相噪全带。

## 工程验证记录

2026-10-09，Windows MSVC Debug/Release clean-first 构建及 CTest 各 105/105 通过；两种配置安装后的 C/C++ consumer 各 2/2 通过。135 个 C/C++ 文件格式检查和 git diff --check 通过。

独立 Python 3.12 使用本阶段 wheel 和安装后的 Release DLL：共享相噪 13、原相噪 13、通道测量 11、加载噪声 9、变频 NF 12、转换网络 13、单器件变频 11、Python API 85、相干系统 51、线性噪声 9 项测试全部通过。

验证覆盖独立随机系数集合重建完整 C/P、重叠边带 P 对角项、正/负/零相位增益、跨器件复杂反射与入射/出射交叉闭式解、共享相噪相消和独立残余保留、DC/PSD/连接端口拒绝、C 输出原子性与别名保护、旧接口回归和 CLI 失败保护。以上是解析及工程证据，尚不是 SystemVue RefClk 或 LO 相噪兼容性验收。
