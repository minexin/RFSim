# P1dB 标定的单音主信号压缩

`rfmodel/fundamental_compression.hpp` 提供 P1dBFundamentalCompression，构造参数为
小信号功率增益 dB 和实际输出 OP1dB（dBm）。输入和输出为 sqrt(W) 复功率波，
适用于外部匹配条件下的单音基波；相位保持不变。它不是多音频谱传输器。

```cpp
rfmodel::P1dBFundamentalCompression amplifier(20., 10.);
auto output = amplifier.transmit_fundamental({0.001, 0.002});
double input_limit_w = amplifier.input_p1db_watts();
```

设 r=10^(-1/20)、功率增益 G，则 Pi1=Po1/(G*r²)，输入 1 dB 点的 dBm 为
OP1dB−GdB+1。输出波为 sqrt(G)*a*[1−(1−r)*|a|²/Pi1]。
因此在 Pi1 处输出恰好为 Po1，增益恰好比小信号低 1 dB。
接口接收零输入；高于 Pi1 则报 out_of_range，仅容纳边界的浮点舍入误差。
非法参数和非有限输入报 invalid_argument，输出溢出报 overflow_error。

该模型实现的是已独立验证的低功率主信号假设，不包含谐波、互调、噪声、AM/PM、
反向传输、失配反馈或饱和；不实现 SpectrumTransmissionProvider，不能逐音调用
后宣称获得多音总功率压缩结果。现有 MatchedPolynomialAmplifier 保留 IIP3 标定和
谐波/互调语义，二者不能混同。JSON 批处理通过 p1db_fundamental 级显式限制单音输入。

模型公式与本机 SystemVue 帮助描述的 P1dB 以下三阶主信号行为一致，四个远低于
P1dB 的实测点支持此假设，但尚未在靠近 P1dB 的隔离器件中完成厂商数值验证。
不改变既有 SystemVue 兼容报告，也不覆盖其高阶专有拟合和饱和算法。

回归检查 P1dB 的实际输出功率及 1 dB 增益下降、复数相位、零输入、小信号极限、
半标定功率、越界、非有限参数，以及已归档两级链路解析结果。独立安装消费者
同时使用此公开头文件验证标定点。

2026-09-29：MSVC Debug/Release 全套各 45/45，独立安装消费者各 2/2。
本轮没有新增 SystemVue 采集；跨平台 CI 仍待核验。

## C 和 Python

C 函数 rfmodel_p1db_fundamental 接受 power_gain_db、output_p1db_dbm、复数 incident，
并将基波写入非空 output 指针。输入和输出均为 sqrt(W)，无需额外指定参考电阻。
任何失败均保持 output 原值；越界及非法参数返回 RFMODEL_INVALID_ARGUMENT。
接口不生成谐波或多音频谱，也不隐式钳制越界功率。

```python
output = library.p1db_fundamental(
    0.001 + 0.002j, power_gain_db=20., output_p1db_dbm=10.)
```

Python 封装调用同一个 C++ 核心，错误以 RFModelError 返回。该符号属于 ABI 1
增量扩展，需要使用更新后的 Python 包和动态库组合。标定点、相位、零输入、
非有限输入及越界由 Python 回归覆盖；C 测试另外检查空指针及失败不改写输出。

2026-09-29 接口验证：Debug/Release 全套各 45/45，安装消费者各 2/2，
安装后的 Release 动态库通过 Python 39 项测试。未新增厂商实测。

## 原生 JSON 链路与归档 SystemVue 信号比较

2026-09-29：新增 `scripts/reference/compare-antenna-compression.py`，通过公开
Python → C ABI → C++ 路径执行两级线性衰减器和两级 `p1db_fundamental`。
输入为已归档的四点功率扫描及非线性参数审计；通过 capture SHA256 关联两者，
核验源功率、节点顺序、OP1dB 和 AM/PM 关闭条件。衰减和增益沿用天线案例的
固定参数映射；此脚本不是任意工作区的自动模型提取器。

```powershell
& 'C:/Program Files/Keysight/SystemVue2023/Python/python/python.exe' `
  scripts/reference/compare-antenna-compression.py `
  build-msvc/Release/rfmodel_c.dll `
  validation/systemvue-2023-antenna-power-sweep.json `
  validation/systemvue-2023-antenna-nonlinear-settings.json `
  build-reference/native-compression-result.json
