# 本地 Codex 长期任务交接（2026-10-16）

## 接手约定与目标

后续开发、构建、诊断及新 SystemVue 实验转回本地 Codex 的**已有长期任务**继续，
不另起并行开发任务。本次云端收尾只整理交接和核验仓库状态，不增加求解器、器件实现、
新数值测试或新 SystemVue 实验；不能把交接完成理解为产品或厂商兼容目标全部完成。

目标保持为 **RF Design 行为器件 → 同一系统组网 → Linear 外部完整 S →
RF System 内部节点/器件端口谱与路径性能**。底层算法为行为和测量服务，不扩大成全 DSP、
GUI 或通用 SPICE。C++17 函数/类型 API 是当前主入口，不恢复已退役 Python 产品包、
wheel 或 Python/JSON CLI；开发用采集、oracle、证据脚本可以保留。

先读 [路线与范围](roadmap.md)、[当前能力及证据](compatibility-evidence.md)、
[SDK 接入](cpp-sdk.md) 和 [同一行为模型/接线表示例](../examples/behavioral-system-sdk/README.md)。
[SystemVue 2023 行为级核验清单](systemvue-2023-behavioral-handoff.md)继续作为资料、
型号及测量语义的技术清单，本页补充执行顺序与接手边界。

## 冻结基线与三类证据

交接准备于 2026-10-16 03:31 UTC 通过 `git ls-remote origin refs/heads/main` 核实：

