# RF/LO 工作点与双线性混频器小信号模型

本阶段为显式给定 RF/LO 工作点的理想双线性实乘法器建立增量 A/B 矩阵，同时保留名义输出。RF 输入噪声、LO 噪声和两者的共享参考相关性现在可以通过同一个增量模型传播。模型可作为普通转换器件接入已有物理连接网络；JSON/CLI 提供一个工作点明确的单混频器分析入口。

## 定义与归一化

输入和输出均使用共同正实参考阻抗下的 RMS 功率波。对应的实波形定义为 x(t)=x0+sqrt(2)*Re(sum(xk exp(j k wt)))，正频率复数 xk 的平方模为瓦特，DC x0 为实数。所建模的乘积为：

~~~text
y(t) = sqrt(2) * k * x_RF(t) * x_LO(t)
g = 10^(gain_db/20)
k = g / abs(LO_operating_wave)
~~~

k 在工作点确定后保持不变；LO 扰动不会重新归一化它。LO 工作点必须只有一个非零正频率载波，RF 工作点可含多个谱线和实 DC。RF、LO、IF 三端口不同，RF/LO 出射波为零，IF 出射波来自乘积；器件内部无附加噪声。

当 RF 与 LO 都是正频率载波、RF>LO 时，b_sum=k*a_RF*a_LO，b_difference=k*a_RF*conj(a_LO)。RF<LO 时差频折叠为 k*conj(a_RF)*a_LO。频率相同时 DC 为 sqrt(2)*k*Re(a_RF*conj(a_LO))，涉及一个 DC 输入时使用相应 sqrt(2) 系数。

固定 LO 时，这与原 ideal_mixer_conversion 的 RF 路径约定一致。新增的 LO 导数来自上述双线性乘积，因此同时具有 LO AM 和 PM 灵敏度；它不表示具有 LO 限幅、驱动门限或压缩的真实混频器已经标定。

## 工作点与扰动分开

在给定 a0 处计算 b0=F(a0)，以及：

~~~text
delta_b = A*delta_a + B*conj(delta_a)
b approximately equals b0 + delta_b
~~~

A/B 由真实实部、虚部基向量的精确双线性导数构造；这不是数值差分实现。RF 导数使用名义 LO，LO 导数使用名义 RF。输入 IF 扰动不影响这个单向模型的出射波。

不能把 a0 直接作为增量 source 使用：双线性乘积满足 J(a0)*a0=2*F(a0)，这样会将名义输出翻倍。使用[绝对波仿射网络](affine-conversion.md)可加入 b0-J(a0)*a0，并检查给定工作点与网络边界的一致性；这里的独立增量接口不隐式执行该流程，自动工作点另见[通用 C++ 核心](conversion-operating-point.md)，其双线性模型已有独立的显式 JSON 自动求解选项。

通道集合必须包含名义输出及所有一阶扰动生成的 IF 频率，包括仅由 LO 偏移扰动产生的通道；缺失即报错。RF/LO 扰动之间的二阶乘积不属于一阶矩阵，也不要求为该项自动扩展频率。频点仍由调用者显式提供。

## C++、C、Python

C++17 头文件 rfmodel/mixer_linearization.hpp 提供 linearize_real_mixer，返回 MixerLinearization，其中 incremental_model 是 FrequencyConversionModel，operating_outgoing 是名义出射向量：

~~~cpp
auto model = rfmodel::linearize_real_mixer(
    1e6, {{0,12}, {1,10}, {2,2}, {2,22}},
    {1., 2., 0., 0.}, 10, 0.);
~~~

该例两个 IF 名义输出均为 1 sqrt(W)；RF 导数幅度为 1，LO 导数幅度为 0.5。可将 incremental_model 加入 FrequencyConversionNetwork，网络的 source 和求解波均解释为扰动。跨器件共享源统计继续通过 additional_source_noise 输入。

C ABI：rfmodel_mixer_linearization_request/output 与 rfmodel_linearize_real_mixer。输入包含通道、全通道工作点复波、泵 bin、gain_db 和端口映射；输出为 N×N 的 A/B 和 N 元素名义输出。所有输出缓冲区必需，容量区间必须互不重叠且与所有输入/描述结构不重叠；容量溢出、缺失频点、非法工作点或数值错误均保持输出不变。原接口没有结构布局变更。

~~~python
model = library.linearize_real_mixer(
    1e6, [(0,12), (1,10), (2,2), (2,22)],
    [1, 2, 0, 0], lo_bin=10, gain_db=0,
)
# model.direct / model.conjugate 可用于增量转换网络；
# model.operating_outgoing 是单独的名义输出。
~~~

