# Linear Analysis 输出兼容矩阵

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

本机来源：`C:\Program Files\Keysight\SystemVue2023\Help\systemvue.qch` 中 `sim/Linear_Analysis.html`、`sim/Linear_Analysis_Output.html`。2026-09-22 通过只读 SQLite 查询和 Qt 压缩数据解码读取。

| 输出/能力 | RFModel 当前状态 | 待完成 |
|---|---|---|
| F：M 个频率 | analyze_linear 返回 frequencies_hz | SystemVue 数值对照 |
| S：M 个 N×N 复数矩阵 | scattering，通过端口激励提取 | 模型库与数值对照 |
| ZPORT：M×N 端口阻抗 | port_impedances_ohms，支持逐频率、各端口独立复参考阻抗 | SystemVue 波定义与数值对照 |
| CS：噪声相关矩阵 | 已有 W/Hz 相关噪声网络扫描输出 | SystemVue CS 归一化/单位及厂商噪声参数后处理约定对照 |
| 二端口 S11/S12/S21/S22 | 可由 scattering 索引访问 | 命名别名接口 |
| Zin1/Zin2 | 已有正实参考阻抗下的 input_impedance | 命名别名、复参考及数值对照 |
| NF、NFmin、GammaOpt、Rn | 已有公共实参考及独立复功率波参考的 NF、NFmin、GammaOpt、物理 Rn，可由 C++ 类型化分析逐频点组合（旧 Python/JSON CLI 已退役） | SystemVue 对照及退化边界语义核验 |
| zin/yin/groupdelay | 已有标量阻抗/导纳和区间群时延 | 输出边界与 SystemVue 对照 |
| stoz/stoy | 已有统一正实及独立复功率波参考的 S→Z/Y 转换 | SystemVue 数值对照 |
| 非线性器件工作点线性化 | 已有 C++ 给定模型直接/共轭 Jacobian、反馈工作点及混合复参考物理接线 | 通用器件 DC 偏置/模型线性化与厂商数值验收；DC 频点本身不证明 DC 偏置能力 |

厂商帮助指出该 Linear Analysis 不包含稳定圆/增益圆/噪声圆及传输线、微带设计功能，不应把这些列为必须复制的功能。该描述不替代对 RF Design 各器件模块的逐项核验。

## 当前 API

`analyze_linear(grid, external_ports, builder)` 返回频率、等效 S、端口阻抗和外端口顺序。builder 返回各频点未激励的网络，支持选定外端口排序。当前逐频点重建网络，并逐端口求解；不宣称性能或完整 API 兼容。

## 当前证据边界（2026-10-14）

[混合复参考工作点](conversion-operating-point-power-wave.md)已实现，并有独立根、
V/I 连续性、收敛 Jacobian C/P 和安装消费者回归；
[因子域噪声修复](../validation/noise-factor-propagation-validation-20261014.json)
保留输入准入规则，改善极端相关/抵消情况下的传播。它们不构成 SystemVue
ZPORT/CS 或通用 DC 偏置线性化验收。[接受工作点本征噪声提供器](conversion-operating-noise.md)已集成，
收敛后一次求值并冻结完整 C/P；它不自动提供厂商器件噪声律。

全项目已有 SystemVue 归档采集、有限通过及原始失败；本表所列复参考/输出
语义仍需各自数值对照。当前接口、证据和最终提交门禁见
[能力证据说明](compatibility-evidence.md)及[机读映射](../validation/compatibility-evidence.json)。
不能用旧测试总数或其他提交 CI 代替当前代码验收。

## 历史记录（非当前状态）

早期阶段记录：MSVC Debug/Release 全套 10/10 通过；该阶段尚未执行 SystemVue。
当时只读确认 Genesys.Application 的 LocalServer32 指向本机
SystemVue2023/Bin/SystemVue.exe；注册不等于许可或自动化成功。该描述不覆盖
后续已完成的 SystemVue 归档采集。

2026-10-09 的[独立复参考输出](power-wave-references.md)及内生噪声变换记录保留。
当时公共正实系统边界的限制已被后续 C++ 复参考网络/工作点能力扩展，但不能
据此新增厂商实测结论。独立复参考噪声后处理、物理源阻抗与 GammaOpt 的关系见
[二端口噪声参数](power-wave-noise.md)。历史 Python 入口按[退役记录](python-interface-retirement.md)处理。
