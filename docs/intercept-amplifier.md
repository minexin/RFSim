# IIP2/IIP3 标定的多项式放大器

该模型以小信号功率增益 G、等幅双音外推输入截点 IIP2/IIP3 标定二次和三次
电压系数。它用于生成低阶失真频谱，不独立接受 P1dB 或饱和功率，也不等价于
SystemVue RFAMP 的完整高阶模型。输入输出端口匹配，无反向传输或噪声模型。

设 A=10^(G_dB/20)，P2/P3 为输入截点功率（W），R 为公共实数参考阻抗，则
`c1=A`、`c2=A/sqrt(2 R P2)`、`c3=−2 A/(3 R P3)`，c0=0。
截点只确定失真幅值，不能唯一确定相位；当前约定正二次系数和负三次系数。
如需其他符号或相位行为，应使用通用多项式或另行建立复数行为模型。

每音输入功率为 P 时，双音和/差频的 IM2 功率为 `A² P²/P2`，IM3 功率为
`A² P³/P3²`；单音二次谐波为 `A² P²/(4 P2)`。这些是外推失真关系，不能把
截点处的实际压缩基波当作线性外推值。DC、谐波和所有互调产物均保留。

C++ 构造入口：

```cpp
auto amp = rfmodel::MatchedPolynomialAmplifier::from_intercepts(
    "amp", 20., 20., 10., 50.);
```

C 入口为 `rfmodel_intercept_amplifier_transmit`，沿用频谱输出缓冲区约定。
Python 和 JSON 分别为：

```python
output = library.intercept_amplifier(
    1e6, {10: .001, 13: .001}, power_gain_db=20.,
    input_ip2_dbm=20., input_ip3_dbm=10., reference_ohms=50.)
```

```json
{"id":"amp","type":"intercept_amplifier","power_gain_db":20,
 "input_ip2_dbm":20,"input_ip3_dbm":10}
```

参数均为有限数值。截点功率或系数无法表示时拒绝，不截断或钳制。
单音/双音解析测试同时检查 IM2、IM3、二次谐波，50/75 ohm 的功率波结果一致，
并检查非法 IP2 和 JSON/Python 一致性。C 安装消费者直接验证新符号与二次谐波。
本模型的失真产品可作为频谱传播和总 RF 功率计算的输入，但非零 DC 必须先由
显式网络处理，不能直接送入只接受 RF 的功率汇总接口。

2026-09-30 验证：MSVC Debug/Release 全套各 48/48，独立安装消费者各 2/2，
安装 Release 动态库 Python 47 项通过。本次没有新增 SystemVue 实测，厂商
偶次失真及其与独立 P1dB 标定的组合仍待比较；不据此修改已有兼容结果。

## SystemVue 独立器件谐波比较

2026-09-30：新增 −30 dBm 单音采集（sample 参数组，20 dB 增益、IIP2=20 dBm、
IIP3=10 dBm、1 GHz、100 dB 反向隔离），并复用此前 +0.9 dBm 原始采集。
当前 SystemVue 参考实例保留 −30 dBm sample 状态。首次采集发现进程已退出，
连接失败日志保留在 compression-minus30-harmonics-001；确认进程不存在后启动
新实例，成功采集为 build-reference/compression-minus30-harmonics-002。

本机官方帮助 `sim/Node_Measurements.html` 定义 P[NetName] 为各独立频谱的
功率向量；F/ID 为其索引，IDNo/IDName 将不稳定的编号映射到来源和传播路径。
比较脚本 `scripts/reference/compare-single-harmonics.py` 据此选择直接由 RFAmp
生成、仅经过一次 RFAmp 的 H2/H3，排除总谱、噪声谱和反向传播谱。
对于当前 1 Hz 单音，检查谐波的两个边界频率、平坦功率值，再取一个功率值比较，
不重复求和两个绘图端点，不作谱密度积分。其他谱形和带宽尚不支持。

| 输入功率 | H2 相对误差 | H3 相对误差 | 原阈值 1e-7 |
| --- | --- | --- | --- |
| −30 dBm | +5.00005e-8 | +7.50010e-8 | 2/2 通过 |
| +0.9 dBm | +0.3235834 | +0.5227443 | 0/2 通过 |

小信号 H2/H3 的实测功率分别为 2.499999875e-10 W、1.111111028e-13 W，
支持截点标定关系。近压缩点的低阶模型高估失真，说明还需要压缩区的失真生成
模型；不能仅在输出基波上应用 P1dB 压缩后宣称完整 RFAMP 已兼容。
这两份报告只比较谐波，不将内部调用的参数校验/基波比较作为额外通过声明。

报告为 validation 下 `systemvue-2023-single-harmonics-minus30.json` 和
`systemvue-2023-single-harmonics-plus09.json`。精简可重放采集数组为
`systemvue-2023-single-harmonic-captures.json`，保留原始采集散列。复现命令：

```powershell
& 'C:/Program Files/Keysight/SystemVue2023/Python/python/python.exe' `
  scripts/reference/compare-single-harmonics.py build-msvc/Release/rfmodel_c.dll `
  build-reference/compression-minus30-harmonics-002/capture.json `
  build-reference/harmonic-comparison.json --source-power-dbm -30
