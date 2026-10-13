# 变频网络的逐通道复功率波参考

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

原生 C++17 的固定泵转换模型和多器件网络支持逐通道参考阻抗。直接矩阵 A、共轭矩阵 B、仿射偏置、边界源以及完整 C/P 噪声均保留各自坐标；内部连线按电压连续和电流守恒求解。本页扩展[转换矩阵](frequency-conversion.md)和[多器件网络](conversion-network.md)的原生接口。

C/Python/JSON 已提供显式逐通道参考入口，见下方跨语言契约。原生自动非线性工作点新增 [solve_conversion_operating_point_power_waves](conversion-operating-point-power-wave.md)；既有工作点入口及跨语言自动求解继续要求公共实参考。固定泵坐标变换不完成实际 LO 工作点、厂商 ZPORT/CS、通用器件 DC 偏置或 SystemVue 路径预算验收。

## 模型与通道坐标

```cpp
#include <rfmodel/conversion/conversion_power_wave.hpp>
#include <rfmodel/conversion/conversion_network.hpp>

rfmodel::FrequencyConversionModel local(
    spacing_hz, channels, direct, conjugate,
    std::vector<rfmodel::Complex>{{31., 12.}, {73., -21.}});
```

向量按 `channels` 排列，长度为 N，每项有限且实部严格为正。同一物理端口的不同频率可以采用不同参考。DC 通道只允许正实参考，以保持实电压、电流和 DC 波；拒绝用复 DC 坐标隐含增加虚自由度。原有 1..512 通道、非负频率、端口/频率唯一性和矩阵校验继续适用。

此构造函数中的 A/B 已在所声明坐标下，不会自动重归一化。`reference_impedances_ohms()` 返回全部实际参考；`reference_ohms()` 仅在全部通道使用相同正实参考时返回该数值，否则报错。原标量构造函数和默认 50 Ω 行为保留。

`FrequencyConversionNetwork::add_power_waves(model)` 和 `add_power_waves(device)` 显式接受局部参考。网络构造时的标量参考只约束既有 `add`，不会覆盖显式添加器件的坐标。旧 `add` 仍拒绝不同公共参考或混合参考模型。网络的 `reference_impedances_ohms()` 返回按器件添加顺序、器件内通道顺序排列的参考；输出波和 C/P 采用同一顺序。

```cpp
rfmodel::FrequencyConversionNetwork graph(spacing_hz);
graph.add_power_waves(first);
graph.add_power_waves(second);
graph.connect(0, 1, 1, 0);
auto result = graph.analyze(true); // 包括加载后的入射/出射/互噪声和净功率
```

物理端口连接仍要求双方频点集合完全相同，连接失败不能占用部分频点。已连接通道必须没有边界源、反射或边界噪声；器件内生噪声和确定性输出偏置可继续存在。

## 物理连线

采用 RMS 功率波：

```text
a = (V + Z I) / (2 sqrt(Re Z))
b = (V - conj(Z) I) / (2 sqrt(Re Z))
V = (conj(Z) a + Z b) / sqrt(Re Z)
I = (a-b) / sqrt(Re Z)
```

对相连通道 p、q，电流均朝向各自器件，要求 Vp=Vq、Ip=-Iq。边界 a=K b 的非零系数为：

```text
D   = conj(Zp+Zq)
Kpp = (conj(Zq)-Zp) / D
Kqq = (conj(Zp)-Zq) / D
Kpq = Kqp = 2 sqrt(Re Zp Re Zq) / D
```

同一公共实参考时退化为直接交换两端的波。实现先缩放阻抗及平方根，避免直接求和/相乘溢出；普通线性网络和变频网络共用这一物理连线系数。变频求解的确定性方程、入射噪声及入射—出射互协方差均使用完整 K，不能只修改出射信号的计算。

净输入功率仍为 `|a|²-|b|²=Re(V conj(I))`，噪声净功率仍为 Caa(ii)−Cbb(ii)。完整 C/P、跨频率相关性和器件间传播后产生的相关项保留。参考温度实验沿用显式外部热端口、正频率参考带和被动边界条件；不添加隐含泵噪声。

