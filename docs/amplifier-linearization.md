# 公共基波压缩的工作点与导数

C++ 接口 linearize_saturating_amplifier 将既有 SaturatingFundamentalCompression 适配到自动非线性工作点求解器。所有选定驱动端口的 RF 通道共享同一个总功率和压缩增益，并保留跨频点的直接/共轭导数。已提供 C++/C/Python/JSON 接口，可与固定系数双线性混频器在同一物理网络中联立求解。本基波模型不生成高阶项；独立电压多项式模型已接入同一求解器。

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

## C、Python 与 JSON 接口

C ABI 新增 rfmodel_saturating_amplifier_parameters，包含 power_gain_db、output_p1db_dbm、output_saturation_dbm、input_port、output_port 和 include_output_drive。最后一项必须为 0 或 1；三个功率/增益参数的单位分别为 dB、dBm、dBm。独立调用 rfmodel_linearize_saturating_amplifier，返回 A、B 和真实名义输出，输出描述符 rfmodel_conversion_linearization_output 与既有混频输出布局相同。

rfmodel_conversion_network_solve_nonlinear 接受 rfmodel_conversion_nonlinear_model 数组。每项由 device、kind 和 parameters 指针组成：

| kind | parameters 指向的类型 |
|---|---|
| RFMODEL_NONLINEAR_BILINEAR_MIXER | rfmodel_bilinear_mixer_parameters |
| RFMODEL_NONLINEAR_SATURATING_AMPLIFIER | rfmodel_saturating_amplifier_parameters |
| RFMODEL_NONLINEAR_POLYNOMIAL_AMPLIFIER | rfmodel_polynomial_amplifier_parameters |

同一次调用允许混合三类模型；device 索引必须唯一，参数指针在调用期间有效。选项、确定性固定偏置、额外源 C/P、加载噪声和诊断沿用原工作点接口。模型数组、每个参数结构、所有描述符及输入数组均参与完整输出范围的别名检查；失败不修改波、噪声或诊断。原 mixer-only C 函数和结构布局保持不变。

```python
parameters = dict(
    power_gain_db=20.0,
    output_p1db_dbm=20.0,
    output_saturation_dbm=23.0,
)
local = library.linearize_saturating_amplifier(
    1e6, [(0, 1), (1, 1)], [0.01, 0.0], **parameters
)
point = library.solve_conversion_operating_point(
    1e6,
    [dict(channels=[(0, 1), (1, 1)],
          direct=[[0, 0], [0, 0]], source=[0.01, 0])],
    amplifiers=[dict(device=0, **parameters)],
    loaded_noise=True,
)
```

local 是 AmplifierLinearization，包含 direct、conjugate、operating_outgoing 和 output_offset。放大器的仿射偏置必须计算为 d=F(a)-A*a-B*conj(a)，不能使用双线性混频器的 -F(a) 简式。原 MixerLinearization 的字段和行为保持不变。求解的 mixers 与 amplifiers 参数可同时传入；include_output_drive 默认为 False，必须为布尔值。

JSON 使用 rfmodel.conversion-network v1，顶层显式填写 operating_point 对象，设备声明完整 channels、noise 和以下 model：

```json
{
  "type": "saturating_amplifier",
  "power_gain_db": 20,
  "output_p1db_dbm": 20,
  "output_saturation_dbm": 23,
  "include_output_drive": false
}
```

可选 input_port/output_port 默认为 0/1。未知字段、布尔数值、非布尔驱动标志、非法锚点和通道集合均拒绝。输出含真实工作点和收敛诊断；noise_analyses 使用收敛处的完整 A/B，保留幅度方向与相位方向的差异。固定器件噪声由输入显式指定，不根据压缩曲线推断。

两个可运行示例：

- [压缩放大器负反馈](../examples/nonlinear-amplifier-feedback.json)：a=0.2-0.2*F(a)，含反馈侧噪声。回归用独立二分求根及径向/切向闭环增益检查工作点和 C/P。
- [混频器与压缩放大器](../examples/nonlinear-mixer-amplifier.json)：输入滤波、双线性 RF/LO 混频、公共基波压缩，保留共享参考相噪。两个 RF 输出频点共同决定压缩增益；并非新增高阶谱再生模型。

