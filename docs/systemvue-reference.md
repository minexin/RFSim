# SystemVue 2023 本机参考入口

已在本机只读核实：

- 安装目录：`C:\Program Files\Keysight\SystemVue2023`
- RF 样例：`Examples\RF Architecture Design\RF Design Kit`
- 自动化封装示例：`Scripting\Excel_to_RF\C# Source Code\SystemVueNET\SystemVue.cs`（相对于上述 RF 样例目录）
- 帮助文件：`Help\systemvue.qch`、`Help\programmers64.qch`

厂商 C# 示例包含 `RunScript`、`OpenWorkspace` 与 `GetData` 方法，可作为确认脚本 API 的入口。该目录中还有 RF Link、放大器压缩、互调及噪声案例，可用于后续能力细分。这里只记录本机路径和发现结果，不将厂商源码或示例复制到仓库。

下一步需要读取相关帮助，确认独立实例启动、分析执行和结果导出契约；随后生成自有最小工作区、记录 SystemVue 版本和输出，并与 RFModel 的同参数结果逐项比较。

尚未启动或执行 SystemVue 对照仿真；安装资料存在不等于许可或自动化通道已验证可用。此前按大类列出的矩阵仍只是规划，不能代表 RF Design 库的完整模块清单。
