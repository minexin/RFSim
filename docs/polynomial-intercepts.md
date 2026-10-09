# 显式高阶截点到多项式系数

## 能力与定义

新增 C++、C 和 Python 接口，将调用者明确给出的二至九阶双音截点转换为
实电压多项式 `y(t)=Σ a[n]v(t)^n` 的系数。结果可直接用于现有
`CoherentPolynomial`、`MatchedPolynomialAmplifier` 及系统图的
`polynomial_amplifier.voltage_coefficients`。

每条截点定义包含以下字段：

| 字段 | 含义 |
| --- | --- |
| first_tone_order / second_tone_order | 有符号的 k1、k2，参考互调项为 k1 f1 + k2 f2 |
| intercept_dbm | 外推截点，按每个输入音的功率定义；必须明确输入或输出参考面 |
| coefficient_sign | 实多项式系数的 +1 或 −1 符号，不能从截点功率推断 |
| reference | INPUT / OUTPUT；默认 INPUT |

两个 k 都必须非零，`n=|k1|+|k2|` 为 2..9，每个 n 最多定义一次。
同阶不同互调项具有不同排列数，因此只写“IP4=某值”不足以确定系数。
例如三倍第一音减第二音的排列数为 4，两倍第一音减两倍第二音的排列数为 6。
调用者须选择和测量定义相符的参考项。负号表示相应音的共轭因子，不表示系数负号。

截点是小信号直线的外推交点，不是要求在实际截点功率下整个放大器输出仍线性。
这里标定的是一个齐次阶次、一个指定互调项的幅度；不包含其他阶次重叠项、
压缩、饱和、输入限幅、AM/PM 或噪声。输入两音应为不同频率。

## 换算

令小信号幅度增益 `g=10^(GdB/20)`，参考阻抗 R 为共同正实数，
参考项排列数 `M=n!/(|k1|! |k2|!)`。输入截点每音功率为 P（W），
则功率波下参考项的幅度为 `M a[n] (sqrt(R/2))^(n−1) P^(n/2)`。
与外推载波幅度 `g sqrt(P)` 相等时：

`a[n] = sign × g / (M × (sqrt(R P / 2))^(n−1))`

输出参考截点先按 `IIPn_dBm=OIPn_dBm−GdB` 换算。实现使用对数计算，
避免 R×P 或高次幂的中间溢出；最终增益或非零系数超出 double 可表示范围
时返回错误，不把下溢悄悄当成零系数。

结果的常数项为 0、一次项为 g，未指定的非线性阶次为 0；长度为最高阶+1，
空截点表返回两个系数。输入顺序不影响结果，重复阶次即使数值相同也拒绝。

## 接口与示例

C++：`rfmodel::polynomial_coefficients_from_intercepts`，头文件
`rfmodel/polynomial_intercepts.hpp`，接收 `std::vector<TwoToneIntercept>`。
C：`rfmodel_polynomial_coefficients_from_intercepts`，最多八条定义，输出至多十个
系数。缓冲区容量不足、别名重叠或参数错误时，系数数组和计数都保持不变。
Python：`Library.polynomial_coefficients_from_intercepts` 返回系数元组；
在 Python 内直接构造 JSON 系统图时，使用 `list(coefficients)` 作为数组。
`TwoToneIntercept` 和 `InterceptReference` 为公开类型。

```python
from rfmodel import InterceptReference, TwoToneIntercept

coefficients = library.polynomial_coefficients_from_intercepts(
    10.0,
    [TwoToneIntercept(3, -1, 33.0, 1, InterceptReference.OUTPUT)],
    reference_ohms=50.0,
)
# a4 ≈ 0.07096267784671509；a2、a3 未指定，返回 0。
response = library.coherent_polynomial(1e8, sources, coefficients)
```

[完整示例](../examples/highorder-intercepts.py)由二、三、四阶输出截点生成系数，
构造现有 JSON 系统图并求解来源频谱：

```powershell
$env:PYTHONPATH = "$PWD/python"
python examples/highorder-intercepts.py --library build-msvc/Release/rfmodel_c.dll
```

这是新增 ABI 符号，更新后的 Python 包需要配套新版共享库。
现有二、三阶工厂与其符号约定未改动。

## 验证与 SystemVue 边界

原生测试以独立的实波形采样和傅里叶投影检查二至九阶所选互调项在截点处
的幅度与相位，覆盖 50 Ω / 75 Ω；另对照原有二、三阶公式，测试参考面换算、
对数域极端单位、无效阶数、重复定义和不可表示结果。C/Python 测试验证绑定、
整数不被 ctypes 截断，以及错误时输出保持不变；安装消费者直接使用新增符号。

已有 [SystemVue 四、五阶诊断](systemvue-highorder-diagnostic.md)已改用此原生 API
生成四阶系数。公开 `OIP4=OP1dB+13 dB` 在该固定工况下得到 33 dBm；
`3f1−f2` 的参考定义来自此前实测。另通过[削减开关实验](systemvue-spectrum-reduction.md)
的 86 个四阶已记录项检查整个转换与展开链路。

这不等于已经复现 RFAMP 从 OP1dB、OPSAT、OIP3 自动生成五、七、九、十一阶
系数的专有算法。当前五阶实测诊断仍使用独立单音识别值，七至九阶的本接口
证据为解析关系及独立数值验证。更广泛 RFAMP 参数化、十一阶、总谱与多级
高阶兼容仍属长期目标中的未完成项。

## 本阶段工程验证

- MSVC Debug / Release 均清理重建，完整 CTest 各 79/79。
- 安装后独立 C/C++ 消费项目，两种配置各 2/2。
- 独立 Python 3.12 wheel：72 项 API、45 项系统图回归及新示例通过。
- C/C++ 格式检查 111 个文件通过。
- [通过新截点 API 重放的 SystemVue 报告](../validation/systemvue-2023-intercept-api-diagnostic.json)
  记录输入文件与本机 Release 动态库哈希；86 个四阶来源项的最大相对误差约
  5.01e-8，保留既有 1e-7 容差。五阶仍使用独立单音识别值，报告为诊断分类。

重放命令：

```powershell
python scripts/reference/diagnose-spectrum-reduction.py build-msvc/Release/rfmodel_c.dll validation/systemvue-2023-highorder-captures.json validation/systemvue-2023-spectrum-reduction-captures.json build-reference/intercept-api-replay.json
```
