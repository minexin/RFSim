# 二端口网络噪声系数

`noise_figure.hpp` 提供 `two_port_noise_figure_db(S, intrinsic, source_reflection={}, reference_temperature_k=290)`，计算从端口 0 到端口 1 的噪声系数，返回 dB。intrinsic 是器件或等效网络自身的 W/Hz 噪声相关矩阵，不包括外部信号源和输出终端噪声。参考阻抗与 S、源反射系数必须一致，当前采用正实数功率波定义。

输出端匹配且不添加测量负载噪声。源反射系数 Gamma 必须满足 |Gamma|<1。由 b=S*a+c、a0=e+Gamma*b0 推得折算到源平面的噪声权重 w=[Gamma, (1-S11*Gamma)/S21]，于是 F=1+(w*C*w†)/(kB*T0*(1-|Gamma|²))。实现先以 kB*T0 归一化矩阵，再用已有相关噪声传播计算二次型，以 log1p 换算 dB。

噪声波定义与源失配处理参考 [NIST TN 1530](https://tsapps.nist.gov/publication/get_pdf.cfm?pub_id=32951) 和 [NIST 源失配噪声温度推导 A.6](https://tsapps.nist.gov/publication/get_pdf.cfm?pub_id=31584)。本文权重式由上述边界方程直接消元得到。

拒绝非二端口、非法协方差、非正参考温度、非无源源反射、零前向传输、奇异源反馈及数值溢出。这里的源反馈检查不构成完整稳定性分析。反向噪声系数需先一致地重排 S 与噪声矩阵端口。

解析验证覆盖常温衰减器 F=1/G、双倍物理温度、源失配、具有虚部相关项的噪声源，以及衰减器加放大器网络求解与独立 Friis 实现的一致性。可逐频点将 analyze_linear 返回的 scattering 和 noise_correlation 配对使用。NFmin/GammaOpt/Rn 提取见下节；非匹配测量负载及 SystemVue 对照仍待完成。

本阶段 MSVC Debug/Release 全套各 23/23 通过。

## NFmin、GammaOpt 与 Rn

`extract_noise_parameters(S,C,reference_ohms=50,temperature_k=290)` 返回 `TwoPortNoiseParameters`，包含 minimum_noise_figure_db、optimum_source_reflection 和 noise_resistance_ohms。参考阻抗用来将归一化噪声电阻换算为欧姆；必须与输入端口参考相同。

先把噪声转换到输入参考矩阵 Q，使分子为 A*|Gamma|²+2*Re(B*Gamma)+D。取 t=A+D，最佳源为 -2*conj(B)/(t+sqrt(t²-4*|B|²))，在缩放后计算判别式以避免平方溢出。Rn=Z0*(A+D-2*Re(B))/4。NFmin 用既有噪声系数函数在最佳源处求值。参数约定及 NF 重建式参考 [Qucs 技术文档 Noise Parameters](https://qucs.github.io/tech/node9.html)。

全零噪声时所有源同样最优，约定返回 GammaOpt=0、NFmin=0 dB、Rn=0。若最优点只位于单位圆边界或在浮点精度内无法分辨其内部距离，则抛出 domain_error，不把不可实现的极限源报告为有效最优源。仍会拒绝在该源处出现奇异反馈的网络。

回归采用已知复数 GammaOpt 的合成矩阵，验证 NFmin、Rn 及多个源反射点的 NF 重建；并覆盖复数增益、输入反射、75 欧姆换算、无源衰减器、零噪声和边界退化。SystemVue 数值对照仍未完成。

## 器件噪声参数导入

`noise_from_parameters(S, parameters, reference_ohms=50, temperature_k=290)` 将 NFmin（dB）、GammaOpt 和 Rn（欧姆）转换为器件本征噪声相关矩阵，可传给 independent_noise 和网络求解。temperature_k 是这些噪声参数的参考温度 T0，并非器件物理温度。固定噪声参数而改变 T0 会按比例改变重建的 W/Hz 矩阵；这不是器件随温度变化的模型。

令 e=10^(NFmin/10)-1、K=4*Rn/(Z0*|1+GammaOpt|²)，输入参考矩阵为 Q=[[K-e,-K*conj(GammaOpt)],[-K*GammaOpt,e+K*|GammaOpt|²]]。用 [[1,S11],[0,S21]] 将 Q 转回器件端口，再乘 kB*T0。实现使用 expm1，复用半正定检查及相关矩阵传播。

非负 NFmin 和 Rn 不足以保证物理可实现性：若 Q 非半正定，输入被拒绝。还拒绝单位圆外/边界 GammaOpt、零前向传输、非法参考及溢出。此接口不读取厂商文件，Touchstone 噪声段解析仍待实现。

新增验证覆盖复数 S 参数双向转换、非默认参考阻抗、参考温度缩放、多个失配源的 NF 重建、无源热噪声矩阵复原及导入参数后的两级网络噪声；还包括不相容参数和溢出拒绝。
