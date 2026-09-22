# 基础匹配传输器件

`MatchedTransmissionModel(name, loss_db, delay_s, reference_ohms)` 实现 RFDeviceModel 和 SParameterProvider。零延时对应理想匹配衰减器，零损耗对应理想延迟线，可组合常数损耗与延时。

散射矩阵对角项为零；两个传输方向均为 `10^(-loss_db/20) * exp(-j*2*pi*f*delay_s)`。频率为 Hz，延时为秒，损耗为非负 dB，参考阻抗为正实数欧姆。负损耗、负延时、非法频率被拒绝。

该模型不包含色散、寄生失配、温度噪声或真实电缆参数；未来噪声能力需另行组合，不能把无噪声接口理解为实际被动器件无热噪声。极大 f*delay 下相位精度受双精度限制。

MSVC Debug/Release 全套 13/13 回归通过。新增测试覆盖 10 dB 功率衰减、四分之一周期相移、双向互易性，以及衰减器与延迟线的级联响应。尚未执行 SystemVue 数值对照。
