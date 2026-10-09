# 理想无损 Chebyshev I 型滤波器

ChebyshevFilterModel 提供低通、高通、带通和带阻四类互易二端口的完整复 S 参数。对应 C ABI、Python 和 JSON 模型已接入现有网络求解流程。实现包含传输相位与反射，尚未完成 SystemVue 滤波器实测兼容验收。

## 参数

JSON type 为 chebyshev_lossless；response 为 lowpass/highpass/bandpass/bandstop，order 为 2..64 的低通原型阶数。带通和带阻的实际响应阶数为 2×order。低/高通使用 passband_hz；带通/带阻使用 lower_passband_hz、upper_passband_hz，要求有限、正数且下边缘小于上边缘。

ripple_db 默认 0.1 dB，必须严格为正。passband_attenuation_db 在 Python/JSON 中默认等于 ripple_db，且不能低于纹波值。此约束定义带边为单调阻带方向的衰减交点，避免在多个通带纹波交点中隐式选择。SystemVue 的 Apass<R 或 R=0 行为尚待查证，本阶段显式拒绝。

input_stopband 默认 open，另可选 short；切换时 S11/S22 反号，S21/S12 不变。网络 reference_ohms 为公共正实数，默认 50 Ω。BSF_CHEBY 官方参数表没有列出 TYPE，因此这里的带阻拓扑选项是 RFModel 明确的数学约定，不冒充厂商参数。

## 幅相与反射定义

设 ε²=10^(ripple_db/10)-1，T_N 为 N 阶 Chebyshev 多项式，归一化功率响应为：

~~~text
|S21|² = 1 / (1 + ε² T_N(Ω)²)
μ = asinh(1/ε) / N
p_k = -sinh(μ) sin(θ_k) + j cosh(μ) cos(θ_k)
θ_k = (2k+1)π/(2N), k=0..N-1
~~~

传输相位由稳定模拟极点 p_k 决定，DC 传输为正实数。频率变换沿用 [Butterworth 的四类映射](butterworth-filter.md)，再乘以 cosh(acosh(sqrt((10^(A/10)-1)/ε²))/N)，其中 A 为边缘衰减。默认 A=ripple_db 时缩放为 1。

open 拓扑满足 S11/S21 = j^N ε T_N(Ω)，S22=(-1)^(N+1) S11，S12=S21。short 将两个反射系数同时反号。这些定义保证互易性和无损功率守恒，并固定反射相位的选择。

奇数阶低通 DC（或带通中心）的传输功率为 1；偶数阶为 10^(-ripple_db/10)，差额是反射，不是耗散。该奇偶阶归一化与 [SciPy 官方 Chebyshev I 定义](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.cheby1.html) 一致。偶数阶可用不等终端的 LC 原型及理想变压器实现；不将其伪装成等终端、DC 必然直通的简单 LC 链。

无损热噪声按既有 kT(I-SS†) 计算，应为零（允许舍入残差）。实现不因通带纹波产生耗散噪声。对数域幅度、归一化极点因子和奇数多项式除以 Ω 的递推分别处理大动态范围、稳定相位和近 DC 消减误差。极端纹波使极点实部在双精度下消失时显式报错。

## 接口与示例

~~~python
s = library.chebyshev_filter(
    95e6,
    response="bandpass",
    order=4,
    lower_passband_hz=90e6,
    upper_passband_hz=100e6,
    ripple_db=0.5,
    passband_attenuation_db=1.0,
)
~~~

C++ 使用 ChebyshevFilterParameters 和 ChebyshevFilterModel，响应枚举为 FilterResponse。C 使用 rfmodel_chebyshev_parameters 和 rfmodel_chebyshev_s；C/C++ 两种衰减参数都需明确设置，初始默认结构值（C++）均为 0.1 dB。C 的 low/high pass 采用 lower_passband_hz 存单一边缘，upper_passband_hz 必须为 0。

C 输出四个按行排列的复数，capacity 以复数个数计，input_stopband_open 只接受 0/1；参数错误、容量不足和输入/输出重叠均不改写输出。

~~~powershell
python -m rfmodel examples/chebyshev-bandpass.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/chebyshev-result.json
~~~

## 验证与兼容边界

原生回归使用独立多项式递推验证幅度、多极点乘积验证相位、三阶显式 LC 网络验证所有 S 元素，并用二阶不等终端 ABCD 电路独立验证偶数阶反射。Python 回归覆盖四类响应、多阶和多纹波、独立带边衰减、双拓扑、奇偶阶中心、相邻浮点带边、近 DC 极端案例、JSON、CLI 及错误输入。C/C++ 安装消费者实际调用新接口。

本阶段未运行 SciPy 数值程序（本机独立 Python 环境没有 SciPy），算法参考不等于外部软件实测。SystemVue 本机帮助来源和 SHA-256 记录于 validation/systemvue-2023-chebyshev-help-provenance.json。IL、Amax、Z1/Z2、Ta、特殊参数极限及厂商精确反射拓扑仍待比对；JSON 对未实现字段报错。官方目录条目未升级为兼容通过。

## 工程验证记录（2026-10-09）

- MSVC 2022 x64 Debug/Release 清理重建后，完整 CTest 各 92/92 通过。
- 安装后独立 C/C++ 消费者在两个配置下各 2/2 通过。
- 独立 Python 3.12 从离线 wheel 导入，Chebyshev 10/10、Butterworth 8/8、API 85/85、系统图 51/51 通过。
- 125 个 C/C++ 文件格式检查与 git diff --check 通过。
- 此为本地实现验证；跨平台证据以对应提交的 GitHub Actions 为准，厂商滤波器数值验收仍未完成。
