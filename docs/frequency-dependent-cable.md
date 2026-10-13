# 频变损耗电缆（C++17）

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

`<rfmodel/frequency_dependent_cable.hpp>` 提供 RFModel 自定义的非磁性同轴电缆工程近似：
常数正实特性阻抗、由相对介电常数给出的恒定传播延迟，以及平方根频率项和线性频率项
组成的单程传播损耗。它计算完整、互易、有失配的二端口复数 S 参数。
此接口为 header-only C++17，链接目标为 `RFModel::rfmodel`，不增加 C ABI、Python 或
JSON/CLI 器件入口。

## 参数和入口

```cpp
#include <rfmodel/frequency_dependent_cable.hpp>
#include <rfmodel/noise_matrix.hpp>

rfmodel::FrequencyDependentCableParameters p;
p.length_m = 2.5;
p.characteristic_ohms = 75.;
p.relative_permittivity = 2.25;
p.reference_frequency_hz = 2e9;
p.sqrt_loss_db_per_m = .6;
p.linear_loss_db_per_m = .15;
p.reference_ohms = 60.;

const double frequency_hz = 1e9;
const double loss_db = rfmodel::cable_propagation_loss_db(frequency_hz, p);
const auto s = rfmodel::frequency_dependent_cable_s_parameters(frequency_hz, p);
const rfmodel::FrequencyDependentCableModel cable("cable", p);
const auto same_s = cable.s_parameters(frequency_hz);
const auto noise = rfmodel::passive_thermal_noise(s, 290.); // 显式开尔文温度
```

`FrequencyDependentCableParameters` 各字段的顺序、单位和默认值为：

| 字段 | 意义 / 单位 | 默认值 |
|---|---|---|
| `length_m` | 电缆长度，m | `1.` |
| `characteristic_ohms` | 线的常数实特性阻抗 Zc，Ω | `50.` |
| `relative_permittivity` | 无量纲相对介电常数 εr | `1.` |
| `reference_frequency_hz` | 调用者指定的损耗系数参考频率 fref，Hz | `1e9` |
| `sqrt_loss_db_per_m` | fref 处平方根项的独立贡献，dB/m | `0.` |
| `linear_loss_db_per_m` | fref 处线性项的独立贡献，dB/m | `0.` |
| `reference_ohms` | 两个端口共同的正实参考阻抗 Z0，Ω | `50.` |

两个损耗系数是 **各自在 fref 处的贡献**，不是分别指定总损耗，也不是每 100 m 的数值。
在 fref 处，单位长度总损耗等于两者之和；参考频率是损耗归一化频率，不是相位零点。
本接口不替调用者判断、转换或拟合厂商数据的单位。

- `cable_propagation_loss_db(frequency_hz, p)` 返回单程传播损耗，单位 dB。
- `frequency_dependent_cable_s_parameters(frequency_hz, p)` 返回 `SMatrix`。
  矩阵索引为 `(出射端口, 入射端口)`，例如 `(1, 0)` 是 S21。
- `FrequencyDependentCableModel(name, p)` 实现 `RFDeviceModel` 和 `SParameterProvider`，
  按值保存参数；名称必须非空。端口编号为 0、1，端口参考阻抗均为 `reference_ohms`。
  `port(2)` 及更大编号抛出 `std::out_of_range`。

## 损耗、延迟和完整 S 参数

令 `x = f/fref`，则

```text
loss_dB(f) = length_m * (sqrt_loss_db_per_m * sqrt(x) + linear_loss_db_per_m * x)
delay_s   = length_m * sqrt(relative_permittivity) / 299792458
p(f)      = 10^(-loss_dB(f)/20) * exp(-j*2*pi*f*delay_s)
r         = (Zc - Z0) / (Zc + Z0)
S11 = S22 = r*(1-p*p) / (1-r*r*p*p)
S21 = S12 = (1-r*r)*p / (1-r*r*p*p)
```

`299792458` 的单位为 m/s；延迟假定相对磁导率为 1。平方根项可用于近似导体型损耗，
线性项可用于近似介质型损耗，但这些名称不代表已完成材料标定。
传播损耗与失配二端口的插入损耗 `-20*log10(abs(S21))` 不同。
当 Zc = Z0 时二者一致；失配时，两端反射与往返传播共同影响幅度和相位。
恒定的传播延迟也不意味着失配二端口的 S21 群延迟处处恒定。

实现按缩放阻抗求反射系数，并使用有界传播因子、稳定的 `1-p*p` 计算和缩放乘积，
避免长有损线双曲函数溢出、很短线的相消以及不必要的中间乘除溢出。
相位先以周期数做象限约简，避免精确半波长处的 `sin(pi)` 舍入残差被高阻抗对比放大。
上述公式是数学定义，不建议调用者直接照式实现极端输入。