## 直接/共轭矩阵、偏置及内生噪声重归一化

```cpp
auto change = rfmodel::renormalize_power_waves(model, new_references);
auto offset_new = rfmodel::renormalize_conversion_offset(change, offset_old);
auto noise_new = rfmodel::renormalize_conversion_noise(change, noise_old);
```

返回 `ConversionPowerWaveRenormalization`，包含新 `model` 以及 `noise_direct`、`noise_conjugate`。旧出射偏置或内生随机波 c 经后两者变为 Uc+V conj(c)。不能只把 A 当作 S 变换后原样保留 B，也不能只传播 C 而丢弃 P。

算法在实正交分量中处理完整关系 b=M a+c。若坐标变换写成 a'=U a+V b、b'=W a+X b，则利用行列式为一的逆坐标关系 a=X a'−V b'、b=−W a'+U b'，求解：

```text
H  = U + M V
F  = W + M X
H M' = F
H N  = Identity
c' = N c
Q' = N Q transpose(N)
```

U/V/W/X 此处均指对应的实正交分量矩阵。直接求解 H 得到新模型和噪声传递系数，避免等价公式 `N=X−M'V` 在大阻抗比下发生相消。普通线性 S 重归一化的噪声传递也改为解 `H=A+S B`。两类变换都不要求器件物理 Z/Y 可逆；理想直通也可变换。最终重新拆出 A'/B' 和噪声直接/共轭传递系数。奇异或不可表示的坐标变换报错，不更改原模型或输入。

DC 的虚输入/输出坐标被显式排除。因为 `conj(a_DC)=a_DC`，DC 输入对应 A/B 列的拆分不唯一：往返变换保证物理关系，而非两个矩阵逐项文本相同。参考完全不变时直接返回模型副本和恒等噪声变换，保留原系数表示。正频率 RMS 与实 DC 的 sqrt(2) 混频约定保持原契约。

## 保留实际源/负载边界

改变模型参考后，还必须按同一物理源/负载解释外部边界。原来反射为零不代表在新坐标下仍为零。

```cpp
auto boundary = rfmodel::renormalize_conversion_boundary(
    model, new_references, external_channels,
    source, reflection, source_noise);
```

函数返回新 `source`、`reflection` 和 `source_noise`。`external_channels` 是当前模型的局部通道索引，必须唯一且在范围内。未选通道的原源、反射及边界 C/P 行列须全零，结果继续为零，供内部连线使用。空选择只接受全零边界。内部器件噪声使用前述独立函数转换。

逐通道原边界为 a=Γb+s+e，坐标关系为 a'=u a+v b、b'=w a+x b，则：

```text
Γ' = (u Γ+v) / (w Γ+x)
L  = u - Γ' w = 1 / (w Γ+x)
s' = L s
C'ij = Li Cij conj(Lj)
P'ij = Li Pij Lj
```

实现用缩放后的倒数求 L，避免两个大数相减。1 Ω 与 1e-24..1e24 Ω 的实/复参考回归按独立 Thevenin/Norton 公式检查传递、偏置和 C/P，要求相对误差低于 1e-12；这不保证病态的完整反馈网络可解。

同样拒绝奇异变换、非有限输入、复 DC 源/反射和无效 C/P。跨通道/跨频率边界源相关性得到共同变换。该辅助函数作用于给定模型的通道；网络的 `additional_source_noise` 仍须预先使用全局通道的局部坐标。调用方应对全局矩阵应用同样的 Li/Lj 变换，不能分别处理器件时清掉原有跨器件相关块。

## 线性样本接入

`lift_linear_conversion(spacing, bins, samples, noise, references_by_sample)` 新增第五参数向量重载。每个频点有一项端口参考向量，S 和噪声都已在这些局部坐标下；通道按频点优先、端口次序展开。函数不隐式重复转换 S 或噪声。DC 噪声保持 P=C，正频率平稳噪声保持 P=0。原公共标量参考重载保持可用。

