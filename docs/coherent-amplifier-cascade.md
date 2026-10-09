# 保留失真来源的放大器级联

## 已实现行为

在已有同频多源放大器之上，新增显式的失真传递级联模式：

- C++：`CoherentLimitedAmplifier::evaluate_cascade`。
- C ABI：`rfmodel_coherent_amplifier_cascade`，数组与失败原子性契约沿用 evaluate。
- Python：`Library.coherent_amplifier(..., propagate_distortion=True)`。
- 系统图：`cascaded_amplifier`，字段沿用 limited_amplifier。

每一级先合并相干输入，把载波、谐波和互调的全部非零 RF 功率计入共同驱动。
所有传入谱项都经过相同的压缩增益，并保留原类型、带宽和相干组；
只有 source 类型载波参与本级二阶、三阶新失真生成。生成所用软限幅也由全部
RF 输入功率决定。传播项的本地 order=1 表示一次传递，不把前级谐波改称一阶载波。

系统图用原始载波键对应的来源式复用相干身份。本级新生成的二次谐波与前级
传来的同源二次谐波相干合并；相同规则适用于三阶互调。不同来源式仍各自保留。
默认 limited_amplifier/evaluate 仍拒绝失真输入，以免旧调用悄然改变含义。

`examples/coherent-amplifier-cascade.json` 是 1.0/1.1 GHz、每音 −30 dBm、
第二音相位 90°的双音链路，两级 G=10 dB、OP1dB=20、OPSAT=23、
OIP2=40、OIP3=30 dBm。接口采用输入参考截点，所以 IIP2=30、IIP3=20 dBm。
第一级有 16 个来源项，第二级有 30 个传播/生成路径，按来源合并后仍为 16 项。

```powershell
$env:PYTHONPATH = "$PWD/python"
python -m rfmodel examples/coherent-amplifier-cascade.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/cascade-result.json
```

每级最多 4096 个输入、64 个非零生成载波和 4096 个输出项。前级失真本身不占
生成载波数量，但占输入/输出容量。高阶生成、由前级失真再次非线性混频、
任意递归来源展开、噪声驱动和跨级反向反馈仍未实现。
本模式不是把 harmonic/intermod 重新标为 source 后再运行一次多音算法。

## SystemVue 2023 受控参考

参考版本 2023.0.0.11903。本机官方 `Equations/Amp S Z Param Analysis.wsv`
中的 Design3 拓扑为 Source → RFAmp1 → RFAmp2 → Port_2。
仅使用项目 build-reference 内的独立副本；官方工作区不入库。
归档保存每次运行的 Model/Netlist 原生拓扑、全部受控参数、分析设置、
脚本文本、工作区及原始采集 SHA256、主/路径数据集时间戳和 F/P/V/Z/ID 谱。

正式八组记录为：

| 条件 | 第二级增益 | 通道带宽 | 结果 |
| --- | --- | --- | --- |
| 单音 −30 dBm / 0° | 10 dB | 1 MHz | 通过 |
| 同一单音窄通道控制 | 10 dB | 1 Hz | 通过归一化后比较 |
| 单音 −10 dBm / 45° | 10 dB | 1 MHz | 通过 |
| 同上，UseVolterra=1 / UseWithin=−140 dB | 10 dB | 1 MHz | 当前来源谱通过；不构成次级混频验收 |
| 双音各 −30 dBm / 0°、90° | 10 dB | 1 MHz | 通过 |
| 双音各 −10 dBm，启用上述次级谱设置 | 10 dB | 1 MHz | 当前来源谱通过；不构成次级混频验收 |
| 单音 −20 dBm / 90°，RISO=100 dB | 0 dB | 1 MHz | 未通过 |
| 同上，RISO=140 dB | 0 dB | 1 MHz | 未通过 |

阈值保持复幅度与功率相对误差各 1e-7。共 214 个复幅度/功率检查和 16 个
RF 输入功率检查；6 组完全通过。两组 0 dB 增益合计 22 项未通过，
最大复幅度相对误差约 3.0001e-7，功率误差约 6.0001e-7。
提高反向隔离没有解决差异，原因尚未确定；不将它归因于反向泄漏，也不调松容差。
全部 RFPwrIn 检查通过。总体报告保持 passed=false。

本轮单音的次级谱开关对照未改变结果。双音开启该设置后仍只有本级载波失真
与前级失真的传递路径。现有证据不足以判断任意“失真再混频”的生成/截断规则，
所以该能力保持开放，不因开关回读为开启就宣布验证通过。

输出通道为 1 Hz 时，二、三阶平坦谱显示的 P 分别为宽通道结果的 1/2、1/3，
V 分别为 1/sqrt(2)、1/sqrt(3)。比较器根据实际谱宽与回读通道宽度恢复整个
谱项的复幅度；同时核对 V²/(2 Re Z)=P。此规则只在当前平坦 CW 案例中验证，
不能外推到任意谱形积分或噪声 PSD。

另一次 maximum_order=2 的运行报告明确警告，实际按 3 阶执行，参数回读却仍为 2。
该次采集被严格拒绝进入验收；诊断单独保存于
[阶数诊断](../validation/systemvue-2023-cascade-order-diagnostic.json)。

## 重放与继续采集

```powershell
python scripts/reference/compare-cascade-amplifier.py build-msvc/Release/rfmodel_c.dll validation/systemvue-2023-cascade-captures.json build-reference/cascade-replay.json
```

因保留两个真实差异案例，当前比较器预期退出 1；回归测试确认失败位置及数量，
不能将测试通过误读成全部 SystemVue 数值已一致。

先打开专用 RFModel_CascadeIntermods 工作区，再运行：

```powershell
python scripts/reference/run-systemvue-reference.py cascade build-reference/RFModel_CascadeIntermods.wsv build-reference/cascade-new-run --source-power-dbm -30 --cascade-two-tone --cascade-phase-deg 90 --timeout 45
```

采集器只附着已有实例，要求唯一且命名正确的专用工作区。可控制第二级增益
0/10 dB、RISO 100/140 dB、通道 1/1e6 Hz、单音/双音、相位及次级谱设置。
新运行目录必须不存在；超时后先检查原收集进程，不能盲目重发。
`archive-cascade-reference.py` 核对命令、参数回读和状态后归档；有警告、
旧时间戳、错误拓扑、错误开关及缺失谱项均不进入通过报告。

本轮还未验证多级噪声、双向负载耦合、任意高阶谱或完整 SystemVue RF System
Analysis 路径预算。长期最终目标不变，下一步继续处理次级失真生成与 0 dB 增益差异。

## 本阶段工程验证

- MSVC Debug / Release 完整 CTest：各 71/71。
- 安装后的独立 C/C++ 消费项目：两种配置各 2/2。
- 独立 Python 3.12 wheel：63 项 API、18 项系统图测试全部通过，级联 CLI 示例运行成功。
- 后台参考运行器：17 项测试通过；级联参考验证器：6 项测试通过。
- C/C++ 格式检查：102 个文件通过；新增 Python 和嵌入 C# 参考工具已整理为多行代码。

上述回归检查包含对已知差异被正确报告的断言，不代表全部参考数值一致。
[正式 Release 数值报告](../validation/systemvue-2023-cascade-amplifier.json)
保留两组失败，[受控采集归档](../validation/systemvue-2023-cascade-captures.json)
可在不安装 SystemVue 的环境中重放。报告中的 combined 检查是按来源对测得路径
复幅度求和后比较系统图输出，不等同于完整 NodeTotal 或路径预算验收。