```

报告 `validation/systemvue-2023-antenna-native-compression.json` 记录动态库及两个
输入文件的 SHA256。四点（−70、−60、−50、−40 dBm）× 五节点 × 增益/信号功率
共 40/40 项通过，固定相对阈值仍为 1e-7。增益最大相对误差约 2.0000001e-8，
信号功率最大相对误差约 4.4999999e-8。没有用实测输出拟合校正系数。

新增 CTest `native_compression_reference` 包含三项测试：归档结果比较、篡改测量
必须失败、不一致证据必须拒绝；由现有三平台 CI 的 CTest 步骤执行。
本机 Debug/Release 均通过该测试及 Python API 的 42 项测试。本次没有修改
C++ 核心或重新采集 SystemVue，也未核验此提交的远端 CI。

这是信号子集的独立报告，不替换包含噪声的原始兼容报告。低功率吻合不能证明
靠近 P1dB、饱和、多音互调、AM/PM、失配或噪声已兼容；这些仍须独立验证。

## 接近压缩点的新实测：存在未通过项

2026-09-29 通过既有 Python/COM 驱动，在专用天线参考工作区分别运行源功率
+10 和 +11 dBm。其他线性参数及两台放大器的 16 个非线性参数均重新审计通过。
两级放大器 OP1dB 均为 60 dBm（1000 W）；+11 dBm 输入时末级实际输出约
978.756 W，已接近输出压缩标定功率。本实验是完整四级链路，不是隔离放大器。

原生 JSON 链路比较结果为 **16/20**，保持 1e-7 相对阈值。失败均在 RFAmp2：

| 源功率 | 增益相对误差 | 信号功率相对误差 |
| --- | --- | --- |
| +10 dBm | +1.1690274e-6 | +1.1940274e-6 |
| +11 dBm | +1.8931755e-6 | +1.9181755e-6 |

RFModel 的预测略高。这证明远低于压缩点的吻合不能直接推广到压缩点附近；
现有证据还不能将差异唯一归因于高阶系数、反馈或厂商其他内部处理。未引入
经验修正，未改变数值核心，也未把信号比较当成噪声兼容验证。

证据文件位于 validation：`systemvue-2023-antenna-plus10.json`、
`systemvue-2023-antenna-plus11.json`，以及 `systemvue-2023-antenna-near-compression-`
前缀的 `sweep.json`、`settings.json` 和 `comparison.json`。原始大体积采集仅保留
在 build-reference 中；归档的派生证据包含采集散列、时间和参数。复跑上一节命令时
将 sweep/settings 换为这里的文件即可，比较程序应返回 1，表示数值未通过。

结束后重新采集 −50 dBm（build-reference/antenna-power-restored-003），CGAIN、
CNF、CND、DCP 四组共 20 个测量值与 antenna-run-003 完全一致；未保存工作区。
新增回归要求明确保留这四项已知差异；将来改进模型后应连同证据重新评估，不能
仅放宽阈值。下一步需隔离单放大器并控制反向传输，区分模型项与链路效应。

### 使用实测前级功率的逐级诊断

比较脚本现另行输出 `local_signal_diagnostic`：逐个执行原生 JSON 器件级，
将该级的输入设为 SystemVue 前一个节点的 DCP（取其平方根作为实数功率波）。
这是在匹配假设下的条件诊断，不能视为新增独立器件实测，也没有测量反向波。
原端到端比较仍只从声明的源功率开始；诊断结果不参与 passed 判定。

对 +10、+11 dBm 的归档采集，末级 RFAmp2 的条件输出残差分别为
+1.1654561e-6、+1.8914633e-6，仍超过原阈值。RFAmp1 约为 +5e-9，
两个衰减器也均低于 1e-7。因此在上述匹配假设下，前级功率误差的累积不足以
解释末级差异。这里不能据此断定厂商内部算法或高阶系数；独立器件和反向波
控制实验仍未完成。

低功率与近压缩点两份报告均补充此诊断；原检查值和验收结论不变。新增测试
会扰动一个中间节点实测功率，确认条件诊断随之变化，而端到端 RFModel 预测
完全不变，防止误把实测中间结果注入兼容计算。Debug/Release 上该组五项测试
通过；本次未修改数值核心、未新增 SystemVue 采集。

随后新增了独立 RFAMP 的 +0.9 dBm 无告警实测，增益/功率 2/2 通过；
详见 [独立压缩参考](systemvue-single-compression.md)。四级链路差异仍保持未通过。

## 总 RF 输入功率诊断

继续检查两份近压缩点原始采集时发现，主数据集中的 `RFPwrIn` 高于前级节点
基波 DCP。本机官方帮助 `sim/Spectrasys_Total_RF_Power_Entering_a_Part.html`
定义该量为器件所有端口的所有输入信号功率总和；不能把它等同于前级基波功率，
也不能把差额全部认定为前向谐波。

`scripts/reference/diagnose-total-drive.py` 用已实现的原生 P1dB 函数计算总驱动
对应的压缩功率增益，再乘以实测前级基波功率。即先计算
`gain = |transmit_fundamental(sqrt(RFPwrIn))|² / RFPwrIn`，然后计算
`predicted_fundamental_output = gain * preceding_DCP`。没有拟合常数。
脚本验证原案例主要参数、数据新鲜度、元素名称映射以及末级 OP1dB。

| 源功率 | 前级基波输入 W | 末级总 RF 输入 W | 原条件残差 | 总驱动条件残差 |
| --- | --- | --- | --- | --- |
| +10 dBm | 0.972245225605 | 0.972251372206 | +1.1654561e-6 | +6.17619e-9 |
| +11 dBm | 1.223847639527 | 1.223857400218 | +1.8914633e-6 | +5.80639e-9 |

两点剩余差异均小于 1e-8，支持“基波之外的驱动功率影响压缩”这一后续实现
方向。**这仍是使用厂商实测输入的条件诊断**：RFModel 尚未自己算出 RFPwrIn，
报告没有兼容通过判定，原端到端 16/20 结果不变。尚未将差额分解为谐波、
反向信号或其他分量，也没有证明厂商内部算法完全等同。

证据为 `validation/systemvue-2023-antenna-total-drive-diagnostic.json`，以及
可用于回归的 `validation/systemvue-2023-antenna-total-drive-captures.json`。
后者是两份精简采集的数组，每份保留原始采集 SHA256。复现命令：

```powershell
& 'C:/Program Files/Keysight/SystemVue2023/Python/python/python.exe' `
  scripts/reference/diagnose-total-drive.py build-msvc/Release/rfmodel_c.dll `
  build-reference/total-drive-result.json `
  --capture 10 build-reference/antenna-power-plus10-001/capture.json `
  --capture 11 build-reference/antenna-power-plus11-001/capture.json