## 可重跑示例与验收范围

[原生混频—热电阻示例](../examples/conversion/mixed_reference_conversion.cpp)将 0.8 GHz 信号用 1 GHz 固定泵转换至 0.2/1.8 GHz，再经串联 25 Ω 电阻送至 50 Ω 负载。七个通道采用不同复参考，命令行可将局部参考整体乘以正数；物理源、负载、器件和泵不变。

```powershell
cmake --build build-msvc --config Release --target mixed_reference_conversion_example
build-msvc/Release/mixed_reference_conversion_example.exe 1 > build-reference/mixed-reference-conversion.json
python scripts/verify-conversion-power-wave-example.py build-reference/mixed-reference-conversion.json
python scripts/verify-conversion-power-wave-example.py --executable build-msvc/Release/mixed_reference_conversion_example.exe
```

独立校验器不导入 RFModel，直接用实数混频的傅里叶系数、串联电阻 V/I 和热噪声公式验证输出。它检查 0.5、1、2 三组坐标、完整重放、非法缩放，以及被篡改的波、互补噪声、电压和净噪声。每个边带的输出噪声为 `7.681242272e-21 W/Hz`；两边带有非零互补相关，不能用互不相关的两份输出噪声替代。

原生专用回归另外从含直接/共轭跨频率导纳的独立 V/I 方程建立十二通道双器件网络，使用独立实高斯消元器核对波、偏置、完整出射/入射/互噪声和内部功率守恒。覆盖实 DC、双向转换、Norton 内生噪声、跨器件边界相关源、参考往返、被动参考温度实验、奇异 Z 的直通及非法输入。

原生阶段历史[机读验证](../validation/conversion-power-wave-validation-20261012.json)分别记录原生全量测试、安装消费者、既有 wheel、旧 C 二进制和独立示例证据。该原生阶段的 C API 声明和实现文件、Python 包均未改变；新 C++ 类型/头文件需一起重新编译。远端验证以对应提交 CI 为准。厂商兼容状态和既有失败报告不因这些数学/工程回归通过而升级。


## 跨语言显式参考接口（2026-10-13）

新增入口不改变 ABI 1 既有结构布局或旧函数签名；旧标量接口仍执行其公共正实参考检查。新 Python 包需要配套的新原生库，ABI 数字相同不保证旧库含新增符号。

### C

`rfmodel_conversion_references` 为 `{count, values}`，每个器件的向量覆盖其全部局部通道。新增 `rfmodel_conversion_analyze_power_waves` 和 `rfmodel_conversion_network_analyze_power_waves` 接受这些描述符，支持可选仿射偏置、额外全局相关源 C/P 及加载噪声输出。新增 `rfmodel_conversion_network_noise_analysis_power_waves` 执行相同局部坐标中的参考温度实验。

这些显式入口使用向量参考，忽略旧 request 中的 `reference_ohms`；A/B、偏置、所有源和内生 C/P 已位于调用者声明的坐标。网络仍按器件/局部通道顺序排列全部波、噪声和参考。调用者持有输入的实际参考向量，无需从旧标量字段猜测坐标。

`rfmodel_conversion_renormalize_power_waves` 输出新的 A/B、内生噪声 C/P、仿射偏置和直接/共轭噪声传递矩阵；`rfmodel_conversion_renormalize_boundary_power_waves` 输出选定外部通道的新源、反射及边界 C/P。两者是原生坐标变换的薄接口，不改变输入。

所有声明的输出容量范围须彼此独立，并与所有输入数组、参考描述符及其数组、请求和输出描述符不重叠。新结构字段参见公共头文件；失败时所有输出保持不变。参考数量、非有限值、非正实部、复 DC 参考、错误外部通道和不可解变换都会失败。

### Python

```python
result = library.frequency_conversion(
    spacing_hz, channels, direct, conjugate,
    reference_impedances_ohms=[31+12j, 73-21j],
    source=source, reflection=reflection,
)

result = library.conversion_network(
    spacing_hz,
    [dict(channels=channels, direct=direct, conjugate=conjugate,
          reference_impedances_ohms=[31+12j, 73-21j])],
    loaded_noise=True,
)
```

