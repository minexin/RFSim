# 理想无损 Butterworth 滤波器

提供低通、高通、带通和带阻四种 LC 梯形网络，保留完整复 S 参数、相位与反射。C++、C、Python 和线性网络 JSON 共用核心实现。此阶段尚未与 SystemVue 滤波器实测对照，不能当作 LPF_BUTTER 等厂商模型的完整等价替代。

## 参数和频率定义

JSON type 为 butterworth_ladder，response 为 lowpass/highpass/bandpass/bandstop，order 是 2..64 的低通原型阶数。带通和带阻变换后的物理阶数为 2×order。低通/高通只接受 passband_hz；带通/带阻只接受 lower_passband_hz 和 upper_passband_hz，后者必须大于前者。所有边缘频率有限且为正，扫描频率允许 DC。

passband_attenuation_db 默认 3.010299956639812 dB，必须有限且严格为正。设 A 为此参数，缩放因子为 (10^(A/10)-1)^(1/(2N))；传输功率为 1/(1+Ω^(2N))，其中 Ω 是缩放后的低通频率变量。每个指定边缘的匹配传输功率为 10^(-A/10)。

| response | 未缩放低通变量 |
|---|---|
| lowpass | f/Fpass |
| highpass | -Fpass/f |
| bandpass | (f²-Flo×Fhi)/((Fhi-Flo)×f) |
| bandstop | -(Fhi-Flo)×f/(f²-Flo×Fhi) |

几何中心为 sqrt(Flo×Fhi)。DC 与带阻中心按极限处理。对数域缩放和带边差分避免高阶、极大/极小频率与窄带边缘的直接乘方溢出。

input_stopband 默认 open，可选 short，表示原型阻带输入端开路或短路拓扑。open 从串联电抗开始，short 从并联电纳开始；两者传输相同、反射符号相反。偶数阶原型的阻带两端反射符号相反，奇数阶相同。这是本模型明确定义的拓扑选项，未确认 BSF_BUTTER 是否提供同义选项。

器件在网络公共正实 reference_ohms 下合成，默认 50 Ω；已有输出复参考阻抗转换可随后使用。内生热噪声由既有 kT(I-SS†) 接口计算，理想无损网络为零（允许浮点舍入残差）。不会因反射造成的插入衰减而加入耗散噪声。

## 可运行接口

~~~python
s = library.butterworth_filter(
    95e6,
    response="bandpass",
    order=3,
    lower_passband_hz=90e6,
    upper_passband_hz=100e6,
    input_stopband="open",
)
~~~

示例 examples/butterworth-bandpass.json 包含 DC、两侧阻带、两个带边和几何中心，并输出内生噪声：

~~~powershell
python -m rfmodel examples/butterworth-bandpass.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/butterworth-result.json
~~~

C++ 使用 ButterworthFilterParameters 和 ButterworthFilterModel（include/rfmodel/butterworth_filter.hpp）。C 使用 rfmodel_butterworth_parameters 与 rfmodel_butterworth_s；低/高通的 lower_passband_hz 是单一边缘，upper_passband_hz 必须为 0。input_stopband_open 必须是 0 或 1。输出为四个按行排列的复数；容量不足、非法参数及输入/输出重叠均失败且不改写输出。

## SystemVue 参数边界

本机 SystemVue 2023 帮助列有原型阶数、边缘频率、Apass、IL、Amax、参考阻抗、端口阻抗与温度等参数。本阶段仅建立理想无损 Butterworth 原型及频率变换。IL 的复反射处理、Amax 的并联泄漏电阻行为、Z1/Z2、Ta 和 Apass=0 退化语义均待验证。JSON 对这些未实现字段报错；没有默默忽略它们。

官方帮助来源页及解压 HTML SHA-256 记录在 validation/systemvue-2023-butterworth-help-provenance.json。这里只记录参数事实，不分发帮助全文。目录条目的兼容评定未升级。

## 验证

C++ 测试以独立 Butterworth 极点乘积验证复传输，以显式三阶 LC 网络验证全部 S 元素；覆盖原型阶数 2..64、双拓扑、四种响应、无损噪声、DC、高频极限与相邻浮点带边。Python 专项 8 项测试覆盖四类响应的多阶/多衰减极点对照、JSON 网络及 CLI 失败保留原输出。C ABI 测试包含输出容量、非法参数和重叠输入保护；安装消费者实际调用新 C/C++ API。

## 工程验证记录（2026-10-09）

- MSVC 2022 x64 Debug/Release 清理重建后，CTest 各 90/90 通过。
- 安装到 build-install-capi 后，独立 C++/C 消费者在两个配置下各 2/2 通过。
- 离线 wheel 由独立 Python 3.12 加载（确认导入路径来自 .whl），滤波器 8/8、API 85/85、系统图 51/51 通过。
- C/C++ 格式检查覆盖 122 个文件，git diff --check 通过。
- 此记录为本地实现验证；厂商实测对照仍未完成。跨平台结果以对应提交的 GitHub Actions 为准。
