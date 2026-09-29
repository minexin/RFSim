# 分布参数 RLGC 传输线

`rfmodel/rlgc_transmission_line.hpp` 提供 `RlgcTransmissionLineModel`，实现
`RFDeviceModel` 和 `SParameterProvider`。它从单位长度串联电阻、电感、并联电导、电容
计算均匀传输线的完整二端口 S 参数，可接入网络、频率扫描和被动热噪声 API。

```cpp
rfmodel::RlgcPerLength per_metre{
    7.,       // R: ohm/m
    400e-9,   // L: H/m
    0.002,    // G: S/m
    120e-12   // C: F/m
};
rfmodel::RlgcTransmissionLineModel cable("cable", per_metre, 0.7, 50.);
auto s = cable.s_parameters(1e9);
```

构造参数依次为名称、RLGC 系数、长度（米）、端口参考阻抗（欧姆，默认 50）。
两个端口编号为 0、1，共用正实数参考阻抗。长度与四个系数均为有限非负值，
允许零值及零长度；频率使用非负有限 Hz。

## 计算与极限

令 `z=R+j*omega*L`、`y=G+j*omega*C`，标准均匀线的 ABCD 矩阵为
`A=D=cosh(gamma*l)`、`B=z*l*sinh(gamma*l)/(gamma*l)`、
`C=y*l*sinh(gamma*l)/(gamma*l)`，其中 `gamma=sqrt(z*y)`，l 为长度。
实现使用归一化总串联量 `a=z*l/Z0`、总并联量 `b=y*l*Z0` 和
`x=sqrt(a)*sqrt(b)`，以衰减因子 `exp(-x)` 缩放整个转换，避免长有损线的
`cosh`/`sinh` 溢出。小 x 用级数计算 `sinh(x)/x`，包含 x=0 的极限。

这同时覆盖直流纯电阻、纯电导、无损 LC、全零参数和零长度直通，不通过除以
零频特性阻抗处理这些情况。中间归一化量超出双精度范围时显式报错；极大电长度
仍存在双精度相位精度限制。无源热噪声使用 `passive_thermal_noise(s, temperature_k)`
显式组合，温度单位为开尔文。

## 验证和边界

回归包括与现有无损阻抗失配线一致、直流串/并联解析值、满足 R/L=G/C 的无畸变线
解析衰减和延迟、一般有损线两段级联一致性、热噪声无源性检查、百万米长线衰减极限，
以及非法输入。安装包消费项目检查公开头文件和直流传输。

R、L、G、C 本身当前为常数；频率响应和复数特性阻抗由分布方程产生。还没有引入
频变 R/G、趋肤效应、介质频散、几何尺寸到 RLGC 的提取或多导体耦合矩阵。
SystemVue 2023 对应传输线模型的参数映射及仿真对照仍未完成，此实现不代表完整
电缆、微带或带状线器件的兼容验收。

2026-09-29 本地验证：MSVC Debug/Release 全套 CTest 各 40/40；安装包消费测试各 1/1；
72 个 C++ 文件通过格式检查。此提交的 Linux/macOS CI 仍需在推送后核验。
