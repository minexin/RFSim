# 当前 C++ 能力与证据

[机读映射](../validation/compatibility-evidence.json)是当前兼容摘要的权威索引，
本覆盖层冻结的源基线为 `a2bbfd5dd3366082620cbd0ebf99dbf11628bd40`，不是全目录验收清单。
writer 可移植性修复已取得精确提交跨平台 CI 成功；line 共振修复已集成，精确源提交 CI 38615005226 已 18/18 成功。
后续覆盖层提交仍须单独核验 CI，不能继承源基线成功。
本覆盖层包含文档、开发用证据 checker/schema 逻辑及 SDK 示例空白格式修正；不改数值实现。
它分列实现范围、独立数值证据、厂商归档状态、剩余问题及发布门禁。
`implemented` 只表示所述 C++ 契约存在；`bounded_pass` 只表示指定厂商子集通过。
没有总兼容率或整体通过标志。243 项目录只作库存，不计作实现数量。

## 已实现的有界能力

- 基础 API：器件端口及噪声 provider 形状契约、有限插值结果与线性求解失败边界。
  冻结阶段严格 C++17 Debug/Release 各 157/157，重定位完整 SDK 各 15/15、纯 SDK 各 13/13。
- 确定性变频波：独立 `ConversionWaveResult` 与单 RHS 求解；Newton 候选不传播噪声，
  初始验证、接受工作点及最终噪声 provider 契约保持。`solve_waves` 不验证噪声，
  `analyze(false)` 仍计算出射 C/P。阶段各 159/159 完整、112/112 纯 SDK，
  完整/纯 SDK consumer 各 15/15、13/13。N=256 非线性端到端五次中位数约
  18.075→11.804 秒，仅为固定案例共享主机证据。旧静态成本包含噪声；不称纯 solver
  42 倍提速或渐近复杂度改善，稠密分解仍为三次复杂度。
- 原生 PHD：显式 F/S/T、单振幅、固定泵相位与实正参考的有界系数切片已实现，
  并有泵钳位扰动 Jacobian。2362 项专项检查及严格 C++17 阶段证据见
  [系数契约](phd-coefficient-contract.md)。这不是厂商文件解析、系数提取、
  插值/外推、参考变换或完整泵 Newton 导数；不从 F/S/T 推断本征噪声。
- SDK：内部工程归档、来源及哈希校验、提取与 installed-only 双 TU 示例已实现。
  示例实际扫描匹配 6 dB 衰减器和无损 50 Ω、1 ns 传输线并核对波与送达功率。
  `main.cpp` 仅补 clang-format 19.1.7 空白格式，`network.cpp` 不变。
  内部 ZIP 不代表正式发布；许可、版本、标签及 API 稳定性仍需用户决策。
- Touchstone：已有有界导入及新增 C++17 单端 S 流导出（2.0 / Hz / RI / Full，
  1–1024 端口、64 MiB payload 上限）。拒绝非空噪声样本；不采样模型、重归一化或导出 PHD。
  本地阶段完整各 161/161、纯 SDK 各 122/122；但原 writer 跨平台 CI 失败，不能据此称全平台通过。
  后续 decimal 可移植性修复 `a987c3d` 的独立运行已 18/18 成功，含 macOS Debug/Release；
  原 writer 失败保留，修复后成功不覆盖尚待冻结的最终覆盖层。
  调用方负责文件、同步和 flush/close；部分写入失败可能留下前缀，无原子性或持久性保证。

以上本地计数只属于各自冻结阶段，不是最终覆盖层重跑结果。精简可移植摘要见
[工程阶段记录](../validation/engineering-evidence-summary-20261014.json)，完整日志保留在外部审计归档。

均匀线共振修复采用独立 100 位 ABCD 固定 oracle；本地重绑定 Debug/Release 各
161/161 完整、149/149 纯原生（均启用开发参考测试）、16/16 完整安装消费者和
14/14 纯安装消费者，总计 680/680。该阶段局部候选与已集成提交源树一致；
精确源提交跨平台 CI 已 18/18 成功。一般频率延迟乘积的精度与溢出限制保留，
独立发现的 RLGC 中间溢出边界尚未闭合，不纳入本轮成功证据。

## 精确提交与运行

以下成功记录均为各自精确提交 18/18 作业，不覆盖后续提交：