- 仓库：[minexin/RFSim](https://github.com/minexin/RFSim)。当时远端 main 为
  `aafa10998403d35e57bd9034217fd3d2c945de40`，树为 `f6a46f89847551bac8a58bb566f7e85a822957d1`。
- 工程源码基线 `6ef3b0c0c840d2434b4b59e959b6c93eb4beb211` 的
  [CI 38835703380](https://github.com/minexin/RFSim/actions/runs/38835703380)
  为 18/18 成功，原始 run/jobs 绑定见
  [仓内记录](../validation/compatibility-behavioral-sdk-guard-ci-38835703380.json)。
  `aafa109` 是其后的 docs/validation 覆盖层，不借用源码 CI 作为自身验证。
  收尾于 03:31 UTC 通过 GitHub run/jobs API 另核
  [CI 38837920499](https://github.com/minexin/RFSim/actions/runs/38837920499)：
  精确 head SHA 为上述 `aafa109`，运行完成且 18/18 作业成功（build、SDK、归档回放各六项）。
- 交接提交是此基线后的独立文档提交；不要把上述 SHA 当作之后交接提交的 SHA。
  拉取时另记实际 HEAD、tree 和该提交自身 CI。CI 的平台/配置/作业成功只证明对应测试范围。

必须分开解读：

1. **云端原生实现及工程测试**：C++ 有界契约、独立解析 oracle、安装与重定位 SDK、
   跨平台回归。已有弱噪声块保留、历史放大器/来源/级联重放工具和行为系统 SDK 示例在上述基线中。
2. **归档回放**：从已保存厂商结果重放固定比较，不是新的 SystemVue 运行。
   [归档入口](native-reference-replay.md)及各阶段文档保留已知失败，成功回放不能改变其验收结论。
3. **新的本地 SystemVue 采集**：必须有本次实际版本、输入、设置回读、运行时间、原始输出及来源哈希。
   不能以云端 synthetic fixture、CI 或历史摘要冒充新采集。

## 已完成与明确未闭合的事项

- [RFAMP A/B/C/D 与 C 的 380 独立来源 holdout](rfamp-op1db-pilot-results-20261015.md)
  已完成，不重复索要或无故重跑。单 profile 有界成功不代表专有参数律或完整器件族兼容；
  A/B 辨识、C 独立验证、D 恢复对照的身份保持。
- [DC/RC 后续](dc-bias-systemvue-followup-20261015.md)为八次分析提交、**零项比较验收**。
  NLRES/NLCAP 模型创建失败、频率/探针及边界问题仍在；不能从运行完成或导出谱推断物理 V/I/Y 获验收。
- [四个实际安装 X-parameter 文件](systemvue-xparameter-format-evidence-20261015.md)
  已做格式取证，但 [installed-only 只读重放](installed-xparameter-replay.md)尚未执行。
  四文件仅三个独立内容哈希；格式重放即使成功也不等于数值兼容或一次独立仿真。
- 示例的内部量是带明确方向/频率/驱动/负载的器件端口 RMS V/I 与波功率。
  实际加载 V/I 比不等于增量 looking-in 阻抗或 SystemVue ZPORT；任意图路径归因和厂商测量同名等价未完成。
- VCVS 的暂缓实验候选未合入 main；mixed-mode 仅做过可行性审计，未实现。
  不计作已交付能力；没有明确行为目标映射之前不自动恢复。
- 原 writer、噪声、行为示例严格目标清单等历史 CI 失败及厂商失败继续保留。
  修复后的成功不能覆盖失败原件、调宽旧容差或改变独立样本身份。
- 许可、公开分发、版本/标签及稳定 API/ABI 决策未关闭；这次上传不等于发布 SDK 或作出稳定性承诺。

## 云端源码收尾审计

2026-10-16 03:32 UTC 对当前可见云端项目目录及共享目录作只读审计：发现三个有效 Git 仓，
未发现尚未上传的正式产品源码。此结论仅覆盖当前可见文件，不推断恢复前丢失的内容。

- 主 checkout 与本次独立交接 clone 均以 `aafa109` 为基线。旧 frequency-dependent-cable
  工作区的 12 个未提交改动逐文件 blob 对应已在 main 祖先中的 `e47ea901a2ded2528f7e26502d8635f4d599ab24`，
  无需重复上传。
- frequency-dependent-cable、Touchstone 2、mixed operating points 和 consumer capture 的
  既有补丁 stable patch-id 分别对应已上游 `e47ea90`、`fc7f1e3`、`02ac402`、`4112238`。
  两个 amplifier 修复目录的 publication-manifest 文件 blob 分别对应已上游 `bf5a5560`、`4303efb8`。
- 仓外 cable-physics-review、mixed-op-review、rfmodel-amplifier-msvc-fix、
  rfmodel-amplifier-msvc-debug-fix 中的 probe/control/mutation，以及根目录的
  cable-double-oracle.cpp、cable-header-probe.cpp、cable-numeric-probe.cpp、cable-mutations.py，
  是残留实验/审查工具，不是待补交的正式产品源码；是否已全部归档无法确认。
  本次保留原状，不盲目上传或并入 main，不承诺能从 GitHub 取回这些仓外文件。
- 暂停的历史 VCVS 候选 `cec1e28299ee1186c7bd0b982886b2a1c478267f` 在三个仓内均无可读对象，
  当前搜索范围未找回对应目录。它未发布，不能作为可接手代码；将来确有目标映射需求时，
  先重新核定契约再重新实现。mixed-mode 同样不列为已合入能力。

审计未删除、修改或推送上述残留。交接文档不会把旧 dirty 工作区和 Python 打包产物重新作为产品源码提交。

## 本地接手步骤

### 1. 先保护现有工作，再同步

在已有长期任务中确认实际项目路径、分支、HEAD、remote 和 `git status --short`，
并检查是否有进行中的构建、采集或未保存 SystemVue 工程。不要猜本地路径或另建并行工作者。

先 `git fetch origin` 读取最新远端，再查看与当前分支的差异。只有工作区干净、分支关系明确且可快进时，
才使用 `git merge --ff-only origin/main`。若有本地修改、未跟踪文件或分叉，先列出并保存现状，
由原长期任务协调整合；不要自动 stash、reset --hard、clean、覆盖文件或强推，也不要把本地修改当垃圾删除。
云端旧主 checkout 的 31 项未跟踪 Python build/egg-info 产物已保留，未随本次交接恢复为产品代码。

记录接手后的实际提交和工具版本，阅读该提交 CI；需要本地构建时使用新输出目录。
按 [SDK 构建说明](cpp-sdk.md) 与 [当前 CI 配置](../.github/workflows/build.yml)选择配置，
先复现已有 C++17/installed-only 行为示例，保留失败日志，不把这一步称作新的厂商验证。
不要运行旧文档的 Python 产品构建流程。

### 2. 优先核清行为与测量语义，再做最小实验

按 [技术核验清单](systemvue-2023-behavioral-handoff.md)先读取实际安装的 2023 帮助，
确认版本/build、分析支持、端口顺序、完整复 S、参数单位、参考阻抗、噪声及谱约简设置。
优先核 Linear 外部端口与 RF System 节点/端口/路径口径，再核具体器件参数到行为的映射。
把“帮助定义”“实验观测”“解析 oracle”“当前实现选择”分开记录。

选择帮助明确支持的一条最小行为链，沿用同一器件与接线表分别检查外部完整 S、指定内部端口及
带源/负载/方向的路径量；再加旁支、失配与复参考案例。先冻结输入、参考面、独立 oracle 与阈值，
后运行比较。没有证据的型号/测量保持未验证，不以新增示例或通过计数替代厂商覆盖。

### 3. 单独关闭 X 文件格式重放缺口

在本地确认实际 SystemVue 安装路径和已安装的 C++17 SDK，严格按
[只读重放步骤](installed-xparameter-replay.md)运行现有工具，输出到新目录。
工具只读取指定四文件，不执行 SystemVue；任何版本/hash 不匹配保留为 identity mismatch，
不以本次输出重建历史期望。缺少工具或 SDK 时记录 blocker，不回退到源码头文件冒充安装包。
成功后仍需另行建立系数/归一化/泵相位/端口与参考面的数值映射及独立实验。

### 4. DC/RC 保留阻碍，避免重复无效实验

先解决可支持精确 I(V)/Q(V) 且能导出求解节点 V/端口 I 的参考后端或厂商示例，
以及 RC 的频率响应、探针加载与实际边界。不要重复提交原八次失败配置当作进展。
若需要改变目标后端或产品范围，先列明证据与待决定事项；现有原生数学测试不能关闭该厂商验收。

## 证据可用性与重建规则

环境恢复后，下列五个旧仓外工作目录在当前云端不可见：
`behavioral-sdk-guard-evidence`、`stage-evidence-overlay-publication`、
`stage-evidence-refresh-preparation`、`cascade-source-final-evidence`、`behavioral-system-final-v2`。
不能声称其原始工作文件仍可读取或可交付。Git 跟踪的 docs/validation、归档和哈希记录仍在；
旧文档中“完整日志保留在外部”等描述只作当时状态记录，不是当前访问保证。

接手时逐一核实需要的仓内归档、CI artifact 和本地原始文件是否实际可读；哈希索引不能替代原件。
需要重新构建或采集时放在新的时间戳目录，记录来源提交、工具/厂商版本、命令、时间、状态与哈希，
明确标作新运行，不能回填为丢失历史日志。保留原失败与原验收阈值，不修改历史证据来补齐缺口。

完整厂商帮助、QCH、专有安装样例、许可证和凭据留在本地，不上传 GitHub。
公开交接只保留必要事实性摘要、索引和哈希；每次上传前检查待提交文件范围。

## 下一次汇报的完成标准

本地长期任务的下一次汇报应包含：实际同步提交/树、本地修改保护情况、已核定义及来源、
选定最小实验与冻结阈值、新结果/失败原件位置、四文件重放状态、仍未闭合的差异和下一项阻碍。
每个结果明确属于原生回归、历史回放还是新的 SystemVue 采集；没有执行的检查直接标未执行。
云端交接后不继续并行开发，后续进度由本地长期任务产生。
