# Linear Analysis 输出兼容矩阵

本机来源：`C:\Program Files\Keysight\SystemVue2023\Help\systemvue.qch` 中 `sim/Linear_Analysis.html`、`sim/Linear_Analysis_Output.html`。2026-09-22 通过只读 SQLite 查询和 Qt 压缩数据解码读取。

| 输出/能力 | RFModel 当前状态 | 待完成 |
|---|---|---|
| F：M 个频率 | analyze_linear 返回 frequencies_hz | SystemVue 数值对照 |
| S：M 个 N×N 复数矩阵 | scattering，通过端口激励提取 | 模型库与数值对照 |
| ZPORT：M×N 端口阻抗 | port_impedances_ohms，当前每频点统一正实数 | 各端口独立/复参考阻抗 |
| CS：噪声相关矩阵 | 未实现 | 相关噪声源和网络传播 |
| 二端口 S11/S12/S21/S22 | 可由 scattering 索引访问 | 命名别名接口 |
| Zin1/Zin2 | 已有正实参考阻抗下的 input_impedance | 命名别名、复参考及数值对照 |
| NF、NFmin、GammaOpt、Rn | Friis 不等价于这些网络结果 | 基于 S/CS 的计算 |
| zin/yin/groupdelay | 已有标量阻抗/导纳和区间群时延 | 输出边界与 SystemVue 对照 |
| stoz/stoy | 未实现 | 多端口矩阵转换 |
| 非线性器件工作点线性化 | 未实现 | DC 工作点与模型能力 |

厂商帮助指出该 Linear Analysis 不包含稳定圆/增益圆/噪声圆及传输线、微带设计功能，不应把这些列为必须复制的功能。该描述不替代对 RF Design 各器件模块的逐项核验。

## 当前 API

`analyze_linear(grid, external_ports, builder)` 返回频率、等效 S、端口阻抗和外端口顺序。builder 返回各频点未激励的网络，支持选定外端口排序。当前逐频点重建网络，并逐端口求解；不宣称性能或完整 API 兼容。

已通过 MSVC Debug/Release 全套 10/10 回归；SystemVue 仿真尚未执行。COM 注册已只读确认：Genesys.Application 的 LocalServer32 指向本机 SystemVue2023/Bin/SystemVue.exe。注册存在不等于许可可用或自动化调用成功。