## 输入、零值和数值范围

- 频率 f、长度和两个损耗系数必须有限且非负。
- Zc、Z0、εr 和 fref 必须有限且严格为正。NaN 和无穷大不属于有效输入。
- 和已有 `TransmissionLineModel` 一样，若 `1-abs(r) <= 64*epsilon(double)`，
  则阻抗对比已不能可靠分辨，会抛出 `std::invalid_argument`。
- 无效参数及频率抛出 `std::invalid_argument`；即使长度或频率为零，也先验证输入。
- 零长度或 DC 的 S 矩阵精确为 `[[0,1],[1,0]]`，损耗精确为 0。
  这是本工程近似的数学极限，不包含实际导体的直流电阻。
- 零损耗系数给出无损线；两个系数可以独立设为零。默认参数是 1 m、50 Ω 的无损线。
- 损耗辅助函数的最终 dB 值若超过 `double` 有限上限，抛出 `std::overflow_error`。
  S 参数函数不要求先把这个 dB 值实体化；它允许衰减完全不透明的有限输入极限，
  此时 S21 = S12 = 0、S11 = S22 = r。
- S 参数计算仍要求相位周期数 `f*delay_s` 可表示；超出范围会抛出
  `std::overflow_error`，即使衰减已使传输为零也不会跳过这个检查。
  可表示不等于任意大电长度都具有精确相位：双精度仍限制小数周期的分辨率。
- 极小损耗或幅度可能舍入或下溢；本接口不提供任意精度或相对误差保证。

## 噪声组合

电缆本身只提供 S 参数，不实现自动噪声或独立的电缆噪声语义。
对指定物理温度（开尔文），显式调用已有的
`passive_thermal_noise(s, temperature_k)` 得到功率归一化波的相关谱密度，单位 W/Hz。
其关系是 `C = kB*T*(I-S*S^H)`，可保留失配两端之间的噪声相关。

既有 API 的有限值、非负温度和无源性检查及 PSD 重构容差继续适用；温度为零也检查
无源性。尤其接近无损时，低于既有数值容差的耗散/相关量可能被归零，不承诺分辨任意小
损耗对应的热噪声。此功能没有新增温度依赖损耗律，也不把温度当作 εr 或损耗系数修正。
已有的噪声参考变换在极高阻抗对比、近秩一协方差等数值敏感边界可能因 PSD 检查而拒绝；
这属于既有噪声矩阵算法的范围限制，本接口不改变其检查或容差。
详细噪声约定见 [噪声矩阵](noise.md)。

## 可运行的频率扫描和安装消费测试

`examples/frequency_dependent_cable.cpp` 在 0、1 MHz、100 MHz、1 GHz、2.5 GHz 和 6 GHz
输出 CSV。示例使用 2.5 m、εr = 2.25、75 Ω 特性阻抗、60 Ω 端口参考阻抗、2 GHz
系数参考频率和两种非零损耗贡献，输出传播损耗及全部四个 S 参数的实部与虚部。
可选 `--noise` 在每个频点显式增加 290 K 和完整复数噪声相关矩阵。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRFMODEL_BUILD_EXAMPLES=ON
cmake --build build --target frequency_dependent_cable_example
./build/frequency_dependent_cable_example
./build/frequency_dependent_cable_example --noise
```

独立安装消费目标 `frequency_dependent_cable_consumer` 的两个翻译单元均包含公开头文件，
链接安装后的 `RFModel::rfmodel`。它验证默认字段、设备接口、独立 ABCD 解析值、
两种损耗标度、DC/零长度、完全不透明极限、异常和既有热噪声组合。
检查不使用 `assert`，在 Release / NDEBUG 下继续执行。安装步骤见
[安装包使用](installation.md)。这些解析/安装检查不构成厂商仿真对照。

## 工程近似和兼容性边界

本模型没有以下保证或功能：

- 因果宽带色散保证，或由频变衰减一致推导的色散相位；
- 直流导体电阻、材料参数标定、温度定律或从几何结构提取参数；
- 频变/复数特性阻抗、多导体耦合、辐射、接头损耗或任意实际电缆的全频段拟合；
- SystemVue 的 `kdb` 参数单位映射、预置电缆或 RG 型号兼容。

历史电缆资料对每米与每 100 m 的损耗单位存在冲突；本 API 的 dB/m 定义由 RFModel
明确给出，不据此推断任何 SystemVue 参数的换算。当前没有对应的 SystemVue 2023
数值对照证据，不宣称该模型等同厂商 Cable 器件或任何 RG 预置。

若已有单位长度 R、L、G、C，应查看 [RLGC 传输线](rlgc-transmission-line.md)；
若只需常数传播损耗和直接指定延迟，可使用 [均匀传输线](transmission-line.md)。
