# 当前 C++ 能力与证据

[机读映射](../validation/compatibility-evidence.json)是当前兼容摘要的权威索引，
绑定源提交 `0d7afc5d6738147a407c2ea16c3d693a31343128`，不是全目录验收清单。
它分列实现范围、独立数值证据、厂商归档状态、剩余问题及发布门禁。
`implemented` 只表示所述 C++ 契约存在；`bounded_pass` 只表示指定厂商子集通过。
没有总兼容率或整体通过标志。243 项目录只作库存，不计作实现数量。

## 精确提交与运行

- 高阶适配器：`1b1fb8511286285b6f63ea808cd68610e458fc83`，
  [CI 38566887480](https://github.com/minexin/RFSim/actions/runs/38566887480)，18/18 作业成功。
- 双线性适配器：`31b291e3ccfbd111687aec4712dc0abf6b793bdb`，
  [CI 38568395820](https://github.com/minexin/RFSim/actions/runs/38568395820)，18/18 作业成功。
- 因子域噪声：`f7f94f81d7f3b8d5d5931e3a7a8a7be51566b0ca`，
  [CI 38571067267](https://github.com/minexin/RFSim/actions/runs/38571067267)，18/18 作业成功；
  本地严格 C++17 Debug/Release 和完整/纯 SDK 证据见
  [阶段记录](../validation/noise-factor-propagation-validation-20261014.json)。
- 接受工作点本征噪声：已纳入本索引源提交；严格 C++17 Debug/Release
  各 154/154，重定位完整 SDK 各 13/13，纯 SDK 各 11/11，638 项专项检查及
  独立数值复核见[阶段记录](../validation/accepted-state-intrinsic-noise-validation-20261014.json)。
  [本提交 CI 38572379978](https://github.com/minexin/RFSim/actions/runs/38572379978) 18/18 作业成功。

四份已完成运行的逐作业快照保存在机读映射所指的证据文件中。
`verified_successful_jobs` 只计已核验成功作业；pending 时的零不表示运行实际没有成功作业。
18 作业是 Windows/Ubuntu/macOS × Debug/Release × 核心/纯 SDK/归档重放；
并非 18 个厂商兼容测试。旧提交运行不能证明后续代码通过。
工作点相关噪声提供器已纳入此源快照；后续文档/代码集成仍须核验精确最终提交 CI。
PHD 后续工作未计为已实现。所有发布门禁目前保持开放；本阶段不添加许可、
版本标签或发布包，许可选择属于用户决策。

## 保留失败与有限结论

- AM/PM：显式校准后的 549 个无告警登记点、37 个告警诊断点及额外端点分开；
  原始解析映射 110/586 的失败仍在。给定实测输出功率的相位吻合不等于网络功率预测通过。
- DataPoints：输出离散候选 8/8 是固定表的有限新配置预测；输入候选 2/8。
  原连续样条及仅名义零旁路反例保留，不导出连续律、精确边界或导数。
- 显式高阶归档 1413 来源项通过不代表自动 RFAMP 系数；0 dB 级联、RFPwrIn、
  衰减器噪声原失败不改判。频谱削减关闭后恢复的来源不再列为未知缺项。
- 通用 ModelAdapter 不等于 PHD、专用器件库或厂商参数翻译器。

原始报告、容差、告警、校准/独立评分边界和历史构建计数不重写。本阶段没有
新增 SystemVue 采集。历史 Python/JSON 叙述按[退役说明](python-interface-retirement.md)阅读。

## 校验与维护

[JSON Schema](../validation/compatibility-evidence.schema.json)使用 Draft 2020-12。
开发检查可运行 `python scripts/reference/check_compatibility_evidence.py`，无需产品
Python 包或第三方模块；它校验此 schema 使用的子集，并核验路径、唯一 ID、
目录库存及成功 CI 的逐作业/提交绑定。`--self-test` 注入坏引用、重复 ID、
虚假 CI 成功及 schema 违规，检查拒绝行为。该工具只校验证据元数据，
不验证自由文本数值/厂商结论的真实性，不执行 C++ 数值测试，也不将归档失败
提升为通过。默认 C++ SDK 构建不依赖它。

新增实现应提供明确契约与独立数值测试，再引用实际运行记录；新增厂商通过必须
有独立的场景/原始采集/容差依据。最终集成更新本映射，不能仅改文档中的“通过”。