## JSON/CLI

格式 rfmodel.mixer-linearization v1 必需 id、spacing_hz、channels、operating_incident 和 model。model 必需 lo_bin，可选 gain_db、rf_port、lo_port、if_port，默认端口为 0/1/2。每个通道给出 port/bin，工作点必须按同一顺序覆盖全部通道。

可选扰动字段为 perturbation_source、perturbation_reflection、source_noise、phase_noise_groups 和 loaded_noise。普通 source_noise 为独立边界统计；phase_noise_groups 的 carrier_channel 使用 [id,port,bin]，载波波幅取 operating_incident。两组统计分别校验，再独立相加。perturbation_source 默认零，禁止用含糊的 source 字段替代。

结果明确标记 analysis=incremental_about_supplied_operating_point，每个通道分别输出 operating_incident/outgoing 和 perturbation_incident/outgoing；噪声 C/P 为该工作点处的一阶扰动密度。loaded_noise 可输出入射、自/交叉统计和增量净噪声流，不代表名义大信号功率预算。

[RF/LO 共参考相噪示例](../examples/mixer-lo-phase-noise.json)：RF=12 MHz、LO=10 MHz，偏移 1 MHz，RF 与 LO 的相位增益均为 1。

~~~powershell
python -m rfmodel examples/mixer-lo-phase-noise.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/mixer-lo-phase-noise-result.json
~~~

## 相噪与互易混频

若 RF 与 LO 由同一参考相位驱动、相位增益为 h_RF 和 h_LO，正常正差频的相位增益为 h_RF-h_LO，和频为 h_RF+h_LO。因此本例差频两侧的一阶共同相噪相消，而和频两侧密度为 4*ell。两个独立参考的相噪按统计量相加，不会相消。

测试还覆盖相位增益 12/10 的理想频率比关系、RF 低于 LO 的频率折叠、复载波相位、LO 幅度归一化，以及同频下变频至 DC/正交相位检波。对于确定性的强 RF 阻塞谱线，LO 偏移噪声可在没有名义载波的邻近 IF 通道产生噪声；专项使用 100 W 阻塞波、-100 dBc/Hz LO 相噪，得到对应 IF 的 1e-8 W/Hz。

一个独立网络回归在 RF 端增加幅度传输 0.5 的上游滤波器，使用滤波后的本地 RF 工作点建立混频器，再从网络外部 RF 源及 LO 源注入共同参考噪声；差频相消仍保持。这验证了跨器件相关性和局部工作点的衔接，工作点数值仍由调用者提供。

## 验证边界

原生导数与独立的正/负频率傅里叶卷积中心差分逐列比较，包含 RF DC、多 RF 谱线、LO DC 扰动和频率折叠。RF/LO 同时扰动时，完整乘积与一阶预测的差额按扰动幅度平方缩小。RF 导数还与原固定泵接口逐项比较。

SystemVue 2023 本机相噪帮助指出混频输出包含 RF/LO 相噪，及共享参考时钟下的和/差关系；来源见[帮助页记录](../validation/systemvue-2023-phase-noise-help-provenance.json)。这里的双线性定义和数值证据不等于厂商 Mixer 模型验收。

绝对波仿射接口与给定工作点一致性检查已见[仿射网络](affine-conversion.md)。[自动工作点 C++ 核心](conversion-operating-point.md)已增加阻尼迭代；固定系数双线性模型已接入 C/Python/JSON；后续仍需完整器件适配、实际 LO 驱动/限幅/压缩、PLL 频率相关传递、相噪连续谱及 PNCP 路径测量、厂商实测。当前模型忽略噪声乘噪声、高阶随机乘积和大角度调制。

## 工程验证记录

2026-10-09：Windows MSVC Debug/Release clean-first 构建成功，CTest 各 107/107 通过；两种配置安装后的独立 C/C++ consumer 各 2/2 通过。137 个 C/C++ 文件格式检查及 git diff --check 通过。

独立 Python 3.12 从本阶段 wheel 加载并连接安装后的 Release DLL：混频器线性化 13、共享相噪 13、原相噪 13、通道测量 11、加载噪声 9、变频 NF 12、转换网络 13、单器件变频 11、Python API 85、相干系统 51、线性噪声 9 项测试全部通过。

验证包含完整导数与名义输出的独立傅里叶卷积对照、二阶残差收敛、和/差相噪、折叠/DC、LO 幅度归一化、强阻塞波互易混频、上游滤波器网络、旧固定泵退化、输入与频率闭合拒绝、C 输出原子性和 CLI 失败保护。SystemVue 实测尚未完成。
