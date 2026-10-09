# 独立复参考下的二端口噪声参数

本接口把 Linear Analysis 的内生噪声相关矩阵转换为 NFmin、最优源反射系数 GammaOpt 和物理噪声电阻 Rn，并按指定物理源阻抗计算 NF。支持两个端口各自不同的有限复参考阻抗，实部必须严格为正。

## 定义

采用[功率波参考](power-wave-references.md)，正向路径为端口 0→1。内生协方差单位 W/Hz，不能包含源与输出负载自身的噪声；输出端为无噪声且入射反射系数为零的终端。源温度 T0 默认为 290 K，必须严格为正。

源边界为 a=GammaS*b+source，因电流正向定义，其关系是：

- GammaS=(Zs-Zref)/(Zs+conj(Zref))
- Zs=(Zref+conj(Zref)*GammaS)/(1-GammaS)

GammaOpt 使用上述源端定义。它与器件 b/a 的反射系数公式不同；当 Zref 有虚部时不能混用。NFmin 用 dB 表示，Rn 用 Ω 表示。Rn 是物理噪声电阻，不是归一化电阻。

物理源导纳形式为 F=Fmin+Rn/Re(Ys)*|Ys-Yopt|²，F 是线性噪声因子。该形式也可在 [scikit-rf 官方噪声实现](https://scikit-rf.readthedocs.io/en/latest/_modules/skrf/network.html) 的 nf 方法中核对。实现使用已有相关矩阵二次型求取最优源，再按复参考波定义还原物理 Rn；不是把实参考阻抗的 Rn 公式直接用于复数。

更换参考坐标时，NFmin、Rn 和物理最优源阻抗不变，GammaOpt 随参考改变。同一物理源阻抗下的 NF 也不变。回归独立使用物理源导纳式验证这些关系。

## 接口

C++ 头文件 include/rfmodel/power_wave_noise.hpp：

- power_wave_noise_figure_db(s, noise, references, source_impedance_ohms, temperature_k)
- extract_power_wave_noise_parameters(s, noise, references, temperature_k)
- noise_from_power_wave_parameters(s, parameters, references, temperature_k)

parameters 使用既有 TwoPortNoiseParameters。references 为长度 2 的复数向量。noise_from_power_wave_parameters 将 NFmin/GammaOpt/Rn 转回 W/Hz 协方差，并检查半正定性；标量参数为正不保证整个噪声模型物理可行。

C ABI 新增 rfmodel_noise_parameters 结构体和以下函数：

- rfmodel_power_wave_noise_figure
- rfmodel_power_wave_extract_noise_parameters
- rfmodel_power_wave_noise_from_parameters

S/协方差都固定为四个按行排列的复数；value_count 必须为 4，参考数组包含两个值。输出不能与输入重叠；容量不足、非法输入、奇异反馈和不可实现噪声参数均报错且不改输出。

Python 对应 Library.power_wave_noise_figure、Library.power_wave_noise_parameters 和 Library.noise_from_power_wave_parameters，返回 NoiseParameters 命名元组。可运行示例 examples/power-wave-noise.py 接受原生动态库路径，输出完整参数及一个物理源阻抗下的 NF。

## 退化情况及验收范围

完全无噪声时 NFmin=0 dB、Rn=0，任意被动源都最优；选 GammaOpt=0，代表 Zs=Zref。最小值仅在单位圆边界达到、前向传输为零、奇异源反馈等情况明确报错。接口不包含输出负载热噪声，也不计算反向噪声参数。

已加入 C++、C、Python 的解析无源衰减器、复参考不变性、相关噪声往返、温度尺度、无噪声退化、非物理解及错误输出原子性回归。本阶段继续保留 SystemVue 2023 对照未完成的状态：尚未确认其 CS 单位、GammaOpt 复参考约定及退化处理，不能据此声明厂商兼容验收通过。

Python 包继续声明 >=3.9；本阶段将上一阶段的 tuple|None 运行时注解改为 Optional[tuple]。本机使用 3.10 与独立 3.12 执行验证，3.9 仅做语法检查，未声称已执行 3.9 运行时测试。

## 工程验证（2026-10-09）

MSVC 2022 x64 Debug/Release clean-first 构建及 CTest 分别 86/86；安装后独立 C/C++ 消费测试分别 2/2。Python 3.12 从独立 wheel 加载，API 85/85、系统图 51/51；示例成功输出 NFmin=3.0102999566 dB、Rn=57.375 Ω，以及 Zs=25+30j Ω 下的 NF=4.1127448985 dB。C++ 格式检查 120 文件通过。GitHub 三平台 CI 由本提交触发，结果以 Actions 为准。

未完成实测的 SystemVue 高阶模型采集草稿未纳入本阶段提交；当前记录不把草稿或尚未关闭的弹窗视作厂商比对证据。
