# SystemVue 2023 脚本驱动边界

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
未误报成功。尚未通过该新入口完成四级工程的新鲜数据采集，不把失败路径测试
当作参考仿真验收。