```powershell
python -m rfmodel examples/nonlinear-amplifier-feedback.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/amplifier-feedback-result.json
python -m rfmodel examples/nonlinear-mixer-amplifier.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/mixer-amplifier-result.json
```

## 数值验证

原生专项包括：

- 零驱动、P1dB 两侧、饱和区的径向导数与既有幅度响应中心差分；小驱动耦合、深饱和可表示斜率及 1e300 W 边界。
- 两个 RF 频点与两种驱动端口选择，在低驱动、锚点、饱和区对每个实/虚输入方向做中心差分，验证完整 A/B 矩阵。
- 公共幅度扰动使用径向斜率，公共相位扰动保持输出相位旋转；不同频点之间产生非零 C/P，和独立解析式对照。
- 压缩放大器与线性负反馈网络联立求解；以独立实现的三次/tanh 幅度曲线二分求根对照工作点，再按径向/切向反馈传递解析核对噪声 C/P。
- 非法驱动集合、频率不匹配、非零 DC 等拒绝，安装后的外部 C++ 消费者调用新增接口。

当前证据证明与既有数学模型的一致性。厂商 RFAMP/RFAMP_HO 的完整高阶、来源追踪、AM-PM、真实宽带噪声与网络反馈仍需逐项实现和 SystemVue 比对，不能用基波验证替代。

## C++ 适配阶段验证记录

2026-10-10：MSVC Debug/Release clean-first 构建成功，CTest 各 112/112；两种配置安装后的独立 C/C++ consumer 各 2/2。142 个 C/C++ 文件格式检查及 git diff --check 通过。

在上述 C++ 适配阶段，Python 源码与 C ABI 尚未新增放大器入口。独立 Python 3.12 使用上一阶段 wheel 连接本阶段安装的 Release DLL，13 组共 264 项既有接口回归全部通过（工作点 13、仿射 11、混频线性化 13、共享相噪 13、相噪 13、通道测量 11、加载噪声 9、变频 NF 12、转换网络 13、单器件变频 11、API 85、相干系统 51、线性噪声 9）。该证据用于旧接口兼容，新适配本身由原生专项与安装 C++ consumer 验证。

SystemVue 仍显示既有 Error Running Script 提示，本阶段没有新增厂商实测。

## 跨语言接口阶段验证记录

2026-10-10：MSVC Debug/Release clean-first 构建成功，CTest 各 113/113；两种配置安装后的独立 C/C++ consumer 各 2/2。142 个 C/C++ 文件格式检查及 git diff --check 通过。安装 C consumer 直接调用新增局部导数和带类型模型列表的求解入口。

独立 Python 3.12 从本阶段 wheel 加载接口，连接安装后的 Release DLL，14 组共 278 项回归全部通过：放大器工作点 14、既有工作点 13、仿射 11、混频线性化 13、共享相噪 13、相噪 13、通道测量 11、加载噪声 9、变频 NF 12、转换网络 13、单器件变频 11、API 85、相干系统 51、线性噪声 9。

新增跨语言验证覆盖 P1dB 两侧的复导数、一般仿射偏置、混合器件求解、跨频点 C/P、反射侧驱动选择、解析反馈根及噪声、收敛点 NF、暖启动、严格参数校验、CLI 失败保留已有文件。C ABI 专项增加 22 个失败场景，检查非法类型/参数、模型描述符/参数/初值/输出之间的别名、不收敛及容量边界，所有波/噪声/诊断输出保持原值。绑定格式整理前后，77 个既有 ctypes 函数签名逐项一致，仅增加两个新函数。

本阶段仍未新增 SystemVue 实测；现有语言识别提示及后续执行错误需要单独处理，不能以本地数学回归替代厂商验收。

独立的[高阶电压多项式适配](polynomial-linearization.md)已提供 C++/C/Python/JSON DC/谐波/互调工作点导数。它不改变本页公共基波压缩模型，也尚未组合厂商 RFAMP_HO 限幅与标定规则。