```

新增 CTest total_drive_diagnostic 两项测试覆盖上述结论及功率/元素映射拒绝；
与现有两组压缩参考测试一起通过 Debug/Release。未新增厂商采集或修改数值核心。
下一步需建立由 RFModel 自行求得总输入功率的频谱传播，再验证条件诊断能否转为
独立预测；不可直接把实测 RFPwrIn 写入兼容模型。

## 总功率驱动的公开接口

2026-09-30：C++ 新增 `transmit_fundamental(incident, total_incident_power_w)` 重载，
C 新增 `rfmodel_p1db_driven_fundamental`；Python 的 `p1db_fundamental` 新增可选
`total_incident_power_w`。不提供该参数时仍使用基波自身的功率。

```python
output = library.p1db_fundamental(
    0.001j, power_gain_db=20., output_p1db_dbm=10.,
    total_incident_power_w=1e-4)
```

总功率单位为 W，必须有限、非负、包含指定基波的功率，且不超过输入 P1dB。
包含关系和 P1dB 边界允许 16 倍浮点 epsilon 的相对舍入误差。压缩增益由总功率
决定，基波相位保持；基波为零时输出为零，但仍检查总功率。其他信号的波形、
相位及其传播必须由调用者另行计算。本接口不产生谐波/互调，不是完整多音模型，
也不会从一个标量重建完整频谱。JSON 单音级仍只使用自身基波功率。

总功率诊断脚本改用该原生接口，归档报告同步记录新动态库散列。该报告仍使用
实测总功率，不能代表 RFModel 自主计算链路总驱动。C ABI 保持版本 1 的增量扩展，
Python 与动态库需配套更新；C 失败保持输出不变。

验证：MSVC Debug/Release 全套各 48/48；安装后独立 C/C++ 消费者各 2/2；
安装 Release 动态库通过 Python 43 项测试。测试覆盖 P1dB 总驱动下的部分基波、
相位、单音等价、零基波、总功率不足、负值、非有限及越界。此次未新增 SystemVue
采集，远端跨平台 CI 尚未核验。
