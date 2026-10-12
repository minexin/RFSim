# SystemVue 参考会话恢复记录

## 2026-10-12

本轮尝试恢复 RFAMP_HO 参数实测，未取得新数据集，不计入兼容验收。[机器诊断快照](../validation/systemvue-session-recovery-20261012.json)记录观察时间、应用/检查进程身份和工作区哈希，状态只代表该时刻，后续必须重新核实。

- 启动前没有 SystemVue 进程。专用副本 `build-reference/RFModel_CascadeIntermods.wsv` 的 SHA256 与最后一次成功的 `order11-restore-baseline-001/capture.json` 记录一致（`21e4b28eadd10919537434bc2e706d17fcf41e8691cc9406dea36ca69464487f`），没有覆盖旧采集证据。
- 以专用副本路径启动 SystemVue 2023（进程 16008）后，COM 可访问，但 `Manager.GetWorkspaceCount()` 返回 0，命令行启动没有确认加载工作区。
- 通过已有 VBScript `OpenWorkspace` 入口打开同一副本，调用明确返回 COMException“出现内部应用程序错误”。这是已结束的失败调用，不是一次有效分析。
- 本地 Interop.GENESYS 元数据确认 `IManager.FileOpen(string)` 直接入口存在。另一个受保护检查准备在工作区数为零时使用该入口，但进程 4792 在打印任何诊断前未返回，因此不能断言已经调用 FileOpen；没有启动新的分析或重复采集。
- 快照时 SystemVue `Responding=false`，上述检查进程仍存活，日志为空。没有终止应用或检查进程，也没有重启或重复调用 COM。不能根据空日志断定许可失败、脚本语言错误或具体模态窗口原因。

离线回归：`test_run_systemvue_reference.py` 17 项、`test_compare_cascade_amplifier.py build-install-capi/bin/rfmodel_c.dll` 9 项通过。现有 RFAMP_HO 采集草稿尚未得到实际运行验证；离线通过不证明厂商参数切换和数据采集成功。

后续先检查该应用与检查进程的实际状态，处理 SystemVue 会话提示或挂起状态；确认旧检查结束后，再从已验证副本恢复普通 RFAMP 基线，随后逐步验证 RFAMP_HO 模型切换与 IMN。旧 `rfamp-ho-imn3-single-m10-001` 的空采集仍然无效，不覆盖或重新解释为成功。
