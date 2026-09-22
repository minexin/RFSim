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
