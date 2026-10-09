# 绝对波仿射转换网络

频率转换网络支持 `b = A*a + B*conj(a) + d`。A/B 描述同一给定工作点的直接及共轭响应，d 是确定性的器件内部出射波，单位 sqrt(W)。物理接线和端口反射仍由整体网络方程联立求解。d 可以出现在已接线的内部通道；它不是外部 source，不能用边界激励替代。

## 工作点与适用范围

对于非线性器件 F，在给定工作点 a0 处，仿射偏置为 `d = F(a0) - A*a0 - B*conj(a0)`。当前实混频器是固定归一化系数的双线性乘积，因此 `J(a0)*a0 = 2*F(a0)`，其偏置为 `-F(a0)`。省略偏置而直接把绝对载波输入增量矩阵会使名义输出翻倍。

`linearized_real_mixer` JSON 器件同时生成 A/B/d，完成网络求解后核对每个本地通道的入射波与提供的 operating_incident，包括 IF 负载反射。允许误差为 `1e-12 + 1e-9*max(abs(actual),abs(expected))` sqrt(W)。不一致即报错，不能继续输出基于错误工作点的噪声报告。成功结果包含 operating_point_checks，记录最大误差及容差。

这是**给定工作点的一致性检查**。当前没有自动迭代非线性网络工作点，也不自动扩展混频频率；A/B/d 在本次分析中固定。对于手工 matrix 或显式 output_offset，调用者负责其物理含义与工作点有效性。

## C++、C、Python 接口

- FrequencyConversionModel::analyze 的最后一个可选参数是 output_offset；空向量保持原行为。
- FrequencyConversionNetwork::analyze(loaded_noise, additional_source_noise, output_offset) 使用器件顺序、随后本地通道顺序的一维全局偏置。
- MixerLinearization::output_offset() 返回混频器仿射偏置，不改变原返回结构的成员布局。
- C ABI 新增 rfmodel_conversion_affine_offset 和 rfmodel_conversion_network_analyze_affine。偏置描述符必需，count 必须等于全局通道总数，values 非空；额外源噪声与加载噪声输出可为空。原有 C 描述符及函数保持不变。
- Python Library.conversion_network 和 Library.frequency_conversion 新增 output_offset 关键字；MixerLinearization.output_offset 是属性，原 NamedTuple 仍为三个字段。

所有偏置必须有限；DC 偏置必须为实数。C 接口的所有输出区间（按声明容量）必须互不重叠，也不能覆盖偏置数组、偏置描述符或其他输入。非法参数、奇异反馈等失败均不写入输出。

```python
mixer = library.linearize_real_mixer(
    1e6, [(0, 12), (1, 10), (2, 2), (2, 22)],
    [1, 2, 0, 0], lo_bin=10,
)
result = library.frequency_conversion(
    1e6, [(0, 12), (1, 10), (2, 2), (2, 22)],
    mixer.direct, mixer.conjugate, source=[1, 2, 0, 0],
    output_offset=mixer.output_offset,
)
# 两个 IF 出射波均为 1 sqrt(W)。
```

C++/C/Python 低层仿射求解器不保留非线性器件定义，不能替调用者检查工作点；上述自动检查属于网络 JSON 的 linearized_real_mixer 装配流程。原独立 rfmodel.mixer-linearization 文档及 CLI 仍分析增量波。

## JSON 网络与可运行示例

rfmodel.conversion-network v1 的普通 matrix、固定泵模型及 linear 器件可声明本地 output_offset 数组。元素可以是实数或 [real,imag]，必须覆盖所有本地通道。单器件 rfmodel.frequency-conversion v1 也接受同名顶层字段。

网络器件 model.type=linearized_real_mixer 需要 lo_bin 和 operating_incident，可选 gain_db、rf_port、lo_port、if_port。偏置由模型生成，禁止再声明器件 output_offset。网络结果使用 wave_relation=affine 并返回全局 output_offset。

[完整示例](../examples/affine-mixer-network.json) 将输入滤波器、RF/LO 实混频器和输出滤波器接为网络。输入幅度传输为 0.5，输出为 0.25；外部 RF 波为 1、LO 波为 1 sqrt(W)，本地 RF 工作点为 0.5。最终 2 MHz 与 22 MHz 的 IF 波均为 0.125 sqrt(W)。

```powershell
python -m rfmodel examples/affine-mixer-network.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/affine-mixer-result.json
```

示例还注入 RF/LO 共参考相噪。相位增益均为 1，偏移 1 MHz，密度 -100 dBc/Hz：差频两侧一阶相噪为零，和频两侧各为 6.25e-12 W/Hz。

d 是确定性项，不改变固定 A/B 下的出射、入射、交叉噪声 C/P，也不改变噪声净功率流或另行执行的参考 NF 实验。更换工作点导致 A/B 改变时，噪声当然可能改变。

## 验证与后续

专项覆盖复反射共轭反馈的独立闭式解、内部接线偏置传播、混频名义波翻倍修正、零偏置退化、噪声不变性、共享相噪、反射 IF 工作点检查、严格 JSON、C 输出原子性和 CLI 失败保护。

自动非线性工作点求解、实际 LO 驱动/压缩、厂商转换矩阵标定和 SystemVue 实测仍未完成。当前解析证据不等于 SystemVue RF Design 库全覆盖。

## 工程验证记录

2026-10-09：Windows MSVC Debug/Release clean-first 构建成功，CTest 各 109/109；安装后的独立 C/C++ consumer 各 2/2。138 个 C/C++ 文件格式检查及 git diff --check 通过。

独立 Python 3.12 从 wheel 加载并使用安装后的 Release DLL：仿射转换 11、混频线性化 13、共享相噪 13、原相噪 13、通道测量 11、加载噪声 9、变频 NF 12、转换网络 13、单器件变频 11、Python API 85、相干系统 51、线性噪声 9 项测试全部通过，共 251 项。

本记录不包含尚受 Error Running Script 弹窗阻塞的 SystemVue 实测。
