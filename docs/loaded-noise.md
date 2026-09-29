# 外部边界反馈下的噪声

`rfmodel/loaded_noise.hpp` 提供 `loaded_noise(scattering, intrinsic, reflections,
boundary_emission)`，用于已提取的 N 端口网络或单个器件。
所有端口共用正实参考阻抗的功率波约定，与现有 S 参数和 NoiseCorrelation 一致。

输入包含完整 S 矩阵、内生噪声 c 的协方差、各外部端口反射系数，以及边界发射噪声 e
的协方差（均为 W/Hz）。内生噪声和边界噪声假定相互独立；每个向量内部可以包含
跨端口复数相关。反射系数须有限且模不超过 1，允许完全反射；奇异或病态反馈回路报错。
主动器件 S 矩阵允许存在，但不提供宽带稳定性保证，边界不支持有源反射系数。

```cpp
const rfmodel::SMatrix pad{2, {0., 0.5, 0.5, 0.}};
const auto intrinsic = rfmodel::passive_thermal_noise(pad, 290.);
const std::vector<rfmodel::Complex> gamma{{0.2, 0.1}, {-0.3, 0.2}};
const auto emitted = rfmodel::thermal_boundary_noise(gamma, {290., 0.});
const auto result = rfmodel::loaded_noise(pad, intrinsic, gamma, emitted);
const double load_absorbed_noise = -result.net_into_device_w_per_hz[1];
```

## 方程及功率方向

采用 `b=S*a+c`、`a=Gamma*b+e`，Gamma 为对角反射矩阵。
令 H=`(I-S*Gamma)^-1`、J=`H*S`，则 b=`H*c+J*e`，
a=`Gamma*H*c+(I+Gamma*J)*e`。分别传播两种独立协方差后相加。
入射噪声不能简单计算成 `Gamma*C_b*Gamma^H+C_e`，因为 e 与反馈后的 b 相关；
实现保留直接发射和返回波之间的相干项。

结果 incident 和 outgoing 是完整端口噪声协方差。
net_into_device_w_per_hz[p]=C_a[p,p]−C_b[p,p]，正值表示流入器件，
负值表示器件向边界净输出。边界本身有温度时，此净值已经扣除边界向器件发射的功率，
不应称为单独的接收噪声；需要出射噪声时读取 outgoing 对角项。

`thermal_boundary_noise(reflections, temperatures_k)` 为独立被动单端口终端生成
对角协方差 `k*T*(1-|Gamma|²)`。温度单位 K，可为 0；完全反射端不发射热噪声。
相关源应直接提供完整 boundary_emission，不能以此独立热终端助手代替。

## 验证与剩余范围

测试覆盖同温匹配衰减器热平衡、无噪声负载下与既有线性链路噪声求解一致、
含复杂反射的单端口解析解、无损直通的冷热终端净功率守恒、复相关边界噪声、
完全反射终端、奇异反馈和非法协方差。安装消费者直接调用新头文件。

现已提供 C ABI 和 Python 调用，见下节。JSON 的 signal_boundaries 尚未调用它，
原 JSON noise_w_per_hz 仍保留匹配参考条件。没有加入内生与边界交叉相关、
非对角连接边界、频率转换噪声或自动噪声系数换算。SystemVue 实测仍待完成。

2026-09-29 本地验证：MSVC Debug/Release 全套各 43/43，最终热边界舍入保护变更
后额外重跑 Debug 专项通过；安装消费者两配置各 2/2，78 个 C/C++ 文件通过格式检查。
本提交跨平台 CI 仍待核验。

## C 和 Python 调用

C 的 rfmodel_loaded_noise 接受 N×N 的 scattering、intrinsic、boundary_emission，
及 N 个复数 reflections，写入 N×N incident/outgoing 和 N 个净流入功率。
矩阵逐行排列、单位 W/Hz；value_count 必须等于 N²，反射数量必须等于 N。
两个矩阵输出共用 matrix_capacity 参数，功率缓冲区使用 power_capacity；
全部输出指针必须有效且彼此不重叠。所有尺寸和求解验证通过后才写结果，失败不写输出。
rfmodel_thermal_boundary_noise 接受 N 个反射和 N 个温度，输出 N×N 边界噪声矩阵。

Python 示例（library 为已加载的 rfmodel.Library）：

```python
s = [[0, 0.5], [0.5, 0]]
intrinsic = library.passive_noise(s, 290.)
gamma = [0.2+0.1j, -0.3+0.2j]
emitted = library.thermal_boundary_noise(gamma, [290., 0.])
result = library.loaded_noise(s, intrinsic, gamma, emitted)
load_absorbed = -result.net_into_device_w_per_hz[1]
```

返回 LoadedNoise 命名元组，incident/outgoing 为嵌套复数元组，净功率为实数元组。
参数尺寸在 Python 与 C 两层校验；反射无源性、协方差和反馈可解性由 C++ 验证。
新增符号保持 ABI 1，Python 包需配套包含这些符号的动态库。

2026-09-29 C/Python 扩展验证：MSVC Debug/Release 各 43/43，安装消费者各 2/2；
安装后 Release 动态库通过二十六项 Python 测试。新增 C 短缓冲区保留输出、
单端口解析解、热发射及 Python 热平衡/复相关/错误尺寸测试。本次跨平台 CI 尚待核验。
