# 集总二端口接口

`lumped_model.hpp` 的 `LumpedTwoPortModel` 实现 RFDeviceModel 和 SParameterProvider。构造参数为名称、连接方式、随频率变化的本构函数及所有端口共用的正实数参考阻抗（默认 50 欧姆）。频率单位 Hz，允许 DC。

- `SeriesImpedance`：函数返回串联阻抗 Z，单位欧姆。令 u=Z/Z0，S11=S22=u/(2+u)，S12=S21=2/(2+u)。
- `ShuntAdmittance`：函数返回对地并联导纳 Y，单位西门子。令 u=Y*Z0，S11=S22=-u/(2+u)，传输项仍为 2/(2+u)。

例如串联电阻返回 R，串联电感返回 j*2*pi*f*L，并联电容返回 j*2*pi*f*C。本构函数可持有参数或数据；其异常向调用方传播，扫描层附加频率上下文。允许有源负阻抗，但不自动验证无源性、因果性或稳定性。

当前要求本构值为有限复数，归一化溢出和接近 2+u=0 的转换均报错。此通用回调接口不接受无穷值；理想 RLC 的 DC 极限由下面的专用模型处理。模型尚不产生热噪声，也没有寄生、温度、偏置或器件容差参数。

验证：50 欧姆串联电阻、75 欧姆并联电导、纯电容功率守恒，以及串联 R 后接并联 C 的三频点网络扫描。RC 网络四个 S 元素对照独立 ABCD 解析表达式，覆盖两端反射不同的情况。MSVC Debug/Release 全套各 18/18 通过；SystemVue 对照待完成。

## 理想 RLC 器件

`rlc_model.hpp` 提供 `IdealRLCModel(name, element, connection, value, reference_ohms=50)`。element 为 `IdealElement::Resistor/Inductor/Capacitor`；connection 使用相同的 `LumpedConnection` 枚举选择串联或对地并联。value 分别以欧姆、亨利、法拉为单位，必须有限且严格为正。零值退化元件应以明确的连接拓扑表示。

例如 `IdealRLCModel("C1", IdealElement::Capacitor, LumpedConnection::SeriesImpedance, 1e-12)` 创建串联 1 pF 电容。它实现相同的器件与 S 参数接口，可按频率接入 LinearNetwork。

DC 时：串联 C 的 S 为单位矩阵，并联 L 的 S 为负单位矩阵；串联 L 和并联 C 均为理想直通。非零频率使用 Z=R、j*omega*L 或 1/(j*omega*C)，按连接归一化。内部使用对数幅值和缩放分式，不先生成可能溢出的阻抗值；极端范围会自然舍入到相应极限，不表示有限精度仍能分辨微小传输。

回归覆盖六种形式在三频点的直接阻抗公式、LC 功率守恒、DC 极限、75 欧姆元数据、非法参数和大数电感极限。MSVC Debug/Release 全套各 19/19 通过。尚未包含热噪声、寄生、频率相关损耗与 SystemVue 对照。