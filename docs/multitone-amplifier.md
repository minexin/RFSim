# 多音限幅放大器的分阶结果

`MultiToneLimitedAmplifier` 已贯通 C++、C ABI、Python 和独立 JSON/CLI 分析。
它按同一输入端口所有 RF bin 的功率之和驱动限幅，并分别返回直接响应、
二阶和三阶产物。当前为匹配单向分析原语，尚不是完整 SystemVue RFAMP。

## 数值契约

输入为离散网格上的 RMS 功率波，单位 sqrt(W)，同一 bin 的相干路径须先合并。
非零 DC 被拒绝。总输入功率 P 使用已有补偿求和，超出可表示范围报错。
沿用单音实测识别的起点/上限偏移：

```text
knee_dBm  = OP1dB_dBm - G_dB - 4
limit_dBm = OPSAT_dBm - G_dB - 1
x = sqrt(P), k = sqrt(knee_W), L = sqrt(limit_W)
x <= k: y = x
x >  k: y = k + (L-k)*tanh((x-k)/(L-k))
limited_input[k] = input[k] * y/x
```

空输入返回空谱，仍验证模型参数和频率网格。直接响应对每个原输入 bin 使用
未限幅输入波和总 P 调用已有 P1dB/增量 tanh 基波模型；二阶、三阶分量用
统一缩放后的所有音调，分别计算 IIP2/IIP3 多项式的齐次分量。正二阶、负三阶
符号沿用既有约定，不是从截点幅值唯一确定的真实器件相位。

二阶/三阶生成的 DC 在返回前移除。同一阶次、同一频率的不同混频组合按本
多项式约定相干合并，但不同阶次保持分离。直接基波与三阶载频项都保留，
不隐式合并或删除。直接模型已经包含压缩，未经验证就将三阶载频项叠加上去
可能重复计入压缩。本接口因此不实现 `SpectrumTransmissionProvider::transmit`，
也不自动接入单谱级联，后续需定义带来源信息的网络传播/测量语义。

返回的 `total_input_power_w` 和 `limited_input_power_w` 用于驱动审计。
OPSAT 标定直接响应的总功率极限，不保证任意截点参数下所有产物的总功率上限。
频率溢出、非有限功率和稀疏卷积资源上限均沿用核心错误处理。

## 接口

```cpp
#include <rfmodel/multitone_amplifier.hpp>
rfmodel::MultiToneLimitedAmplifier amp(20., 20., 23., 20., 10., 50.);
auto result = amp.evaluate({1e8, {{10, .001}, {11, .002}}});
// result.direct, result.second_order, result.third_order are PowerWaveSpectrum.
```

参数依次为 G、OP1dB、OPSAT、IIP2、IIP3（dB/dBm）及共同实参考阻抗 ohm。
`MatchedPolynomialAmplifier::homogeneous_component(order)` 和对应基础多项式
方法提供 0..9 阶分解，缺失阶次返回零模型，越界拒绝，不用大数相减提取弱失真。

C 入口 `rfmodel_multitone_amplifier_evaluate` 输出
`rfmodel_amplifier_component {order,index,amplitude}` 数组，order=1/2/3 分别表示
直接/二阶/三阶，按 order、bin 升序。`rfmodel_amplifier_drive` 返回两种驱动功率。
容量按元素计，最多 10240；输出缓冲区互不重叠，count/drive 必填，空结果可用
NULL 数组。容量不足、参数错误或数值失败均保持数组、count、drive 不变。
ABI 号仍为 1，新增符号要求搭配同版 DLL/共享库。

Python `library.multitone_amplifier(...)` 使用与单音接口相同的参数名称，返回
`LimitedAmplifierResponse`，包含三个 bin→complex 字典及两种功率。

独立文件格式 `rfmodel.amplifier-components` version=1 要求 spacing_hz、input、
parameters，可选 reference_ohms；parameters 必须恰含上述五个 dB/dBm 参数。
输出格式 `rfmodel.amplifier-components-result` 分别编码三个谱，每行含 bin、
frequency_hz、复幅度和功率；不输出含糊的合并总功率。

```powershell
$env:PYTHONPATH = 'python'
python -m rfmodel examples/multitone-amplifier.json `
  --library build-msvc/Release/rfmodel_c.dll --output build-reference/multitone-example.json
python scripts/reference/compare-multitone-amplifier.py build-msvc/Release/rfmodel_c.dll `
  validation/systemvue-2023-two-tone-captures.json build-reference/multitone-replay.json
```

示例为 1.0/1.1 GHz、每音 −3 dBm、零相位。JSON 拒绝额外字段、重复 bin、
布尔版本号、非有限值、非零 DC 和非法参数，即使输入为空也不绕过参数验证。

## 参考验收及限制

复用 [双音实测](systemvue-two-tone.md) 的三组数据，不重新采集、不重新拟合。
每组 10 个无频率重叠的二/三阶产物和 2 个直接载波，共 36 个功率点；原生
输出均满足原 1e-7 相对阈值。比较报告同时保留未限幅模型在两组较高功率输入
下的原始差异。改动观测功率不会改变模型预测，未知告警或过期数据仍被拒绝。

此项只验证等功率、零相位双音下的指定功率分量。不等功率/相位的解析测试
验证代码约定，不能代替 SystemVue 相位或不等功率对照。载频处重叠项的观测
语义、来源逐项追踪、完整级联/反馈、非线性噪声、高阶和 AM/PM 仍未完成。
单音接口保持原来的多音拒绝行为。

上一轮 SystemVue 采集进程仍未返回，本轮不向它重复提交命令；新增模型和
回归均基于已完成并留存散列的数据。验证报告为
validation/systemvue-2023-native-multitone.json。

本机清理重建后 Debug/Release 各 56/56 CTest 通过，安装后的 C/C++ 消费者
两种配置各 2/2 通过。36 个实测功率点最大相对差异约 7.50e-8。新增验证还覆盖
不等功率的解析相位关系、参考阻抗归一化、单音一致性、重叠载波保留、饱和
极限、空输入及错误处理、C 输出原子性和 JSON/CLI 往返；88 个 C/C++ 文件
通过格式检查。跨平台结果以本提交 CI 为准。
