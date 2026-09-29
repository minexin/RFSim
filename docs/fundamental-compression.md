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
谐波/互调语义，二者不能混同。C/Python/JSON 的对应调用仍待接入。

模型公式与本机 SystemVue 帮助描述的 P1dB 以下三阶主信号行为一致，四个远低于
P1dB 的实测点支持此假设，但尚未在靠近 P1dB 的隔离器件中完成厂商数值验证。
不改变既有 SystemVue 兼容报告，也不覆盖其高阶专有拟合和饱和算法。

回归检查 P1dB 的实际输出功率及 1 dB 增益下降、复数相位、零输入、小信号极限、
半标定功率、越界、非有限参数，以及已归档两级链路解析结果。独立安装消费者
同时使用此公开头文件验证标定点。

2026-09-29：MSVC Debug/Release 全套各 45/45，独立安装消费者各 2/2。
本轮没有新增 SystemVue 采集；跨平台 CI 仍待核验。
