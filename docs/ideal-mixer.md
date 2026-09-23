# 理想实数混频器

`IdealRealMixer(name, lo_bin, conversion_gain_db, lo_phase_radians=0, reference_ohms=50)` 实现 RFDeviceModel 和 SpectrumTransmissionProvider。它是两端匹配、固定外部 LO 驱动的 RF/IF 二端口模型，LO 作为参数而不是可求解的第三个端口。

同一公共频率网格上，LO 频率为 lo_bin*spacing_hz，lo_bin 必须为正整数。数学定义为 y(t)=2*g*x(t)*cos(2*pi*fLO*t+phi)，其中 g=10^(conversion_gain_db/20)。对于远离 DC 且不发生碰撞的单个输入音，上下边带各自功率增益为 g²。此模型保留两个边带，不隐式选带或模拟镜像滤波。

输入输出使用 PowerWaveSpectrum 的 sqrt(W) 幅度。内部先转为有符号傅里叶系数，分别乘 g*exp(±j*phi) 并平移频率，再恢复正频率功率波。负差频正确共轭，多个路径落在同一频点时相干相加。到达 DC 或从 DC 上变频时使用独立的归一化因子，不能套用普通单边带功率公式。

公共接口移至 power_wave_spectrum.hpp，NonlinearTransmissionProvider 保留为 SpectrumTransmissionProvider 的兼容别名。混频器没有同频 SParameterProvider 接口，因为变频行为不能由普通同频 S 矩阵表示。

回归包含上下边带相位、RF 低于 LO、同频下变至 DC、DC 上变频、两音同一 IF 的相消、转换损耗、75 欧姆元数据和非法输入。频率索引、频率值或幅度溢出显式报错，输出不得超过公共谱的 2048 项上限。

尚未实现真实 Mixer 的 LO 驱动功率依赖、端口隔离/泄漏、谐波转换矩阵、压缩与杂散表、镜像选择、相位噪声和变频噪声。此模型是变频原语，尚不代表 SystemVue Mixer 模块兼容。
