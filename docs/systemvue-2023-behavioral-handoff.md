# SystemVue 2023 行为级资料与最小核验清单

本清单供后续本地核验使用，不启动脚本或仿真。主要框架是 RF Design 行为器件库 → 同一系统组网 →
Linear 外部 S → RF System 节点/端口谱与路径性能；它不是完整 2023 规格，也不扩展为全 DSP、GUI 或通用 SPICE。
当前 C++ 组合入口见[行为系统 SDK 示例](../examples/behavioral-system-sdk/README.md)。
接手执行顺序、Git 同步保护、源码与证据可用性见[本地 Codex 操作交接](local-codex-handoff.md)；
继续本地已有长期任务，云端不再并行开发。

## 先只读资料，保留来源

1. 从实际安装读取 SystemVue 精确版本/build 与帮助容器，记录 QCH 原字节 SHA-256、读取时间、
   实际页 path、解压 HTML SHA-256；用自己的话记录定义、单位、适用分析与未解决问题。
   不猜安装位置或页面文件名，不以另一版本的名称代替 2023 定义。
2. 将“帮助明确规定”“实际实验观测”“解析 oracle”“当前实现选择”分列；每条结论链接其来源。
   缺页、检索未命中或访问受限只能记为尚未核实，不能据此断言厂商功能不存在。
3. 不读取或收集许可证、密钥、凭据。完整厂商帮助正文、专有安装样例和原始安装文件留在本地，
   不上传 GitHub；项目只保存必要的事实性索引、哈希与自行概括的定义。

本轮公开访问未获得完整 2023 官方帮助正文，不能写成已经阅读全文。
[Keysight 官方概念培训资料](https://www.keysight.com/us/en/assets/9018-04145/training-materials/9018-04145.pdf)
可作为行为级系统设计的背景入口，不替代 2023 逐项规格；本轮该地址返回网页，未据此核实 PDF 全文。

## 优先 A：分析与测量语义

先在实际 2023 帮助中定位 Linear Analysis、RF System Analysis 概述，再核对：

- 外部端口如何定义；内部节点、器件端口与截面如何区分；支持哪些网络拓扑。
- 路径选择、方向、主信道及首节点/源参考面；旁支和失配如何影响结果。
- CP、DCP、CV、CNV 及节点阻抗相关项，实际名称和页名以 2023 正文为准。
- 峰值与 RMS；离散谱、PSD 与频带积分；incident/outgoing/net；总谱与期望谱。
- 坐标参考阻抗、增量 looking-in 阻抗与实际加载 V/I 的区别；电流正方向、功率符号及零电流边界。

已有[通道噪声帮助来源](../validation/systemvue-2023-channel-noise-help-provenance.json)记录的真实页为：
`sim/Spectrasys_Channel_Noise_Power.html`、`sim/Spectrasys_Channel_Noise_Density.html`、
`sim/Spectrasys_Carrier_to_Noise_Ratio.html`。它们是有哈希的历史定义摘要，不是本轮完整正文读取。
其他可定位的已归档来源包括：

- [变频噪声](../validation/systemvue-2023-conversion-noise-help-provenance.json)：
  `sim/Cascade_Noise.html`、`sim/Spectrasys_Cascaded_Noise_Figure.html` 等。
- [相位噪声](../validation/systemvue-2023-phase-noise-help-provenance.json)：
  `sim/Behavioral_Phase_Noise.html`、`sim/Spectrasys_Phase_Noise_Channel_Power.html`。
- [谱约简](../validation/systemvue-2023-spectrum-reduction-help-provenance.json)：
  `sim/Output_Tab.html`、`sim/Increasing_Simulation_Speed.html`、`sim/Intermod_Sub-Spectrum.html`。

## 优先 B：具体行为器件

按[现有型号/帮助页索引](../validation/systemvue-2023-rf-models.json)读取
`rfdesign/RFAMP.html`、`rfdesign/NPO10.html`、`rfdesign/FOU.html`，
以及 `rfdesign/VarAmp.html`、`rfdesign/ATTN_Ctrl.html`、`rfdesign/LPF_BESSEL.html`。
后三类先核正文，再决定是否及如何实现；目录存在只证明索引，不能算兼容验收。

逐型号核对适用分析、参数在各分析中的作用、端口顺序、完整复 S（两端口时为四项）、频率/数据文件解释，
以及实际存在的 IL、Amax、TYPE、参考阻抗、温度与噪声定义。不要假定这些字段适用于每个型号。
仅 S21 不唯一决定完整 S、失配响应或噪声；不能自行选一套数学实现冒充厂商参数律。
VCVS 和 mixed-mode 尚未建立该目标映射，不列为必做，也不因目录检索结果断言它们不存在。

## 非穷尽的额外功能核查

调制谱、EVM/BER、RF Link 与 Data Flow 的关系、扫参/优化等额外功能，应按 2023 章节确认
所属分析和可选模块；不因主要框架的概括而自动纳入或排除，不用旧版/新版说明替代 2023 结论。
这不是全 DSP 实现任务；当前最优先仍是节点/路径口径和具体器件参数到端口行为的映射。

## 资料明确后的最小实验与交付

- 先选择一个资料明确支持的行为器件链，再增加带旁支、失配和复参考的案例。
  每个型号只运行其明确支持的分析，不假定所有模型在不同分析间自动共用全部行为。
- 记录版本、模型/参数、实际单位、输入、端接、端口顺序/方向、温度、分析与噪声开关；
  涉及谱传播时记录实际谱约简设置回读。无法回读的项明确列为未知，不能补默认值当实测。
- 原始结果、独立解析 oracle 与模型预测分别保存，固定判定身份与阈值，再比较外部完整 S、
  指定内部端口量及有明确源/负载/方向的路径量；不能用自测计数替代厂商覆盖。
- 返回一份简洁映射：已核定义及来源、当前 API 对应、未决差异、最小实验配置与结果、下一项阻碍。
  不能验证的案例保留失败或未执行，不据此改原容差或历史记录。

既有 RFAMP A/B/C/D 与 380 项独立来源 holdout 已完成，不重新索要；
DC/RC 八次分析提交仍为零项比较获验收，四个真实安装 X-parameter 文件的实际只读重放仍未完成。
本清单不将暂停的底层候选并入主线，也不关闭许可、公开发布或稳定 API/ABI 决策。
