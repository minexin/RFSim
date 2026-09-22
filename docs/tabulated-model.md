# 表格 S 参数器件

`TabulatedSParameterModel` 同时实现 `RFDeviceModel` 与 `SParameterProvider`，从已验证的 TouchstoneData 或 `from_touchstone(name,path)` 创建。

模型拥有数据副本，调用者修改原数据不会影响已创建模型。提供名称、端口数量、端口编号/参考电阻、有效频率上下限及频点 S 矩阵。端口以 Port1 等名称表示，API 索引从 0 开始。

默认拒绝带外查询，可显式设置 Clamp。插值仍采用复数实虚部线性插值；每次查询仍会执行数据校验，性能优化尚未完成。当前仅保存统一正实数参考电阻；不宣称完整 Touchstone 2 支持。

回归通过基类接口调用模型并接入 analyze_linear，检查中间频点、75 欧姆参考、数据副本隔离、带外和越界行为。MSVC Debug/Release 全套各 11/11。
