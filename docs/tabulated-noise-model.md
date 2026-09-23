# 频变噪声器件

`TabulatedNoiseModel` 同时实现 RFDeviceModel、SParameterProvider 和 NoiseCorrelationProvider，持有 S 数据和独立噪声频率表的副本。NoiseTable 的样本单位为 W/Hz，必须与 S 使用相同端口顺序和功率波参考。

构造时检查频率严格递增、矩阵尺寸及半正定性。噪声查询采用相邻相关矩阵的凸组合，保留 Hermitian 半正定结构。S 和噪声可以使用不同频率网格；默认各自在带外报错，显式 OutOfBand::Clamp 才使用端点值。无联合查询时不隐式限制为两表的交集；同时求 S 与噪声的扫描必须满足两者范围。

噪声相关项不能通过单独插值幅度/相位替代本接口的矩阵插值。此模型不自动施加热平衡关系，也不读取厂商噪声文件。调用方可用 noise_from_parameters 生成噪声表样本。

MSVC Debug/Release 全套各 26/26 通过，新增检查覆盖复数相关项插值、独立频率网格、数据所有权、单频钳位、非法矩阵及端到端 NF 扫描。SystemVue 数值对照仍待完成。
