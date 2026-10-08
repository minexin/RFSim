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
验证代码约定，不能代替 SystemVue 相位或不等功率对照。载频处重叠项的相位/
合并语义、跨级来源追踪、完整级联/反馈、非线性噪声、高阶和 AM/PM 仍未完成。
单音接口保持原来的多音拒绝行为。

上一轮 SystemVue 采集进程仍未返回，本轮不向它重复提交命令；新增模型和
回归均基于已完成并留存散列的数据。验证报告为
validation/systemvue-2023-native-multitone.json。

本机清理重建后 Debug/Release 各 56/56 CTest 通过，安装后的 C/C++ 消费者
两种配置各 2/2 通过。36 个实测功率点最大相对差异约 7.50e-8。新增验证还覆盖
不等功率的解析相位关系、参考阻抗归一化、单音一致性、重叠载波保留、饱和
极限、空输入及错误处理、C 输出原子性和 JSON/CLI 往返；88 个 C/C++ 文件
通过格式检查。跨平台结果以本提交 CI 为准。

## 2026-10-08：本级混频来源追踪

`evaluate_terms(incident)` 返回 `TracedAmplifierResponse`，保留每个本级生成组合。
每项含 order、bin、contributors 和复幅度。contributors 是升序的带符号输入
bin，多次出现表示乘方，负号表示共轭；各项之和等于输出 bin。直接响应只有
一个正 contributor；二阶/三阶结果不包含 DC 或负频率镜像。

例如 bin 10 处的 `[-11,10,11]`（交叉调制）和 `[-10,10,10]`（自身三阶项）
独立返回，即使输出频率相同。展开使用无序二/三元组合及 1/2/3/6 的排列重数，
不通过相减较大的聚合谱提取弱失真。三类分谱接口不变，单阶项按复幅度合并可
重建该阶聚合谱；这只是现有相位约定下的恒等关系，并非真实器件相位验证。

C 入口 `rfmodel_multitone_amplifier_terms` 参数与 evaluate 一致，输出项为
`rfmodel_amplifier_term {order,index,contributors[3],amplitude}`；只用前 order 个
contributor，其他槽补零。按阶次、输出 bin、contributors 排序；容量不足或
异常时数组/count/drive 全部保持不变。Python 同名方法省去 `rfmodel_` 前缀，
返回 `TracedAmplifierResponse` 和含元组 contributors 的 `AmplifierMixingTerm`。
JSON 输入可选 `include_terms: true`，输出增加 terms 数组；省略时保持旧结果。
该选项只接受布尔值。

来源展开最多返回 4096 项、检查 1000 万个组合，另受已有聚合卷积资源限制；
超过任一限制整体报错，不静默截断。因来源项可能远多于聚合 bin，某些输入
可以聚合求解但无法在当前来源预算下展开。来源只表示本级输入 bin，不包含
跨器件传播历史；级联需另行保留原始源身份和传播路径，不能将 bin 当全局 ID。

新的比较器 compare-amplifier-terms.py 按完整来源表达式匹配 SystemVue IDName，
并检查预期 16 个 RF 项全部存在。三组等功率双音共 48 个功率点在 1e-7 阈值
内通过，最大相对差异约 7.50e-8，包含此前未单独检验的 12 个载频三阶项。
−30 dBm 每音下，自身项约 1e-12 W、交叉项约 4e-12 W；该差异来自排列重数，
未按观测拟合修正。报告为 validation/systemvue-2023-amplifier-terms.json，保存
原始/精简采集及 DLL 散列。仍未验证合并后载频、测量通道功率或来源项相位。

```powershell
python scripts/reference/compare-amplifier-terms.py build-msvc/Release/rfmodel_c.dll `
  validation/systemvue-2023-two-tone-captures.json build-reference/terms-replay.json
```

新增回归检查不等功率复相位、三音频率碰撞的分项重建、资源限制、C 输出
原子性、安装消费者、JSON/CLI，以及改动某个载频项只使该项失败、来源错误
不能仅按频率匹配。沿用之前完成的三组参考，无需重新启动 SystemVue。

2026-10-08 核对旧采集日志：003 调用已于 2026-09-30 09:27:56 UTC 返回，最终
被采集器以未生成新鲜数据拒绝；旧 runner 的 timeout_unresolved 状态文件没有
自动追踪后续退出。当前 SystemVue 已不存在，该次采集仍无有效结果。后续新
采集应重新核对实例和专用工程，不能复用这次超时输出作为参考。

本次 Debug 全套 57/57 在 2026-09-30 完成，2026-10-08 继续完成 Release 清理
重建与 57/57 测试；期间数值实现未再更改。两种配置的独立安装消费者各 2/2
通过，C++ 格式检查覆盖 88 个文件。跨平台结果绑定后续提交 CI。

来源项可继续经过 [线性后级](term-propagation.md)，通过可选 post_stages 输出
逐级快照；尚不隐式定义重叠项的总功率测量，也不接入第二个非线性级。
