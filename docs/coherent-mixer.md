# 保留 RF/LO 相干关系的理想混频

本接口将已有的理想实数混频与相干多端口网络连接起来。一次批处理可包含多个混频
支路，每个输入给出一个 RF 分量和规定的无噪声 CW LO。输出保留支路顺序，后续
网络按照各支路的实际传输系数传播，再执行相干合并。

## 核心与绑定

C++ 头文件为 `rfmodel/coherent_mixer.hpp`：

```cpp
std::vector<CoherentComponent> mix_coherent_components(
    double spacing_hz,
    const std::vector<CoherentMixerInput>& input,
    std::uint64_t reserved_group_max = 0);
```

`CoherentMixerInput` 依次包含 `component`、`lo_bin`、
`conversion_gain_db`、`lo_phase_radians`、`lo_coherence_group`。
C 的对应结构为 `rfmodel_coherent_mixer_input`，函数为
`rfmodel_mix_coherent_components`；Python 使用同名数据类型及
`Library.mix_coherent_components` 方法。没有改动已有 ABI 1 的结构或函数签名。

每个输入准确产生两个输出：`[差频, 和频]`，零幅度也保留。输出数量为输入的
两倍，最多 4096 项。第 i 条支路的两个输出位置为 `2*i`、`2*i+1`；
可据此构造 `PortCoherentComponent`，送入 `Network.transmit_coherent`。
这里不在混频前后跨支路合并，避免合路网络的相移、损耗被跳过。

复幅度由已有 `IdealRealMixer` 计算，单位 sqrt(W)。每个非重叠边带的
功率转换增益为 `10^(conversion_gain_db/10)`；RF 与 IF 使用相同正实参考阻抗。
LO 相位使用弧度，功率波变换在这种匹配条件下与该共同阻抗的数值无关。
RF 高于 LO 时，和频乘 `g*exp(j*phi)`、差频乘 `g*exp(-j*phi)`；
RF 低于 LO 时，正差频为 `conj(a_RF)*g*exp(j*phi)`。
分量的 source/harmonic/intermod 类型及带宽不变；CW LO 不增加带宽。

## 相干身份和使用范围

每个不同的有序 `(RF 相干组, LO 相干组)` 对获得一个新组号。同一批中，
同一 RF 来源或参考时钟配合同一 LO 来源或参考时钟的并行支路共享输出组。
最终合并仍要求输出频率、带宽、类型一致；LO 相位影响复幅度，不改变组号。
不同 RF 组或不同 LO 组不会因输出频率相同而自动相加电压。

先对所有独立 RF 和 LO 源共同调用 `assign_source_coherence`，或由调用方
在一个共同命名空间中提供有效组号。输出组号大于本次所有 RF/LO 输入组号，
也大于 `reserved_group_max`。若同时存在旁路分量，应将其最大组号传入
`reserved_group_max`，防止新组与旁路身份碰撞。后续混频级可接收前一级输出组。

组号仅在本分析上下文内有意义；同一输入集合的顺序变化不会改变映射。
增加或移除输入可能使组号变化。**需要相干合并的并行支路必须一次提交**，
不能将各自独立调用分配出的组号直接合并。当前接口没有跨调用的全局谱系注册表，
也不推断不同混频级数、不同 LO 顺序经过代数抵消后是否代表相同来源。

最多 2048 条输入，每个 RF 分量遵守已有相干接口的正频率和有限带宽约束。
RF 等于 LO 会产生 DC，本接口明确拒绝；差频带跨过 DC、非有限值、频率/功率
溢出和 uint64 组号耗尽也会报错。已有 `IdealRealMixer` 的标量 DC 功能保留，
但不能拿它的隐含全相干频谱代替独立组的 DC 语义。空输入仍验证频率网格。
C 输出数组和计数在任何失败时均保持原值，不支持内存重叠。

## 可复现镜像抑制链路

运行 `examples/coherent-image-rejection.py`，传入构建出的共享库路径，并设置
`PYTHONPATH=python`。示例使用两路相干 1 GHz、各 1 W 的 RF 入射波，相位为
0°/90°；两路 800 MHz LO 相位也为 0°/90°，每边带转换增益为 0 dB。
变频后经过耦合幅度各为 `1/sqrt(2)` 的匹配三端口合路器：

| LO 关系 | 200 MHz 差频 | 1800 MHz 和频 |
|---|---:|---:|
| 同一参考时钟 | 2 W | 数值舍入范围内为零 |
| 独立参考时钟 | 1 W | 1 W |

原生回归还覆盖负差频折返、同一 IF 的镜像碰撞、独立 RF 来源、类型/带宽保留、
零项、输入排列、旁路组号预留和边界错误。C 回归验证容量不足、后续输入错误、
组号耗尽均不部分写入；Python 回归贯通源时钟分组、混频、多端口合路。
安装后 C/C++ 消费者直接调用新增接口，Python wheel 单独验证绑定。

## 与 SystemVue 2023 的证据边界

规则依据为本机官方 `Help/systemvue.qch` 中的 `sim/Coherency.html`：
混频器需要同时满足输入与 LO 的来源/参考时钟关系，且输出类型、中心频率和
带宽必须相同。官方举例为共用 RF/LO 源经分路移相后组成镜像抑制混频器。

本阶段是依据该文字规则和解析公式实现的接口回归，**尚无新增 SystemVue
混频器实测**。现有 SystemVue 双源线性合路数据不能证明此混频实现兼容。
当前 SystemVue 脚本错误对话框仍阻塞新的采集，未因超时重启或重复提交分析。
完整 Mixer 模块仍缺 LO 驱动功率、求解型 LO 端口、泄漏、杂散/转换矩阵、
压缩、相位噪声和变频噪声；相干前馈图已接入 JSON/CLI，见下文；完整 RF System Analysis 仍待实现。

## 本阶段验证记录

MSVC x64 Debug/Release 全量重建后，CTest 各 65/65 通过，安装后的独立
C/C++ 消费者各 2/2 通过。独立 Python 3.12 wheel 的 Python API 回归
60 项通过。98 个 C/C++ 文件通过格式检查。数值、容差与本机 DLL 摘要见
[解析验证记录](../validation/coherent-mixer-analytic.json)；其中不包含厂商实测
或后续远端 CI 结果。新增的全链路测试会由三平台 CI 持续执行。

新增 [相干 RF 前馈图文件](coherent-system-file.md) 为一次文件分析维护持续来源注册表，支持跨阶段复用 RF/LO 身份；原生批处理 API 仍为无状态接口。
