# 双向小信号放大器

`include/rfmodel/amplifier_model.hpp` 提供 `LinearAmplifierParameters`、`LinearAmplifierModel` 和 `reflection_from_impedance`。它是后续 RFAMP 适配使用的小信号构件，尚不代表完整 RFAMP 兼容。

## 参数与接口

公共接口继承 RFDeviceModel 和 SParameterProvider，可直接把 `s_parameters(f)` 加入 LinearNetwork。输入、输出阻抗均相对于同一个正实数参考阻抗定义。

| 参数 | 单位 / 默认值 | 含义 |
|---|---|---|
| gain_db | dB / 20 | 20 log10 abs(S21) |
| gain_phase_degrees | 度 / 0 | S21 相位 |
| reverse_isolation_db | dB / 50 | -20 log10 abs(S12)，允许正无穷表示单向 |
| reverse_phase_degrees | 度 / 0 | S12 相位 |
| input_impedance_ohms | 复数 Ω / 50 | 另一端口参考匹配时的输入阻抗 |
| output_impedance_ohms | 复数 Ω / 50 | 另一端口参考匹配时的输出阻抗 |
| reference_ohms | Ω / 50 | 共同正实数参考阻抗 |

`S11=(Zin-Zref)/(Zin+Zref)`，`S22=(Zout-Zref)/(Zout+Zref)`。正负相位采用复指数约定；行是出射端口，列是入射端口。阻抗换算先缩放以避免有限大阻抗相加溢出；Z=-Zref 明确报告奇异。可以表达主动端口，不自动保证网络稳定性。NaN、非法参考、负反向隔离及增益溢出均报错；负无穷前向增益表示零传输。

```cpp
rfmodel::LinearAmplifierParameters parameters;
parameters.input_impedance_ohms = {50.0, 10.0};
parameters.output_impedance_ohms = {75.0, -5.0};
parameters.reverse_isolation_db = 40.0;
rfmodel::LinearAmplifierModel amplifier("LNA", parameters);
rfmodel::LinearNetwork network(parameters.reference_ohms);
network.add(amplifier.s_parameters(1e9), parameters.reference_ohms);
network.terminate(0, {}, 1.0);
network.terminate(1);
const auto waves = network.solve();
```

## 基准依据与边界

SystemVue 2023 本机帮助 `sim/Gain_and_Impedance.html` 明确区分模型增益（S21）与路径测量增益（工作功率增益）。`sim/Spectrasys_Port_Parameter_Types.html` 的公式图确认 S11/S22 是另一端参考匹配时的反射系数，S12/S21 是对应行列的波传输比。本实现按该定义构造小信号矩阵，不额外乘入失配修正。网络求解通过反射反馈计算实际信号；例如匹配负载、S11=1/3、S21=2 时，工作功率增益是 4.5，abs(S21)^2 是 4。

目前矩阵跨频率恒定，包括 DC。RFAMP 的 DC 阻断、频率参数数组、端口表示选择、噪声、温度、压缩、AM/PM 与相位噪声仍需在后续模型适配中完成。增益相位和反向相位是这里显式提供的数学参数，不宣称它们与厂商所有参数输入格式一一对应。

`sim/Spectrasys_Noise_Parameters.html` 同时使用归一化导纳/噪声电阻的定义和带 Zref 的默认值表述，不能在未验证单位的情况下直接转换为协方差默认值。因此本类不实现 NoiseCorrelationProvider；不把“缺少噪声模型”隐式解释成厂商 NF 已支持。

## 验证

失配功率增益的公共测量接口已补充在 [power-gain.md](power-gain.md)，可以对本模型及一般二端口 S 矩阵分别计算三种增益。

2026-09-24，MSVC Debug 与 Release 全套各 32/32 通过，58 个 C++ 文件格式检查通过。新增用例覆盖默认参数、有限 S12 的源/负载反射反馈解析解、复数阻抗、相位周期、单向极限、极大阻抗缩放及错误输入。尚未执行 SystemVue 数值对照，也不据此声称 Linux/macOS 本提交已验证。