显式参考的结果提供 `reference_impedances_ohms` 元数据；未使用此参数时保留既有结果元组和默认行为。`None` 表示未指定；混合网络中未给向量的器件使用调用的公共标量参考，显式向量优先于该标量。JSON 字段存在时必须是数组，`null` 不表示省略。`conversion_noise_analysis` 接受同样的器件描述。

`renormalize_conversion_power_waves(spacing_hz, channels, direct, old_references, new_references, conjugate=None, ...)` 返回新 direct/conjugate、noise_direct/noise_conjugate、output_offset、intrinsic_covariance/intrinsic_complementary 及新参考。可选输入为 output_offset、intrinsic_covariance 和 intrinsic_complementary。

`renormalize_conversion_boundary(spacing_hz, channels, old_references, new_references, external_channels=..., ...)` 返回 source、reflection、source_covariance/source_complementary 和新参考。边界输入均为可选零值；external_channels 必须显式给出。内部全局跨器件 C/P 仍由调用者按完整坐标转换，不能拆成独立块而丢失相关项。

### JSON 坐标与限制

`rfmodel.frequency-conversion` version 1 可在顶层加入 `reference_impedances_ohms`；`rfmodel.conversion-network` version 1 在每个 device 加入同名字段。值为覆盖全部通道的复数数组，复数使用已有 `[real, imag]` 编码；线性提升器件按频点优先、端口次序展开。结果在显式参考路径标明实际逐通道参考，并将原标量输出字段改名为 `construction_reference_ohms`，避免误称所有波都以该标量为参考；原公共参考默认输出保持不变。

- 显式 matrix A/B、linear.s 和显式噪声矩阵已处于所声明的局部坐标，不重复转换。
- 参数生成的理想混频器、线性及 Touchstone 模型先按公共标量参考构建，再调用原生接口转换 A/B；模型生成的内生热噪声随模型一起转换。显式内生 C/P 始终是局部坐标。
- 边界 source/reflection、source_noise、additional_source_noise 和显式 output_offset 始终由调用者给为局部坐标。改变参考时不能照搬旧边界；可用上述边界辅助函数生成等价输入。
- 显式向量参考暂不与 automatic operating_point、frequency_plan、linearized_real_mixer、phase_noise_sources 或 phase_noise_groups 组合。解析器明确拒绝这些组合，避免频点扩充/重排或载波侧带计算后错误地沿用参考；公共实参考路径仍保留。

可重跑的七通道 JSON 案例见[混合参考混频—热电阻网络](../examples/mixed-reference-conversion.json)，包含加载噪声和参考温度分析；专用测试以独立傅里叶、物理串联电阻和热噪声公式核对三个参考尺度的全部波与 C/P。

上述跨语言接口及数学/工程验证不增加 SystemVue 厂商兼容通过项，也不扩展跨语言自动非线性混合参考、实际 LO 求解或未知厂商语义。原生混合参考自动工作点的新入口另见[专门契约](conversion-operating-point-power-wave.md)。历史原生验收记录保留；本次验证单独记录，不借用历史 Windows/macOS 结果。

边界变换同时修正数值奇异检测：以缩放后的两个分母项之和为相对尺度，消去残差不超过 `64·epsilon` 时拒绝，未使用绝对阈值。50→100 Ω、Γ=3 在多个共同比例尺度下均报错；Γ=3+1e-8 的可解相邻配置仍接受。该检查避免把精确数学极点的舍入残差当作有效分母，但不保证所有病态反馈网络具有良好条件数。

本轮本地 Linux Debug/Release、安装 SDK、新旧 wheel/旧 C 客户端及格式验证见[跨语言机读记录](../validation/conversion-power-wave-api-validation-20261013.json)。当前未执行 Windows/macOS 或远程 CI，厂商兼容状态保持不变。
