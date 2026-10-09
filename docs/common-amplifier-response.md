# 多来源分量的共同压缩响应

混频折叠后的多个来源贡献现在可以共同进入 `fundamental_compression`、
`limited_amplifier` 和 `cascaded_amplifier`，再继续经过线性网络、混频或多项式。
真实物理相干分量先归并复波，同组相加、异组加功率；该总功率只求一次公共工作点，
然后对来源贡献施加同一响应。不会用展开项的独立功率重新计算驱动。

## 原生工作点接口

C++ `SaturatingFundamentalCompression::amplitude_gain(total_incident_power_w)`
返回公共复波的实数幅度增益，包括零驱动时的小信号增益。
`P1dBFundamentalCompression::amplitude_gain` 同样提供 P1dB 以下的版本。
原有 `transmit_fundamental` 仍要求选定物理波的功率包含于总驱动；
没有放松这个校验去接受相消前的来源项。

`CoherentLimitedAmplifier::operating_point` 返回：

| 字段 | 含义 |
|---|---|
| fundamental_amplitude_gain | 作用于每个直接传播贡献的公共幅度增益 |
| nonlinear_input_scale | 所有新生成载波项采用的共同输入限幅比例 |
| limited_input_power_w | 限幅后的等效总输入功率 |
| quadratic_voltage_coefficient | 未乘限幅比例的二阶电压系数 |
| cubic_voltage_coefficient | 未乘限幅比例的三阶电压系数 |

非线性生成时先把各 source 类贡献乘 `nonlinear_input_scale`，再用原始电压
系数展开；不得再次对系数乘入同一比例。参考阻抗沿用模型的等实正参考阻抗。
工作点没有相位旋转、噪声或逆向反馈。零驱动时限幅比例为 1、限幅功率为零。

C ABI 提供 `rfmodel_saturating_amplitude_gain`、
`rfmodel_get_amplifier_operating_point`，Python 分别为
`Library.saturating_amplitude_gain`、`Library.amplifier_operating_point`。
C 输出指针必填，非法参数或非有限结果均保持输出不变。参数有效性在空流时也检查。

## 图层规则

- `fundamental_compression` 只接受 source 类分量，不生成谐波/互调。
- `limited_amplifier` 只接受 source 类分量；直接项采用公共基波增益，
  新的二、三阶项按共同限幅比例生成。
- `cascaded_amplifier` 的既有失真也计入真实总驱动，按相同基波增益传播；
  只有 source 类输入生成新的二、三阶项，仍不做失真对失真的二次混频。
- `polynomial_amplifier` 继续按显式电压多项式展开全部输入种类；
  它不自动等同于 RFAMP 的完整压缩及高阶模型。

来源项可在相消前大于真实总波，因此实现从不以“输出总波/输入总波”反求增益。
例如两个不同来源的 +0.01 与 −0.01 在同一 IF 相消，过滤其他边带后总驱动为零，
G=20 dB 时它们分别传递为 +0.1 与 −0.1，并保留各自历史。
限幅放大器仍可保存相互抵消的非零非线性来源贡献，最终物理波与功率为零
（浮点求和可能留下舍入残差）。

共同驱动、公共增益和限幅比例写入阶段测量；来源根、复波和 RF/LO 因子继续
按流保存。[混合图的 RF+LO 来源阶数政策](mixed-polynomial-graphs.md)保持不变。
放大器阶段沿用既有字段，不新增隐式全局阶数裁剪；可由后级多项式显式筛选。

纯线性多项式现在允许 4096 个活跃输入；存在任一非零二阶或更高系数时仍
限制为 64 个活跃输入。4096 项输出上限、枚举工作量及图层累计存储限制不变。
这使限幅放大器展开后的大量来源能够通过一次增益后级，不放宽组合爆炸的边界。

## 示例和证据

[示例](../examples/coherent-mixed-compression.json)为同源双音混频、共同基波压缩、
二阶多项式链路：

```powershell
$env:PYTHONPATH = "$PWD/python"
python -m rfmodel examples/coherent-mixed-compression.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/mixed-compression-result.json
```

原生工作点测试以 50/75 Ω、零驱动、极小驱动、P1dB、饱和区及 1e300 W
数值边界重构既有放大器响应；与独立的原生逐项求值比较直接和二/三阶波。
图回归覆盖实际相干驱动与错误的逐来源功率和之间的差异、全相消、独立源、
失真单独驱动且不再生成新项、空流校验，以及基波/限幅类型拒绝失真输入。
C 测试检查新接口的失败原子性，安装消费者检查公开头文件与导出符号。

本轮扩展组合能力，未重新标定既有 RFAMP 近似，未新增 SystemVue 原始采集。
原有零增益等已知厂商差异仍保留；完整高阶系数、MaxOrder、子谱合并、
AM-PM、变频噪声及非线性反馈仍未验收。

## 本阶段工程验收

- MSVC Debug / Release 完整 CTest 各 76/76。
- 两种配置安装后的独立 C/C++ 消费者各 2/2。
- 独立 Python 3.12 wheel：70 项 API、45 项系统图回归通过。
- wheel 的共同压缩示例运行成功；输入总驱动为 0.0014112096712143901 W，
  公共幅度增益为 3.040373910825172。
- C/C++ 格式检查 109 个文件通过。

这些证明接口与当前模型组合行为，不构成新增的 SystemVue 厂商兼容验收。
