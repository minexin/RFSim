# 理想无源器件的 C/Python/JSON 接口

已有 C++ 核心的理想 R/L/C、匹配衰减/延迟、隔离功分器与正交耦合器现在可通过 C ABI、Python 和线性网络 JSON 使用。参数化器件也可用于已有相干网络与系统图的 network 节点，频率由所在扫描或频谱 bin 决定。

这些接口复用 IdealRLCModel、MatchedTransmissionModel、EqualPowerDividerModel、IsolatedPowerDividerModel、QuadratureCouplerModel。它们是明确定义的理想原语，不等同于 SystemVue 某一同名器件的全部寄生、频率特性、噪声或非线性行为；不据此更新官方目录条目的兼容评定。

## JSON 模型

所有模型放在 devices 的 model 字段。参考阻抗继承网络顶层公共正实 reference_ohms，默认 50 Ω。未知字段直接拒绝。

| type | 必需字段 | 可选字段及默认值 | 端口 |
|---|---|---|---|
| resistor | connection、resistance_ohms | 无 | 2 |
| inductor | connection、inductance_h | 无 | 2 |
| capacitor | connection、capacitance_f | 无 | 2 |
| matched_transmission | 无 | loss_db=0、delay_s=0 | 2 |
| equal_power_divider | branches | excess_loss_db=0 | branches+1 |
| isolated_power_divider | branch_transmissions | 无 | 数组长度+1 |
| quadrature_coupler | coupled_power_fraction | excess_loss_db=0 | 4 |

connection 必须为 series 或 shunt，分别表示串联阻抗与并联导纳。R/L/C 元件值必须有限且严格为正，单位明确为 Ω/H/F。频率允许 0 Hz：串联电容为开路、并联电感为短路，串联电感和并联电容为直通；按极限直接返回有限 S 参数。

loss_db/excess_loss_db 和 delay_s 必须有限且非负，单位 dB/s。matched_transmission 是互易匹配二端口，S21=S12=10^(-loss_db/20) exp(-j 2π f delay_s)，S11=S22=0。

功分器端口 0 为公共端，端口 1..N 为匹配且相互隔离的分支。branches 必须为 2..64 的整数，不接受布尔值。等功率分支幅度是 10^(-excess_loss_db/20)/sqrt(N)。

branch_transmissions 是 2..64 个复数 S 幅度，复数用 [实部, 虚部]；模平方和不得超过 1。反向传输使用同一个复幅度以满足互易性，不自动取共轭。

正交耦合器的 coupled_power_fraction 为 [0,1] 内的功率比例。激励端口 0 时，端口 1 为实数直通、端口 2 为 +j 耦合、端口 3 隔离；其他端口的互易关系由四端口矩阵确定。此模型不包含有限方向性或端口失配。

## 热噪声与网络使用

器件不隐式添加热噪声。可沿用统一 temperature_k 或逐器件 noise 配置，由核心按 kT(I-SS†) 生成内生协方差。功分器即使 excess_loss_db=0，其隔离网络仍耗散差模输入，因此分支相关噪声不能直接设为零。理想无损电感、电容和无损正交耦合器的内生热噪声为零。

examples/passive-rc-network.json 连接 50 Ω 串联电阻、100 Ω 并联电阻与并联电容，包含 DC 和两个射频频点、内生噪声及二端口噪声参数分析。

~~~powershell
python -m rfmodel examples/passive-rc-network.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/passive-rc-result.json
~~~

设并联总导纳 Y=1/100+jωC，公共参考为 50 Ω，该网络 S21=2/(3+100Y)，S11=1/(3+100Y)。回归用此独立 ABCD 结果验证连接及频率计算，并检查全网热噪声关系。

仅有串联电阻时，NFmin 的最优源位于无限阻抗极限；当前噪声参数接口会报告单位圆边界错误。此情况已保留为拒绝回归，未用有限数值冒充极限最优值。取消 noise_analysis 仍能输出其 S 与内生协方差。

## C 与 Python

C API 新增五个 rfmodel_*_s 函数：

- rfmodel_ideal_rlc_s：element 枚举选择 R/L/C，connection 枚举选择串联/并联，value 为相应 SI 单位。
- rfmodel_matched_transmission_s
- rfmodel_equal_power_divider_s
- rfmodel_isolated_power_divider_s
- rfmodel_quadrature_coupler_s

输出为 N² 个按行排列的复数，capacity 单位是复数元素个数。非法参数或容量不足时不修改输出。带分支数组的接口禁止输出与输入重叠。

Python Library 对应 ideal_rlc、matched_transmission、equal_power_divider、isolated_power_divider、quadrature_coupler。ideal_rlc 的 element 使用 resistor/inductor/capacitor 字符串，connection 使用 series/shunt，其余字段与 C API 一致。

## 验证范围

新增文件专项回归涵盖 R/L/C 解析公式与 DC 极限、损耗/延迟相位、功分器分支噪声相关、复分支互易性、64 分支上限、正交耦合器功率与隔离、RC 网络、相干网络复用、严格输入校验和 CLI 失败不覆盖结果。C ABI 另覆盖新函数导出、错误输出原子性与输入/输出重叠拒绝。

本阶段没有新增 SystemVue 实测证据。实际 RF Design 器件的参数映射、非理想特性和数值对照仍按目录逐项推进。

## 工程验证记录（2026-10-09）

MSVC 2022 x64 Debug/Release clean-first 构建及 CTest 各 88/88。独立安装消费测试 Debug/Release 各 2/2，其中 C 消费程序调用五个新增导出符号。Python 3.12 从独立 wheel 加载，无源专项 12/12、API 85/85、系统图 51/51、线性噪声文件 9/9；带阻性负载的 RC CLI 样例成功输出三个频点并通过 DC 解析值检查。C++ 格式检查 120 文件通过。跨平台 CI 由本提交触发，结论以 GitHub Actions 为准。
