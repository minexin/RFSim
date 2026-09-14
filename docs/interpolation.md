# 频率扫描与插值

`interpolate_s(data,f,policy)` 校验完整数据集后对复数实虚部线性插值。频率单位为 Hz，支持单频样本的精确查询。默认 `OutOfBand::Reject` 拒绝带外频率；显式 `OutOfBand::Clamp` 才返回端点值。这改变了原型默认静默截断的行为。

检查频率与矩阵数量、矩阵尺寸、正参考电阻、有限复数值及严格递增频率。尚未实现相位展开插值、无源性修正或缓存验证结果。

`sweep_network(FrequencyGrid, builder)` 先验证整个扫描网格，再调用 `builder(f)` 获得每个频点的 `LinearNetwork` 并求解。返回有序 `FrequencySolution` 数组，包含 Hz 频率、所有端口入射/出射波与残差。失败时报告频率和底层错误，不返回部分成功结果。

该接口目前每频点重建网络，拓扑一致性由 builder 调用者保证；后续将扩展长期持有器件和连接关系的系统对象及结果导出。

已验证 Touchstone 两端点与中间插值频点、失配负载反射响应、带外报错/截断、非法数据集和扫描前网格检查。MSVC Debug/Release CTest 各 6/6。尚未完成 SystemVue 对照、性能测试和跨平台实测。