```

新增 CTest single_harmonic_reference 三项测试验证低功率通过、近压缩点失败、
谱 ID 重编号仍可识别，以及错误身份、谱形、频率或陈旧数据拒绝。Debug/Release
均通过定向测试，现有 CI 的 CTest 步骤自动包含它；本次未改动数值核心。

## 五点功率扫描与输入限制诊断

同日补充 −10、−3、0 dBm 三次新采集，与 −30、+0.9 dBm 构成五点扫描。
所有采集使用相同 sample 参数组及 100 dB 反向隔离，并通过参数、数据新鲜度、
谱身份和谱形校验。扫描后再次采集 −30 dBm，H2/H3 功率与原基准完全一致；
恢复记录位于 build-reference/compression-minus30-harmonics-restored-001。

对于截点模型的 H_n 功率与输入功率 n 次方成正比的关系，分别由 H2、H3 实测值
推导条件性的等效输入功率比例 `(P_SystemVue / P_RFModel)^(1/n)`：

| 输入功率 dBm | 由 H2 推导的比例 | 由 H3 推导的比例 |
| --- | --- | --- |
| −30 | 0.999999975000 | 0.999999975000 |
| −10 | 0.999999975000 | 0.999999975000 |
| −3 | 0.998917072579 | 0.998917072579 |
| 0 | 0.926723119709 | 0.926723119709 |
| +0.9 | 0.869209253449 | 0.869209253449 |

各点两种独立推导的相对差异均小于 1e-12。本机官方帮助
`sim/Gain_Compression_and_Intermod_Generation.html` 说明接近饱和时会限制输入
源音调功率，以限制输出总谱功率；当前观测与共用输入功率限制的解释相符。
但尚未得到独立预测该比例的公式，不能把由实测反推的比例当作已实现模型。
诊断只写入报告 effective_drive_diagnostic，不回馈原生预测，也不改变 1e-7
兼容判据。新增三点报告后缀为 minus10、minus3、zero，精简采集数组已包含五点。

谐波回归新增五点一致性及单独扰动 H3 后一致性失效的检查，共五项 Python 测试。
本机 MSVC Debug/Release 全套 CTest 各 49/49 通过，其中包含这些诊断回归。

## OPSAT 受控扫描

2026-09-30：固定输入 0 dBm、G=20 dB、OP1dB=20 dBm、OIP2=40 dBm、
OIP3=30 dBm、RISO=100 dB，其余参数同 sample，仅改变 OPSAT：

| OPSAT dBm | H2 推导的输入功率比例 | H3 推导的输入功率比例 | 基波判据 | 谐波判据 |
| --- | --- | --- | --- | --- |
| 22 | 0.889813758586 | 0.889813758586 | 2/2 通过 | 0/2 通过 |
| 23 | 0.926723119709 | 0.926723119709 | 2/2 通过 | 0/2 通过 |
| 26 | 0.975705764106 | 0.975705764106 | 2/2 通过 | 0/2 通过 |

23 dBm 点复用前述 0 dBm 输入采集；22、26 dBm 为新采集。逐项验证 16 个参数，
两组新采集相对基准只改变 OPSAT。升高 OPSAT 后谐波抑制减弱，H2/H3 仍支持同一
输入功率比例。当前 P1dB 基波模型保持通过，而截点谐波模型保持失败，说明仅靠
基波通过无法证明失真生成兼容。OPSAT 已被证实是待实现输入限制模型的必要参数；
本次没有将反推比例写入数值核心，也未声称已经得到限制公式。

尝试 OPSAT=30 dBm 时，SystemVue 给出相对 P1dB 过大、建议范围为 21.4–28.1 dBm
的 INFO；28 dBm 时仍给出相对 P1dB 过大的 INFO。采集器继续执行“任何 manager
消息均不纳入参考验收”的现有规则，两次尝试以 collector_failed 结束，未产生
可验收 capture。原始 stderr/status 分别保留在 build-reference 下的
compression-opsat30-zero-001 和 compression-opsat28-zero-001。
这表示参考数据准入未通过，不能据此声称仿真崩溃或数值必然错误。

报告与精简采集分别为 validation/systemvue-2023-harmonic-saturation.json、
validation/systemvue-2023-harmonic-saturation-captures.json，均保留原始采集散列。
采集器的 `--compression-opsat-dbm` 限定 sample 压缩案例及 22/23/26 dBm；
比较脚本用 `--output-saturation-dbm` 显式声明，未声明时仍要求原 23 dBm。
天线参数组不接受此覆盖。复现示例（每次使用新的输出目录）：

```powershell
& 'C:/Program Files/Keysight/SystemVue2023/Python/python/python.exe' `
  scripts/reference/run-systemvue-reference.py compression `
  build-reference/RFModel_AmplifierCompression.wsv build-reference/opsat26-new `
  --source-power-dbm 0 --compression-profile sample --compression-opsat-dbm 26
& 'C:/Program Files/Keysight/SystemVue2023/Python/python/python.exe' `
  scripts/reference/compare-single-harmonics.py build-msvc/Release/rfmodel_c.dll `
  build-reference/opsat26-new/capture.json build-reference/opsat26-new/comparison.json `
  --source-power-dbm 0 --output-saturation-dbm 26
```

比较命令返回 1 表示保留已知谐波差异。采集后已显式恢复 OPSAT=23 dBm、输入
−30 dBm 并复验 H2/H3 与原基准完全一致，记录为
build-reference/compression-opsat23-minus30-restored-001。
本机后台运行器 4 项测试通过；Debug/Release 的独立压缩和谐波 CTest 各 2/2
通过，内部共 10 项 Python 测试，覆盖参数覆盖边界、采集可重放性、缩减趋势及
原预测/阈值未改变。其他平台以对应提交 CI 为准。

后续已加入显式饱和诊断模式，用于保留厂商准确性警告并研究超过 P1dB 的响应；
默认验收仍拒绝警告。候选连续 tanh 模型与三个实测点存在 1.2%–6.2% 差异，
未纳入数值核心，详见 [饱和区诊断](systemvue-saturation-diagnostic.md)。
