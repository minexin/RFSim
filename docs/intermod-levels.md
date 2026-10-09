# IMN 输出互调功率参数

高阶放大器现在可直接接收一组 `IM1..IMn` 输出功率，自动换算显式多项式系数。
转换采用本机 SystemVue 2023 `RFAMP_HO` 帮助页公开的公式及参考互调项，
当前支持 n=1..11。官方出处与页面哈希见
[来源记录](../validation/systemvue-2023-intermod-levels-provenance.json)。

这补齐了 RFAMP_HO 的一条参数输入路径，尚不是整个器件的数值兼容验收。
普通 RFAMP 根据 OP1dB/OPSAT/OIP3 自动生成高阶系数的算法仍未实现。
本阶段没有新增 SystemVue 实际运行数据。

## 功率、参考项与符号

数组第一个值 IM1 是标定时每个基波音的输出功率，后续值是指定齐次阶次的
互调输出功率，单位均为 dBm。两输入音功率相等。公式为：

`OIPn = IM1 + (IM1 - IMn) / (n - 1)`

换算输出截点后，沿用已有
[显式截点转换](polynomial-intercepts.md)生成电压域系数。
本接口固定使用以下参考项，不能替换为任意同阶组合：

| 阶数 n | 参考互调 |
| ---: | --- |
| 2 | f1 − f2 |
| 3 | 2f1 − f2 |
| 4 | 3f1 − f2 |
| 5 | 3f1 − 2f2 |
| 6 | 4f1 − 2f2 |
| 7 | 4f1 − 3f2 |
| 8 | 5f1 − 3f2 |
| 9 | 5f1 − 4f2 |
| 10 | 6f1 − 4f2 |
| 11 | 6f1 − 5f2 |

IMn 表示该阶指定产物的功率，不是所在频率所有阶次重叠后的总功率。
实际测量应在适当的小信号区分离产物；将总谱峰直接作为单阶 IMn 会引入偏差。
相同截点可以使用不同的 IM1 参考，例如文档的
`[-10, -80, -140]` 与 `[0, -60, -110]` 均对应 OIP2=60、OIP3=55 dBm。
把 IM1 改成 0 时必须同步换算其他项，不能简单地整体减去 IM1。

功率不能决定实电压系数的正负，更不能决定一般复数相位。
因此 `coefficient_signs` 必须显式提供 a2..an 的 n−1 个 +1/−1 整数。
示例采用的偶正奇负是调用者选择，未宣称是 SystemVue 对任意 IMN 的默认行为。

数组必须包含 IM1 到最高阶的全部项目，不能跳过中间阶。可以用很低的有限
功率表达极弱阶次，仍会保留其非零系数；例如 −1000 dBm 不是禁用开关。
非有限输入、不可表示的截点/非零系数、错误数量或符号会失败，不静默裁剪或下溢成零。
只提供 IM1 与空符号表会得到线性系数，此便利形式不表示 SystemVue 的
MaxOrder 枚举支持一阶 RFAMP_HO。12..20 阶暂不支持。

## 公开接口

C++ 头文件 `rfmodel/intermod_levels.hpp` 提供：

```cpp
auto coefficients = rfmodel::polynomial_coefficients_from_intermod_levels(
    10., {0., -40., -60.}, {1, -1}, 50.);
```

返回 a0..an，a0=0，a1 为小信号电压增益；把 `coefficients[2:]` 对应部分传给
`CoherentHighOrderAmplifier`，或将整个数组传给 `CoherentPolynomial`。
IM1 用于参数标定，不会变成运行时固定输入功率；放大器工作点仍取实际输入驱动。

C ABI 新增 `rfmodel_polynomial_coefficients_from_intermod_levels`，分别传入
输出功率数组和符号数组及数量，输出最多十二个系数。输入数组、输出数组和
计数必须互不重叠，接口主动检查；失败时输出及计数保持不变。
一个 IM1 值时符号指针可为 NULL，数量必须为 0。

Python 提供同名 `Library.polynomial_coefficients_from_intermod_levels`：

```python
coefficients = library.polynomial_coefficients_from_intermod_levels(
    10.0, [0.0, -40.0, -60.0], [1, -1], reference_ohms=50.0
)
```

新版 Python 包需要配套包含新符号的原生库。

## JSON 与运行示例

`highorder_amplifier` 支持两种互斥参数形式：

- `nonlinear_voltage_coefficients`：现有显式 a2..an 数组。
- `intermod_output_levels_dbm` 加 `coefficient_signs`：IM1..IMn 和显式符号。

其余增益、压缩、最大来源阶数、失真传播参数沿用
[高阶放大器契约](coherent-highorder-amplifier.md)。两个形式不能同时出现，
IMN 形式也不能省略符号表。三种语言和 JSON 共用同一个原生转换函数。

[十一阶完整示例](../examples/coherent-intermod-levels.json)可通过 CLI 执行：

```powershell
$env:PYTHONPATH = "$PWD/python"
python -m rfmodel examples/coherent-intermod-levels.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/intermod-levels-result.json
```

图层仍先求真实相干驱动，再施加共同压缩与限幅。IMN 参数不会绕过
64 个活跃非线性输入、4096 个原生 RF 输出项以及来源展开的资源限制。
多音和多级展开可能超过限制，此时明确失败。

## 验证范围

独立实波形傅里叶投影逐阶检验指定互调项，覆盖二至十一阶、50/75 Ω、
0/17 dB 增益、输入相位以及不同的显式系数符号。另验证官方等价 IMN 示例、
极弱非零阶次及非法参数。C 回归检查容量不足、符号数量和指针重叠的输出保护。

Python/JSON 回归比较 IMN 与显式系数两条完整执行路径，包括双音、混频两边带、
十一阶来源、线性退化以及资源超限拒绝。现有 SystemVue 归档重放继续执行，
但它不包含 RFAMP_HO 的 IMN 实际运行，因此不能作为本接口的新增厂商实测证据。

## 本阶段工程验收

- MSVC Debug / Release 均清理重建，完整 CTest 各 84/84。
- 两种配置安装后的独立 C/C++ 消费者各 2/2。
- 独立 Python 3.12 从新 wheel 导入，77 项 API、51 项系统图测试通过。
- IMN wheel CLI 示例生成 670 个非线性项，最高来源阶数为 11。
- C/C++ 格式检查 116 个文件通过。

跨平台 CI 需对本阶段提交另行核查；上述数值证据不扩大为完整 RFAMP_HO 兼容结论。
