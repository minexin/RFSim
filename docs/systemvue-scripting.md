# SystemVue 2023 脚本驱动边界

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

当前执行规则以文末“2026-10-12：已验证的调用与比较流程”为准；前面的启动和超时描述保留为历史记录。

后续进展：Python + COM 后台入口已完成首次受控四级链路采集，见
[天线噪声对照报告](systemvue-antenna-comparison.md)。下文的失败记录保留为排障依据。

核对本机安装目录 `Help/systemvue.qch` 的 2023 文档后，确定应优先用脚本驱动，
界面仅用于处理启动或异常对话框。后台自动化不等于无需 SystemVue 进程或许可证。

| 接口 | 本机文档说明 | RFModel 使用方式 |
|---|---|---|
| 内置 Python | Python_Analysis_Class 中的 run() 支持运行分析 | 可在 SystemVue 脚本对象内运行 |
| 外部 keysight.systemvue | External_Python_Script_API 以读取工作区数据为主；外部 Analysis.run() 标记 2023 不支持，只列出 Sweep/Data Flow 分析 | 不把它作为 2023 RF System Analysis 的直接驱动 |
| COM + RunScript | 官方 C# 示例通过 RunScript 执行脚本 | 当前已实测的参数修改、分析及结果回读通道 |

官方内置 Python 示例的核心形式是：

```python
import keysight.systemvue as systemvue
app = systemvue.Application()
ws = app.current_workspace
ws.analyses["System1"].run()
```

这段代码应在 SystemVue 内部 Python 上下文使用，不能据此假定从普通 Python 进程调用
也会执行分析。本轮尚未实测内置 Python 的 RF 分析路径。官方文档建议 Python 3.10.x，
安装目录提供 `Python/keysight_systemvue-2023.0-py3-none-any.whl`；不擅自升级安装环境。

本机随附 C# 包装器 `Examples/RF Architecture Design/RF Design Kit/Scripting/Excel_to_RF/`
`C# Source Code/SystemVueNET/SystemVue.cs` 的 OpenWorkspace 方法通过 VBScript 执行
`OpenWorkspace("完整路径")`。这与 Manager.FileOpen 不同，后者此前会触发文件选择器。
2026-09-28 已实测 OpenWorkspace 打开 RFModel_AntennaNoise 副本，回读名称正确。
打开工作区可能关闭当前工作区，因此通用脚本仍要求打开前工作区数为零。

当前实现为 PowerShell/C# COM 驱动，已有 Python 采集、校验和比较工具；
后续批处理可用 Python 编排此驱动，不需要依赖鼠标操作，也不必为了使用 Python
而退回到 2023 不支持的外部分析入口。随附旧 Interop.GENESYS.dll 的 ScriptLanguage
枚举仅有 JScript/VBScript；不猜测 Python 枚举数值调用。

## 本轮发现与修正

- 四级天线噪声工程使用 RF Design 文件夹，原单衰减器使用 Designs。分析入口已分开，
  新增 RunAntennaAnalysis，禁止混用单衰减器参数覆盖。
- 错误脚本被取消后，COM 可能正常返回且 Manager.GetErrors 为空，数据却仍是
  时间戳 1584015817 的旧结果。因此采集端现要求目标数据集时间戳落在本次运行区间，
  与后续 Python 校验形成双重检查；旧结果不得作为新实测发布。
- 已只读看到四级工程的五节点 CF/CGAIN/CNF/CND/DCP，但该次分析缺少完整受控运行
  时间记录，不纳入正式比较基准。修正后的完整重跑遇到 MK_E_UNAVAILABLE，尚未验收。

文档依据（均来自本机 QCH）：`users/Python_Scripting.html`、
`users/Python_Analysis_Class.html`、`users/Python_Application_Class.html`、
`users/External_Python_Script_API.html`、`users/Python_Analysis_Class_for_External_API.html`。

## Python 后台采集入口

