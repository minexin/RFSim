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