- 高阶适配器 `1b1fb8511286285b6f63ea808cd68610e458fc83`：[38566887480](https://github.com/minexin/RFSim/actions/runs/38566887480)
- 双线性适配器 `31b291e3ccfbd111687aec4712dc0abf6b793bdb`：[38568395820](https://github.com/minexin/RFSim/actions/runs/38568395820)
- 因子域噪声 `f7f94f81d7f3b8d5d5931e3a7a8a7be51566b0ca`：[38571067267](https://github.com/minexin/RFSim/actions/runs/38571067267)
- 接受工作点噪声 `0d7afc5d6738147a407c2ea16c3d693a31343128`：[38572379978](https://github.com/minexin/RFSim/actions/runs/38572379978)
- 旧证据文档 `d64ac59c380bc1bd3367846c68646d60585da382`：[38574445472](https://github.com/minexin/RFSim/actions/runs/38574445472)
- 原生 PHD `ddd2e4be64a628f3e21cd954a6f752144f3e944c`：[38575586569](https://github.com/minexin/RFSim/actions/runs/38575586569)
- SDK 归档 `1a6d1bb60b2513aaff6648acd70c5e50788e9f65`：[38578171209](https://github.com/minexin/RFSim/actions/runs/38578171209)
- pilot LF 修复 `8b555eecb9e878949703b2640a0bfd039ba518c6`：[38581465433](https://github.com/minexin/RFSim/actions/runs/38581465433)
- 迁移修复 `0d18e464bd825a2851cd5bda2706395d5a6915d6`：[38583782458](https://github.com/minexin/RFSim/actions/runs/38583782458)
- 基础 API `89c93d57d23d9aeea150a21468662a7d00fe10d9`：[38595759547](https://github.com/minexin/RFSim/actions/runs/38595759547)
- 确定性 wave `1a0f5ffcd27d9efad09ecd59b238a8e622900367`：[38597218325](https://github.com/minexin/RFSim/actions/runs/38597218325)
- SDK 数值示例 `45bafd674b46f9a7f35353631f72c131a0edc5c1`：[38598650770](https://github.com/minexin/RFSim/actions/runs/38598650770)
- Touchstone decimal 可移植性 `a987c3dc116f31477f6c97fb23d0c3a4a22f2f3c`：[38607571844](https://github.com/minexin/RFSim/actions/runs/38607571844)
- 均匀线共振修复 `a2bbfd5dd3366082620cbd0ebf99dbf11628bd40`：[38615005226](https://github.com/minexin/RFSim/actions/runs/38615005226)

原失败也保留：pilot `2752af6a5010a7c5ee5fbeddda42c360cf54ff97`
[38579642585](https://github.com/minexin/RFSim/actions/runs/38579642585) 为 16 成功、2 失败；
writer `a88dcc041917e30a620a6f06a98caa56042b5b04`
[38600226155](https://github.com/minexin/RFSim/actions/runs/38600226155) 为 14 成功、3 失败、1 取消。
后续修复不会改写这些历史结论。

逐作业快照由机读映射引用。18 作业是 Windows/Ubuntu/macOS × Debug/Release ×
核心/纯 SDK/归档重放，并非 18 个厂商兼容测试。所有发布门禁保持开放。

## 保留失败与有限结论

- AM/PM：显式校准后的 549 个无告警登记点、37 个告警诊断点及额外端点分开；
  原始解析映射 110/586 的失败仍在。给定实测输出功率的相位吻合不等于网络功率预测通过。
- DataPoints：输出离散候选 8/8 是固定表的有限新配置预测；输入候选 2/8。
  原连续样条及仅名义零旁路反例保留，不导出连续律、精确边界或导数。
- 显式高阶归档 1413 来源项通过不代表自动 RFAMP 系数；0 dB 级联、RFPwrIn、
  衰减器噪声原失败不改判。衰减器约 9.8915667e-7 残差仍超过原 1e-7 阈值。
  频谱削减关闭后恢复的来源不再列为未知缺项。
- 通用 ModelAdapter 和原生 PHD 切片不等于专用器件库或厂商参数翻译器。
  厂商 PHD 格式/系数与条件 A/B 的用户采集仍待证据，没有新增 SystemVue 实测。

原始报告、容差、告警、校准/独立评分边界和历史构建计数不重写。
历史 Python/JSON 叙述按[退役说明](python-interface-retirement.md)阅读；产品 Python 接口已退役，
开发用证据脚本不是恢复 Python 产品。

## 校验与维护

[JSON Schema](../validation/compatibility-evidence.schema.json)使用 Draft 2020-12。
开发检查可运行 `python scripts/reference/check_compatibility_evidence.py --self-test`，无需产品
Python 包或第三方模块。它验证所用 schema 子集、路径、唯一 ID、目录库存、CI 提交/运行绑定、
终态矩阵与成功/失败/取消计数；所有快照均强制逐 job SHA/run/status 绑定。
早期快照逐 job 字段根据独立重新取得的原始 CI 数据补全。
自测注入错误引用、重复 ID、虚假 CI 成功及 job 身份/状态/计数篡改，检查拒绝行为。
此处 checker/schema 是开发证据工具逻辑变更，默认 C++ SDK 构建不依赖它。

该工具只校验证据元数据，不验证自由文本数值/厂商结论真实性，不执行 C++ 数值测试，
也不将归档失败提升为通过。新增实现应有明确契约与独立数值测试，再引用实际记录；
新增厂商通过须有独立场景/原始采集/原容差依据，不能仅改文档中的“通过”。