`scripts/reference/run-systemvue-reference.py` 通过参数列表启动 COM 采集子进程，
无需鼠标操作。先在本机准备专用 SystemVue 实例和参考副本，再执行：

```powershell
python scripts/reference/run-systemvue-reference.py antenna `
  build-reference/RFModel_AntennaNoise.wsv build-reference/antenna-run-001 --timeout 120
```

单衰减器用 `attenuator` 和对应文件名。`--open-copy` 仅用于没有工作区的已启动实例，
由采集脚本再次校验。不会自行创建/关闭 SystemVue 实例，不会修改系统 Python 配置。

每次使用新的输出目录，保留 `capture.json`、`stderr.log`、`status.json`。
状态中的 `captured` 只表示目标数据集的新鲜度通过，不代表数值兼容通过；
参数、测量向量和 RFModel 比较仍由后续校验器负责。
目录已存在则拒绝覆盖，子进程失败返回 1，超时返回 124。

超时状态为 `timeout_unresolved`，记录 collector_pid；采集进程和 SystemVue 分析
可能仍在运行。此时先检查该 PID、日志和 SystemVue 状态，不得直接重试、重启或把
超时解释为分析失败。驱动不会杀死进程或自动重试。人工处理结束后另建新目录采集。

2026-09-28：后台驱动测试 3/3、既有采集比较测试 9/9 通过，新增测试接入三平台 CI。
在本机无 SystemVue 实例时实测返回 collector_failed/退出码 1，保留 COM 错误日志，
未误报成功。该段是初始失败路径验证，后续已完成新鲜数据采集，见上方对照报告。

2026-09-29：在没有 SystemVue 进程时，通过已确认的本机
`C:/Program Files/Keysight/SystemVue2023/Bin/SystemVue.exe` 启动隐藏实例，再执行
上述 Python 驱动的 antenna 案例并带 --open-copy，完成 antenna-run-003 新鲜采集。
启动记录保留在 build-reference，采集驱动自身仍不自动启动、关闭或重试实例。
全程没有鼠标输入；分析、参数读取和结果导出均通过脚本完成。

驱动现在支持 --source-power-dbm（有限数，−200 至 30 dBm）。天线案例只改变
Pwr 向量的第一个载波项，第二项保留 −50；衰减器案例仍使用标量参数。
天线案例仍禁止使用衰减器专用的损耗/温度覆盖。改变功率后需要向 collect 提供
同名选项用于实际回读校验。实验结束应显式恢复 −50 dBm 并验证基准。

## 2026-10-09：消除脚本语言识别提示

包含对象赋值和方法调用的旧脚本文本会触发“像 Python，但使用 VBScript 执行”
的语言切换提示。采集器现在明确使用 Dim、Set、Call，继续通过官方旧 COM
接口的 genLangVBScript 调用，不猜测 Python 枚举，也不修改全局语言设置。

本机已实测 SetProperty 设置 ShowTotals 并回读 0 → 1；修正后的正式采集器
完成六组方向总谱及一组关闭总谱的恢复采集，均产生新鲜数据、无 Manager 错误。
CaptureRun 新增 script_language 与 submitted_script；相干案例新增
--coherent-show-totals，省略时显式关闭，避免跨运行残留。
本轮真实重跑覆盖相干案例，未重新运行其他参考工作区或 --open-copy 路径。

## 多源压缩与相消诊断

compression-two-tone 新增 same-frequency、locked、show-totals、disable-noise 控制，
完整拼写、范围和实测见 [共同压缩参考](systemvue-shared-compression.md)。
每次显式恢复频率、参考时钟和分析噪声/总谱设置，避免前次运行残留。

compression-cancellation-diagnostic 只允许同频共时钟双源模式，
保留已确认的两条相消路径警告并标记不可用于验收；与原过压缩诊断互斥。
新比较器同时验证路径/主频谱时间戳、参数、时钟、分析设置和诊断分类。

## 2026-10-12：已验证的调用与比较流程

后续参考任务按本节执行。本节更新前文历史驱动的启动、超时和恢复规则；旧日志保持原义。run-systemvue-reference.py 等六份采集草稿有未提交改动，本轮未覆盖，未将其视为整体验收的新入口。可维护流程先固定在此处；改动草稿前必须逐项查看原差异，再小范围整合。

1. 使用 Windows PowerShell 5.1 原生 powershell.exe，以参数数组启动、隐藏 helper 窗口。子进程 PSModulePath 指向该可执行文件旁的 Modules，防止继承 PowerShell 7 模块导致 AuthManager 错误。2026-10-12 经用户批准的本机 CurrentUser=RemoteSigned 已生效；启动时记录各作用域及有效策略，不再添加 -ExecutionPolicy 或设置 Process 策略，也不自动修改其它作用域。不能假定其它机器配置相同。
2. 绑定既有 Genesys.Application 实例，记录 PID、开始时间、响应状态及工作区数/名称。仅操作确认的专用参考副本。未加载时先核对零工作区；若是用户已确认可丢弃的空白工作区，另做明确身份检查。通过官方 VBScript RunScript 的 Call OpenWorkspace("完整副本路径") 打开，再回读身份及 Manager.GetErrors。禁止用 FileOpen 代替以免弹出选择器，不得覆盖用户现有工程。
3. 原始输入只读，副本位于 build-reference。每次使用唯一输出目录并记录输入 SHA256，拒绝覆盖输出。禁止 Save、关闭并保存或覆盖恢复文件。修改前保存实际参数表达式、解析值、单位、局部温度和分析选项，结束时恢复并回读；空默认值不能冒充已解析值。
4. 优先查询已有分析 IsAnalysisDone，核对输入及结果归属。已完成结果优先导出，不为补日志自动重跑。确需新分析时，用明确 genLangVBScript、Dim/Set/Call 生成脚本，经 RunScript 修改受控参数并调用 RunAnalysis，记录调用/返回 UTC、HRESULT/异常和 Manager 错误。仅正常返回或错误列表为空不足以确认仿真成功。
5. 每个 helper 从启动起设 120 秒硬上限；超时只终止本次持有进程句柄对应的 helper，记录 PID、耗时和最后阶段。不得按名称杀 PowerShell 或 SystemVue。SystemVue 可能继续分析，超时不是仿真失败证明；不自动重试、重启或关闭应用。helper 被终止时 finally 未必执行，参数恢复必须另行核实。
6. Disabled 主窗口单独出现不代表弹窗。#32770 类也不能单独代表错误：本轮 “System1 Simulation Status” 曾造成提前中断，随后读回证明分析已完成。记录窗口标题、类、PID及观察时间，区分已知进度窗口和需要处理的提示，不盲点对话框。未知阻断时停止新写入并保留证据，不更改安全或许可设置。
7. 采集实际参数、有效温度、分析设置、原始节点/频谱 V/Z/P、完整测量向量、数据集时间戳、脚本文本、UTC 和输入/输出哈希。检查时间戳属于调用区间；从中断事务恢复时保留原返回时间为空，另记完成观察时间，不能伪造完整日志。参数恢复后遗留的旧数据集须明确标识。
8. 独立验证单位、索引、维度及有限数值，再调用对应 RFModel 探针和原比较器。单衰减器仍采用 1e−7 相对阈值；不改容差、替换 SI 常数或漏掉失败项。等温探针不能冒充非等温结果。分别报告 helper 自动化是否完整、仿真是否完成、数值是否通过。

本轮已验证原生 PS5.1 环境、既有实例绑定、官方 RunScript 打开副本/执行分析、状态查询/节点导出与原容差比较。新实验和恢复细节见[噪声辨识报告](systemvue-attenuator-noise-identification.md)。这不保证未来没有许可证、模型错误、超时或新窗口问题；发生问题按上述边界保留证据和恢复状态，不把历史成功当作当前成功。
