# 总体架构

```text
System Description -> Network Builder -> Linear RF Solver
                                      -> RFDeviceModel
                                      -> SParameterProvider
                                      -> model data adapters
```

器件负责提供行为数据；求解器负责端口连接、网络方程、频率扫描和结果汇总。噪声、非线性、频率转换作为独立能力接口加入，不强制所有器件实现。

M1 以散射参数为统一网络表示，所有端口显式携带参考阻抗。模型数据与求解过程分离，后续可接入 JSON、Touchstone、测量数据和电磁仿真结果。
