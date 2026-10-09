# 公共基波压缩的工作点与导数

C++ 接口 linearize_saturating_amplifier 将既有 SaturatingFundamentalCompression 适配到自动非线性工作点求解器。所有选定驱动端口的 RF 通道共享同一个总功率和压缩增益，并保留跨频点的直接/共轭导数。该阶段提供 C++ 适配与安装 SDK；放大器专用 C/Python/JSON 入口及高阶生成模型尚未接入。

## 既有模型与导数

设选定驱动功率 P=sum(abs(a_j)^2)，r=sqrt(P)，公共实增益为 g(P)，输出频点 i 的复波为 b_i=g(P)*a_i。沿用现有模型：P1dB 以下是三次基波压缩，以上是与锚点幅度和斜率连续的增量 tanh 饱和。没有新增拟合系数或修改旧 amplitude_gain/transmit_fundamental 的行为。

P1dBFundamentalCompression 与 SaturatingFundamentalCompression 新增 gain_response(P)，返回：

| 字段 | 含义 |
|---|---|
| amplitude_gain | 公共幅度增益 g；相位方向的切向斜率 |
| radial_amplitude_slope | 沿公共幅度方向的 d(r*g(r*r))/dr |
| radial_gain_difference | 2*P*dg/dP，即径向斜率减去 g |

低驱动时独立计算 radial_gain_difference，避免相近斜率相减丢失跨频点耦合。饱和段用 4*exp(-2u)/(1+exp(-2u))^2 计算 sech(u)^2，避免 tanh(u) 已舍入为 1 时过早丢掉仍可表示的小斜率。

对于驱动通道 j，令 D=radial_gain_difference，直接/共轭矩阵为：

```
A_ij = g*delta_ij + (D/2)*(a_i/r)*conj(a_j/r)
B_ij =            (D/2)*(a_i/r)*(a_j/r)
```

不属于驱动集合的通道没有功率耦合项。P=0 时使用精确小信号极限：A 为同频前向增益，B=0。实现使用归一化复波，避免先除以极小功率再乘波幅导致不必要的溢出。

这是一阶导数对既有有限频点公共功率模型的适配。它不是完整时域无记忆多项式或谐波平衡模型，不能凭这些导数宣称已覆盖任意 AM 边带、谱再生或高阶噪声混频。

## 端口和频率契约

输入和输出端口默认 0/1，可显式指定；通道集合只允许这两个端口，二者 bin 集合必须一致且至少有一个正频 RF bin。所有波使用共同正实参考阻抗，频率由 spacing_hz*bin 指定。DC 通道可以存在但波必须为零，不计入驱动、没有输出或导数；非零 DC 明确拒绝。

默认 drive_ports 只包含输入端口。显式传入 {input_port,output_port} 可以让输出侧入射的 RF 波也参与共同压缩，与既有多端口总驱动计算方式衔接。驱动集合必须包含输入端口，不允许重复或无关端口；这保证选定传输基波的功率确实包含在总功率中。是否让输出侧驱动压缩是调用者明确选择的模型约定，尚未获 SystemVue 验收。

匹配、单向的大信号模型仅在输出端口产生同频基波，输入端口出射为零。工作点求解仍处理外部线性网络与反射；当输出侧参与驱动时，相应输入波还会通过公共压缩影响所有前向输出。

## C++ 接口

```cpp
#include <rfmodel/amplifier_linearization.hpp>
using namespace rfmodel;
const SaturatingFundamentalCompression amplifier(20., 20., 23.);
const std::vector<ConversionChannel> channels{
    {0, 11}, {0, 13}, {1, 11}, {1, 13}
};
auto local = linearize_saturating_amplifier(
    1e6, channels, {.01, .02, 0., 0.}, amplifier);
// local.jacobian 是 FrequencyConversionModel；local.outgoing 是真实基波输出。
ConversionNonlinearDevice nonlinear{
    0, [=](const std::vector<Complex>& incident) {
        return linearize_saturating_amplifier(1e6, channels, incident, amplifier);
    }
};
// 将 nonlinear 和同通道的 ConversionDevice 交给 solve_conversion_operating_point。
```

返回类型为通用 ConversionLinearization，可直接用作器件回调。模型参数在迭代中固定。通道噪声仍由 ConversionDevice 的 source_noise/intrinsic_noise 提供；新增适配不凭压缩曲线推断噪声系数或工作点相关器件噪声。

## 数值验证

原生专项包括：

- 零驱动、P1dB 两侧、饱和区的径向导数与既有幅度响应中心差分；小驱动耦合、深饱和可表示斜率及 1e300 W 边界。
- 两个 RF 频点与两种驱动端口选择，在低驱动、锚点、饱和区对每个实/虚输入方向做中心差分，验证完整 A/B 矩阵。
- 公共幅度扰动使用径向斜率，公共相位扰动保持输出相位旋转；不同频点之间产生非零 C/P，和独立解析式对照。
- 压缩放大器与线性负反馈网络联立求解；以独立实现的三次/tanh 幅度曲线二分求根对照工作点，再按径向/切向反馈传递解析核对噪声 C/P。
- 非法驱动集合、频率不匹配、非零 DC 等拒绝，安装后的外部 C++ 消费者调用新增接口。

当前证据证明与既有数学模型的一致性。厂商 RFAMP/RFAMP_HO 的完整高阶、来源追踪、AM-PM、真实宽带噪声与网络反馈仍需逐项实现和 SystemVue 比对，不能用基波验证替代。

## 工程验证记录

2026-10-10：MSVC Debug/Release clean-first 构建成功，CTest 各 112/112；两种配置安装后的独立 C/C++ consumer 各 2/2。142 个 C/C++ 文件格式检查及 git diff --check 通过。

Python 源码与 C ABI 本阶段没有新增放大器入口。独立 Python 3.12 使用上一阶段 wheel 连接本阶段安装的 Release DLL，13 组共 264 项既有接口回归全部通过（工作点 13、仿射 11、混频线性化 13、共享相噪 13、相噪 13、通道测量 11、加载噪声 9、变频 NF 12、转换网络 13、单器件变频 11、API 85、相干系统 51、线性噪声 9）。该证据用于旧接口兼容，新适配本身由原生专项与安装 C++ consumer 验证。

SystemVue 仍显示既有 Error Running Script 提示，本阶段没有新增厂商实测。
