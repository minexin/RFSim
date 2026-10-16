# 本机 SystemVue 2023 帮助与下一采集入口

已找到 `C:\Program Files\Keysight\SystemVue2023\Help` 下的安装帮助库。
本机 exe 的文件版本为 `2023.0.0.11903`。完整厂商文件留在安装目录；
公开仓库只保存[来源哈希、事实摘要及采集准备索引](../validation/systemvue-installed-help-readiness-20261016.json)。

| 文件 | 内容与实际计数 |
| --- | --- |
| `systemvue.qch` | 产品帮助：15,847 个资源，其中 6,273 个 HTML 资源 |
| `programmers64.qch` | 开发帮助：416 个资源，其中 374 个 HTML 资源；已检查的索引主要为 C++ ModelBuilder/Engine |
| `systemvue64.qhc` | Qt 帮助集合及索引 |
| `WJ Double Balanced Intermods.pdf` | 本轮仅确认文件存在，未作为已读依据 |

计数来自 SQLite `FileNameTable`；总行数、不同 Name 和不同 FileId 数一致，
HTML 按 `.html/.htm` 后缀过滤，因此上述数字没有重复别名计数。
这不是功能覆盖率，也不表示全部页面已读。此前核验 27 页测量/模型帮助，
本次补读 13 页模型与自动化帮助，共 40 个不同路径；记录均绑定解压正文 SHA-256。

下一次[同电路 Linear / RF System 实验](systemvue-linear-rfsystem-minimal-plan-20261016.md)
有了更明确的入口，冻结数值和比较门限保持：

- `rfdesign/MultiSource.html` 明确同时支持 System 与 Linear 仿真。
  下一步可准备 MultiSource、两只 RES 与输出端口组成的单层共享设计，保留 MID 节点可见；
  这是帮助支持的候选，还没有运行验收。
- `users/Script_Verbs.html` 给出 COM `RunAnalysis`、`IsAnalysisDone`、参数 `GetValue`
  和数据集 `ExportS`。`GetValue` 的 MKS 值与界面单位必须分开保存；
  `Set` 使用显示单位，不能把字符串 `100` 直接推断为 100 Hz。
- Linear 应同时保留 F、ZPORT、完整复 S、四项别名和 ExportS 输出。
  旧一端口数组不能证明两端口轴序；对称 RES 的数值也不能用来辨识转置或轴交换。
- RF 要先回读路径定义、方向、频率、单位和数据集身份，再提取 DCP/DCV。
  `Manager.Update` 会运行待处理分析，不能作为只读轮询方法。
- 外部 Python API 是可选的结果读取路线：本机文档要求 Python 3.10.x，
  `Variable.value` 不支持矩阵，Analysis wrapper 只覆盖 Sweep/Data Flow。
  本轮未安装、导入或运行该包；下一实验仍优先沿用有来源依据的 COM 路径。

索引还核验了八份自有辅助文件，与已公开的
[`dc-systemvue-followup-raw-20261015.zip`](../validation/dc-systemvue-followup-raw-20261015.zip)
成员逐字节一致。它们提供构件初始化、分析创建和原始导出入口，
但带有旧会话、路径或未验收的解释，不能原样重跑。

下一次先新建准备/设置回读步骤，确认 Path0 的枚举与解析节点、两端口复数数组布局，
并使用新数据集名及时间戳。身份明确后才分别提交一次 Linear 与 RF 分析。
本次只做本机文档与文件核验：没有启动应用、调用 COM 或增加厂商分析次数；
历史 DC/RC 零项验收与 X 文件仅格式成功的边界保持。
