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
