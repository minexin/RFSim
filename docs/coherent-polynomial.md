# 带本地来源的九阶相干 RF 多项式

## 能力与接口

`CoherentPolynomial` 为确定性 RF 分量提供 1..9 阶实电压多项式展开。
与原有仅由 source 生成失真的限幅放大器不同，本接口让输入的 source、
harmonic、intermod 都参与生成。它为后续高阶模型和失真再混频提供数值
原语；没有自动套用 RFAMP 的 P1dB、饱和或高阶截点标定。

- C++：`CoherentPolynomial(coefficients, reference_ohms).evaluate(spacing_hz, inputs, reserved_group_max)`。
- C：`rfmodel_coherent_polynomial_evaluate`。
- Python：`Library.coherent_polynomial(spacing_hz, components, coefficients, reference_ohms=50., reserved_group_max=0)`。

coefficients 为实电压系数 a[0]..a[n]，满足 y(t)=Σa[n]v(t)^n，最多 10 个，
a[0] 必须为零。输入输出幅度为 sqrt(W) 的 RMS 功率波，共同参考阻抗为正实数。
实电压傅里叶系数为正频率功率波乘 sqrt(R/2)；所以第 n 阶输出波的系数为
a[n]×(sqrt(R/2))^(n−1)，再乘相应多项式排列数和带共轭的输入波乘积。

输入先按既有相干键合并，返回 reduced inputs。每个输出项携带本地阶数
以及最多九个带符号的一基输入下标：负数表示共轭，重复表示幂次，
不做正负来源相消；C/C++ 数组剩余位置补零，Python 只返回有效下标。
输出按阶数、频率 bin、下标序列排序。

一次传递保留输入类型和相干组。新生成项各有独立组号，大于所有输入组号
和 reserved_group_max；相同正下标重复的生成项标为本地 harmonic，
其他生成项标为 intermod。谱宽记为参与分量谱宽之和。
输入中为零的相干组仍保留在 reduced inputs 中，不生成非零项。

这些是**本级输入来源**，不是递归展开后的原始源身份。例如，对前级 H2
再次求三次幂，本地阶数是 3，而原始源阶数可能为 6。调用方仍须展开来源、
应用全局阶数限制并确定跨级相干归并。新系统图 polynomial_amplifier 阶段
现通过[递归来源](recursive-mixing-origins.md)接入上述处理；后续
[混合图](mixed-polynomial-graphs.md)已接入多来源表达式。共同压缩组合及
SystemVue 高阶语义尚未验收。现有 cascaded_amplifier 的数值模型未改变。

## RF 投影与资源边界

展开时使用正负频率对称输入，最终只保留正 RF bin；DC 生成项被投影掉。
多级调用相当于每一级之间去掉 DC，不能代表带偏置反馈的全波形非线性电路。
同频但不同来源/阶数的项保持分离，不隐式求和，也不将压缩模型和多项式
载频项叠加。本接口不实施次级谱幅度阈值、噪声、AM/PM、反向反馈或带宽谱形积分。

上限为 4096 个原输入、64 个合并后非零输入、4096 个正 RF 输出项、
一千万次枚举递归访问。溢出或容量超限时整个计算失败，不截断返回部分谱。
C 输出数组和计数不得互相重叠；错误时保持所有输出不变。
ABI 为增量新增符号，使用新版 Python 包时需要配套新版共享库。

## 示例与验证

`examples/coherent-polynomial-cascade.py` 先生成 H2，再使其与载波共同参与
下一级二、三阶生成，返回两级的局部来源供调用者继续处理。

```powershell
$env:PYTHONPATH = "$PWD/python"
python examples/coherent-polynomial-cascade.py --library build-msvc/Release/rfmodel_c.dll
```

数值回归使用独立的稀疏傅里叶卷积实现验证 50 Ω / 75 Ω 下全部一至九阶
RF bin，包括复相位、共轭和排列数。另有两级级间去 DC 后的独立卷积对照，
以及相干输入合并、抵消、组号/频率溢出、阶数与项数边界检查。
C 回归验证容量不足不改变输出；C/C++ 安装消费和 Python 回归覆盖九阶符号。

## SystemVue 参考实验

[六组采集](../validation/systemvue-2023-secondary-controls-captures.json)
与[诊断报告](../validation/systemvue-2023-secondary-controls.json)记录本轮
双音各 −10 dBm、两级 10 dB、RISO=140 dB 的实验：

- 三阶：次级谱开关关/开，阈值 −140 dB；另检查开启时 +50、+140 dB。
- 五阶：次级谱开关关/开，阈值 −140 dB。
- 输出来源分别为三阶 30 条、五阶 94 条；同阶对照的原生复幅度未改变。

这不能证明次级再混频已由 SystemVue 验收，也不能由来源数推断其全部高阶
项展开规则。官方 Intermod Sub-Spectrum 帮助说明显示标签可能只代表主导
子谱，而幅度包含多个子谱，因此仍需核实高阶系数和合并规则。
三阶比较器现明确拒绝 maximum_order=5，防止按三阶实现验收五阶数据。

```powershell
python scripts/reference/audit-cascade-secondary-controls.py validation/systemvue-2023-secondary-controls-captures.json build-reference/secondary-controls-replay.json
```

该工具只审计开关对照，报告固定为 diagnostic_only，不能充当九阶模型的
SystemVue 兼容性报告。当前九阶模型的证据是解析计算及独立数值算法验证。

## 本阶段工程验证

- MSVC Debug / Release 清理重建后，完整 CTest 各 73/73。
- 安装后独立 C/C++ 消费项目，两种配置各 2/2。
- 独立 Python 3.12 wheel：64 项 API、18 项系统图回归通过，示例运行成功。
- 后台参考运行器 17/17，C/C++ 格式检查 104 个文件通过。
- 已确认上一阶段提交 310f422 的 Windows、Ubuntu、macOS 六项 CI 均通过。
