# 核心接口规范

## 端口与频率

- 频率统一使用 Hz，内部使用 `double`。
- 复数波按端口顺序存储。
- 参考阻抗可以是每端口独立的复数值。

## 能力分离

`RFDeviceModel` 提供名称、端口和初始化；`SParameterProvider` 提供 `S(f)`。噪声、非线性和频率转换分别使用独立 provider，避免把所有模型能力塞进单一基类。

## 求解约定

网络连接由外部 `Network` 描述，求解器不得通过 `dynamic_cast` 判断 PA、Mixer 或 Filter。频率扫描输出必须包含频率、端口波、功率和诊断信息。
