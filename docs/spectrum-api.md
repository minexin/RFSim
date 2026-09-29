# C/Python 非线性与混频频谱接口

本接口复用 C++ MatchedPolynomialAmplifier 与 IdealRealMixer，不在 Python 中重写频谱算法。
它提供匹配条件下的单向器件传输，尚不包含非线性网络反馈或谐波平衡求解。

## 输入与输出

频率采用公共正数 spacing_hz 与非负整数 bin，实际频率为 bin*spacing_hz。
输入是实信号的正频率表示，负频率由共轭关系补全。波幅单位 sqrt(W)，正频率单音功率
为幅度模方；DC 波幅必须为实数。与电压傅里叶系数的 sqrt(2) 换算由核心处理。

C 使用 rfmodel_spectrum_bin（int index、rfmodel_complex amplitude）数组。
rfmodel_cubic_amplifier_transmit 接受功率增益 dB、输入 IP3 dBm、实参考阻抗；
rfmodel_ideal_mixer_transmit 接受正 LO bin、每边带转换增益 dB、LO 相位弧度、实参考阻抗。
两者输出按 bin 升序排列，零项可被省略。输入最多 2048 项，不允许重复 bin。
输入为空时允许 NULL，输出为空时允许 NULL；output_count 必须非 NULL。
分配 RFMODEL_SPECTRUM_CAPACITY（4096）项足以容纳当前核心输出上限。
容量不足会返回非法参数，不截断结果，也不改变输出数组或 output_count。
更复杂的谱仍可能触发核心稀疏卷积工作量上限，并明确报错。

Python 使用字典 `{bin: complex_amplitude}`，返回同类字典；封装管理输出内存。

```python
import math
from rfmodel import Library

library = Library("build-msvc/Release/rfmodel_c.dll")
tones = {10: math.sqrt(1e-5), 13: math.sqrt(1e-5)}
output = library.cubic_amplifier(
    1e6, tones, power_gain_db=20., input_ip3_dbm=10.)
print(abs(output[7])**2)  # 1e-9 W at lower IM3 frequency 7 MHz
converted = library.ideal_mixer(
    1e6, output, lo_bin=8, conversion_gain_db=-6., lo_phase_radians=0.)
```

两种 Python 调用都有 reference_ohms=50 默认参数；混频器还默认转换增益为 0 dB，
LO 相位为 0。串接时须使用同一频率间隔及参考阻抗，库不会自动重采样或重归一化。
本版封装要求动态库提供新增频谱符号；属于 ABI 1 的增量扩展。

## 模型边界

三阶放大器从 IIP3 标定负三次电压系数，支持基波压缩、谐波及互调。
它不是饱和限制器，深压缩区的多项式外推不能视为真实 PA 行为；没有记忆效应、
AM/PM 或自动噪声。混频器由固定 LO 驱动，保留和频、差频、穿过零频率的共轭折叠
以及落在同一 bin 的相干叠加。没有自动 IF 滤波、LO 泄漏、LO 端口加载或变频噪声。

解析回归覆盖两音 IM3、单音 1 dB 压缩点、上下边带 LO 相位、同频混频 DC、
相干抵消、重复 bin、非法 DC/间隔及短输出缓冲区。安装消费者额外调用混频器符号。
JSON 线性网表不接受这些器件；新增独立的 [单向频谱链路格式](spectrum-model-file.md)
可重放放大器和混频器级联。一般非线性网表及 SystemVue 对照仍待完成。

2026-09-29 本地验证：MSVC Debug/Release 全套各 42/42，安装消费者各 2/2，
安装后的 Release 动态库通过十八项 Python 测试。Debug 初次运行中相位测试采用
精确比较而失败，改为浮点容差比较后全套通过；数值核心未为该测试添加修正。
本次提交的跨平台 CI 及 SystemVue 实测仍待核验。
