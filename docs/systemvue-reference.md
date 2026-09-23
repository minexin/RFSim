# SystemVue 2023 本机参考入口

已在本机只读核实：

- 安装目录：`C:\Program Files\Keysight\SystemVue2023`
- RF 样例：`Examples\RF Architecture Design\RF Design Kit`
- 自动化封装示例：`Scripting\Excel_to_RF\C# Source Code\SystemVueNET\SystemVue.cs`（相对于上述 RF 样例目录）
- 帮助文件：`Help\systemvue.qch`、`Help\programmers64.qch`

厂商 C# 示例包含 `RunScript`、`OpenWorkspace` 与 `GetData` 方法，可作为确认脚本 API 的入口。该目录中还有 RF Link、放大器压缩、互调及噪声案例，可用于后续能力细分。这里只记录本机路径和发现结果，不将厂商源码或示例复制到仓库。

下一步需要读取相关帮助，确认独立实例启动、分析执行和结果导出契约；随后生成自有最小工作区、记录 SystemVue 版本和输出，并与 RFModel 的同参数结果逐项比较。

尚未启动或执行 SystemVue 对照仿真；安装资料存在不等于许可或自动化通道已验证可用。此前按大类列出的矩阵仍只是规划，不能代表 RF Design 库的完整模块清单。

## 2026-09-22 自动化运行核验

本机帮助 `users/Calling_Scripts_From_External_Programs.html` 确认 COM 名称由 Genesys 与 SystemVue 共用；RunScript/RunScriptFromFile 可执行脚本，分析对象提供 RunAnalysis。不能仅凭 ProgID 名称区分产品，必须检查 LocalServer32。

已实际提交与 `scripts/reference/probe-systemvue.ps1` 相同的探测命令，探测前没有 SystemVue 进程。启动后观察到 PID 624，无主窗口标题；执行工具会话 58675 仍未返回 JSON 或错误。尚不确定阻塞在激活还是工作区查询阶段，不能推断许可失败，也不能宣称自动化调用成功。

当前没有打开、修改或保存工作区。脚本只在探测前无 SystemVue 实例且返回工作区数为零时调用 Quit。脚本内没有 COM 激活超时机制，后续无人值守运行需由外部进程管理设置超时；本次应继续观察已存在的调用，不要再启动第二个探测。

后续仍需确认分析对象与数据路径、生成自有参考工作区并导出 S/CS。此探测不等于仿真结果验证。

## 探测终态

工具会话 58675 已退出（退出码 1）。失败发生在 New-Object/COM class factory 激活阶段，HRESULT 为 0x80080005（CO_E_SERVER_EXEC_FAILURE），未获得 Application 对象，也未调用工作区查询或仿真。上面的“仍未返回”记录为先前观察快照，现已终结。该错误不能单独证明许可不足，后续应检查 SystemVue 启动与 COM 注册环境。

## 2026-09-24 重新核验

观察到已有 SystemVue 进程 PID 21144，进程路径经只读 CIM 查询确认指向 SystemVue2023/Bin/SystemVue.exe。没有启动第二个实例或关闭现有进程。

Windows PowerShell 的 GetActiveObject("Genesys.Application") 能取得非空对象，但直接动态访问 Manager 和 Application 均得到 null，工作区数量查询失败。不能仅据此判定产品不支持自动化；可能涉及默认 COM 接口或实例状态，仍需强类型接口核验。

已新增 `scripts/reference/inspect-active-systemvue.ps1`，引用安装目录自带 Interop.GENESYS.dll，以强类型接口只读查询已有实例。脚本不创建实例，不打开、保存或关闭工作区。首次尝试因自动审批额度失败而未执行；本次正常审批后，Windows PowerShell 的脚本执行策略阻止加载，仍未到达强类型 COM 调用。未修改系统执行策略。该脚本尚未完成运行验证。

不受上述阻碍的本机帮助目录提取已完成并重复核验，详见 rf-design-catalog.md。迄今尚无 SystemVue 仿真输出或数值兼容结论。

## 2026-09-24 强类型只读连接成功

用户明确授权执行 SystemVue 比对相关命令后，使用 Windows PowerShell 5.1 的进程级 `RemoteSigned` 策略执行本地检查脚本成功。没有更改 CurrentUser/LocalMachine 执行策略，也没有重新激活或关闭 SystemVue。

```powershell
& 'C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe' `
  -NoProfile -NonInteractive -ExecutionPolicy RemoteSigned `
  -File D:/project/RFModel/scripts/reference/inspect-active-systemvue.ps1
```

厂商 Interop.GENESYS.dll 强类型 Application.Manager 返回 1 个工作区。进一步以 IItem 只读枚举，名称为 Data Flow Template，顶层对象计数为 8，分别为 Analyses、Designs、Graphs、Note、Page、TuneList、Variables、WorkspaceVariables。这证明已有实例的管理器与对象访问可用，先前动态 PowerShell 返回 null 的现象不能视为 COM 接口不可用。

检查脚本仅附着现有实例，最多读取每个工作区前 100 个顶层对象名称，并释放取得的 COM 引用；不会调用 Save、RunAnalysis、FileOpen、Quit 或修改工作区。当前仍没有运行参考仿真，因此这次成功是自动化入口验证，不是数值兼容验收。下一步在项目内独立的参考工程副本中运行最小线性用例，保留原已打开工作区。
