# 同频多源非线性放大器

## 接口与模型

`CoherentLimitedAmplifier`、C ABI `rfmodel_coherent_amplifier_evaluate` 与 Python
`Library.coherent_amplifier` 在同一频率上保留多个独立来源。先按
`(bin, kind, bandwidth_hz, coherence_group)` 合并输入，再用合并后的总 RF 功率
驱动已有的共同基波压缩和软限幅模型。直接项使用未限幅的输入及共同压缩，
二、三阶项使用共同限幅后的输入；标定公式与原有 MultiToneLimitedAmplifier 相同。

参数为 power_gain_db、output_p1db_dbm、output_saturation_dbm、
input_ip2_dbm、input_ip3_dbm 和 reference_ohms（默认 50）。截点为输入参考值。
输入/输出波为 sqrt(W) 复幅度；匹配、正实参考阻抗、正频率离散网格。

返回的 inputs 是有序归并输入。每个输出项包含 order、input_indices、component：

- input_indices 是 inputs 中**带符号的一基位置**，不是频率格点。负号表示共轭，
  重复表示同一输入的幂；C 固定数组未使用项为零，Python 元组只含有效项。
- 输出频率为带符号输入频率之和。输出带宽为所有输入带宽之和（包括共轭项）。
- 原始直接项保留 source 类型和组号。全由同一正输入重复产生的项为 harmonic；
  其余生成项为 intermod，包括落在载波频率上的三阶项。
- 正负来源不做代数消去。例如 [-2, 1, 2] 和 [-1, 1, 1] 保留两个不同身份。
- 项按 order、输出 bin、input_indices 排序。生成组号超过全部输入及
  reserved_group_max，每个不同来源式分配不同组。不同调用的本地组号不可直接混用。
- 保留合并后幅度为零的直接组；不输出 DC、负频率或幅度为零的生成项。

最多 4096 个输入分量、64 个非零归并输入、4096 个输出项。超限、组号耗尽、
非法频率/带宽、数值溢出均报错，不截断。C 输出数组、计数及 drive 不得重叠；
任一错误均不修改任何输出。

```python
from rfmodel import CoherentComponent, SpectrumKind

result = library.coherent_amplifier(
    1e8,
    [CoherentComponent(10, SpectrumKind.SOURCE, 1., 7, .01),
     CoherentComponent(10, SpectrumKind.SOURCE, 1., 9, .02j)],
    power_gain_db=20., output_p1db_dbm=20., output_saturation_dbm=23.,
    input_ip2_dbm=20., input_ip3_dbm=10., reserved_group_max=100)
# 两个独立同频载波，15 个直接/二阶/三阶 RF 项。
```

## 系统图与后级传播

`rfmodel.coherent-system` version=1 新增 `limited_amplifier` 节点，必填
id/type/input/output 以及上述五个 dB/dBm 参数，参考阻抗沿用图的 reference_ohms。
阶段结果保存 input_power_w、limited_input_power_w、reduced_inputs 和 origins；
origins 内索引指向该阶段的 reduced_inputs，组号为图范围内的最终组号。

执行器以完整的带符号输入键多重集管理生成身份，忽略放大器实例名称。
同源经过并行放大器产生的相同来源式可相干合路；不同来源式、带宽和时钟保持区分。
同一输入流被引用两次不自动分功率，需要物理分路时应使用线性网络。
线性后级保留类型、带宽、组号；混频后级继续按 RF/LO 来源管理组号。

`examples/coherent-harmonic-combiner.json` 展示两条反相支路：
每路 1 GHz、−10 dBm，经过相同放大器及各 0.5 幅度合路。
直接和三阶项相消，二次谐波保留。改成独立源后各项按功率合并。
此例为解析回归，未声称新增 SystemVue 多放大器实测。

## 实测证据与边界

`validation/systemvue-2023-coherent-amplifier.json` 重放已保存的
`systemvue-2023-shared-compression-captures.json`，未重新启动或修改 SystemVue。
21 组无警告案例、206 个 RF 来源项全部通过复幅度及功率比较，阈值均为 1e-7。
最大复幅度相对误差约 3.7501e-8，最大功率相对误差约 7.5001e-8。
逐项核对 V2/Z2/P2、频率上下界、来源全集与组号等价关系，不要求两端数字组号相同。

覆盖同频锁相/独立源、0/90/180 度、等功率/20 dB 不平衡和强弱互换，
并含一个 1.0/1.1 GHz 对照以及低功率关噪声对照。两组完全相消的有警告记录
保留为诊断，不进入通过结论。参数和数据新鲜度沿用原采集的严格回读校验。

```powershell
python scripts/reference/compare-coherent-amplifier.py build-msvc/Release/rfmodel_c.dll validation/systemvue-2023-shared-compression-captures.json build-reference/coherent-amplifier-replay.json
```

默认 evaluate / limited_amplifier 仅接受 source 类型输入。已经生成的谐波/互调再次进入该模式会报错，
直到完整递归来源及高阶截断规则实现；不能把类型改回 source 规避这一限制。
尚未完成噪声驱动压缩、AM/PM、跨级双向反馈、任意谱带积分、完整路径预算和
全 RF Design 器件兼容。原有小信号 RFPwrIn 噪声差额与衰减器路径预算差异仍然开放。
单个放大器的确定性来源谱通过，不等于完整总谱或系统功能验收。

本轮 MSVC Debug/Release 各 70/70，安装 C/C++ 消费者各 2/2；
独立 Python 3.12 wheel 的 API 62 项、系统图 17 项通过。
反相/独立支路的解析期望及实际结果见
[解析验证记录](../validation/coherent-amplifier-analytic.json)。
远端三平台 CI 以本次提交的运行结果为准。

新增显式 [失真传递级联模式](coherent-amplifier-cascade.md)，可将前级失真计入共同驱动并传播，与本级同源失真相干合并；次级失真再混频仍未实现。
