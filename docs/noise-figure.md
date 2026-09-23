# 二端口网络噪声系数

`noise_figure.hpp` 提供 `two_port_noise_figure_db(S, intrinsic, source_reflection={}, reference_temperature_k=290)`，计算从端口 0 到端口 1 的噪声系数，返回 dB。intrinsic 是器件或等效网络自身的 W/Hz 噪声相关矩阵，不包括外部信号源和输出终端噪声。参考阻抗与 S、源反射系数必须一致，当前采用正实数功率波定义。

输出端匹配且不添加测量负载噪声。源反射系数 Gamma 必须满足 |Gamma|<1。由 b=S*a+c、a0=e+Gamma*b0 推得折算到源平面的噪声权重 w=[Gamma, (1-S11*Gamma)/S21]，于是 F=1+(w*C*w†)/(kB*T0*(1-|Gamma|²))。实现先以 kB*T0 归一化矩阵，再用已有相关噪声传播计算二次型，以 log1p 换算 dB。

噪声波定义与源失配处理参考 [NIST TN 1530](https://tsapps.nist.gov/publication/get_pdf.cfm?pub_id=32951) 和 [NIST 源失配噪声温度推导 A.6](https://tsapps.nist.gov/publication/get_pdf.cfm?pub_id=31584)。本文权重式由上述边界方程直接消元得到。

拒绝非二端口、非法协方差、非正参考温度、非无源源反射、零前向传输、奇异源反馈及数值溢出。这里的源反馈检查不构成完整稳定性分析。反向噪声系数需先一致地重排 S 与噪声矩阵端口。

解析验证覆盖常温衰减器 F=1/G、双倍物理温度、源失配、具有虚部相关项的噪声源，以及衰减器加放大器网络求解与独立 Friis 实现的一致性。可逐频点将 analyze_linear 返回的 scattering 和 noise_correlation 配对使用。NFmin/GammaOpt/Rn 提取、非匹配测量负载及 SystemVue 对照仍待完成。

本阶段 MSVC Debug/Release 全套各 23/23 通过。
