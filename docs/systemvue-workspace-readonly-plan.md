# 下一次 SystemVue 只读工作区诊断方案（待单独批准）

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

状态：仅准备，未执行本方案的 COM 调用。前次授权的进程级 RemoteSigned 已使用完毕。用户离开或锁屏期间不解锁、不改变锁屏/休眠设置，也不发起依赖界面或弹窗的参考仿真。

## 本次依据与已知状态

上一轮 2026-10-12 02:45:37 UTC 的只读探测成功连接既有实例：SystemVue PID 21556，窗口响应正常，GetWorkspaceCount 返回 1。这是历史快照，不能把下次的 PID 或工作区数量固定为这些值。工作区名称、文件路径及修改状态尚未读取。

本机 Interop.GENESYS.dll 来自官方 Excel_to_RF 样例目录，SHA-256：
`5326e6d22c9d4328526255483d0d8f4c1eb31137fa130b3ac762d6667e505666`。
本轮只用反射读取类型/方法签名，没有创建 COM 对象。

| 需要的信息 | 已核实接口/依据 | 本方案处理 |
|---|---|---|
| 工作区数 | IManager.GetWorkspaceCount() → Int32 | 读取，限制 0..4；超限停止 |
| 枚举工作区 | IManager.GetWorkspaceByIndex(Int32) → Workspace | 仅按合法索引读取 |
| 逻辑名称 | 转换为 IItem 后 GetName() → String | 记录，不能作为磁盘文件名 |
| 根对象可用变量 | IItem.GetVarCount()、GetVarName(Int32)、GetVarType(Int32) | 仅列名称和类型，最多 64 项；不调用 GetVarValue |
| 根对象可用方法 | IItem.GetMethodList() → String | 记录方法描述，不执行描述中的方法 |
| 完整文件路径 | 当前 IWorkspace/IItem 元数据未提供直接路径 getter | 输出 UNKNOWN，不拼接名称猜路径 |
| 已修改/未保存状态 | 当前已核实接口未提供 dirty/modified getter | 输出 UNKNOWN，不由窗口标题、工作区名或空白界面推断 |

官方本地帮助 `tutorials/Exploring_the_Workspace_Using_Visual_Basic.html` 明确逻辑名称可能不同于实际文件名；页 SHA-256 为 `4372ba16f5ecda06034bf207720dab031aa638eb83c04530f87136715ab70b0a`。

`users/Python_Workspace_Class_for_External_API.html` 文档确实列出 workspace_path，但属于另一套 External Python API，尚未核实与当前已绑定 COM 工作区的可靠对应；本方案不切换绑定路线。页 SHA-256 为 `3a983f381fbb0b56cde3222c332e7f8e4e70fe62bfdd2bf5114b61a1c9fc0231`。MATLAB 脚本函数 getworkspacedir 只承诺目录，不等同于完整文件路径；本方案不执行脚本求值。

## 可审阅草案

本地草案：[systemvue-workspace-inventory-pending.ps1](../build-reference/systemvue-workspace-inventory-pending.ps1)。

SHA-256：`b23ba84e39c200d5f28764cee770783282e63fbaed4c1e815793120c67b79166`。

该文件仅通过 PowerShell 静态语法解析；没有运行、没有现场 COM 验证。它保留已验证的 GetActiveObject/Manager 绑定方式，再加入上表中的只读枚举。草案位于忽略的 build-reference 中，本计划文档保留其摘要，不能直接把草案单独运行而省略外部超时监督。

## 单一诊断子进程执行流程

1. **监督端只读预检**：重新按名称查询 SystemVue 进程、PID、窗口标题与 Responding。仅存在一个实例且响应正常才继续；多实例、无实例或无响应均停止。复核草案/Interop 哈希、签名和 Zone.Identifier，以及实际 Windows PowerShell 的 MachinePolicy/UserPolicy；出现新的强制设置、阻止标记或内容变化则停止，不自行解除。
2. **一次子进程**：新建唯一输出目录，拒绝复用已有证据目录。记录开始 UTC、命令、草案哈希、观察到的应用 PID 及新辅助 PID；随后以 Windows PowerShell 5.1、NoProfile、NonInteractive、进程级 ExecutionPolicy RemoteSigned 启动该草案。RemoteSigned 仅覆盖这一个诊断子进程，不修改 CurrentUser/LocalMachine 等永久策略。
3. **绑定**：Marshal.GetActiveObject("Genesys.Application") → 转换 Application → Application.Manager。仅连接已运行实例，不 CreateInstance。观测到的单一应用 PID 是外围身份记录，不能声称 GetActiveObject 返回了一个经独立 PID 查询确认的对象。
4. **读取清单**：GetWorkspaceCount；每个索引 GetWorkspaceByIndex → IItem → GetName、GetVarCount、GetVarName/GetVarType、GetMethodList。名称、变量和方法字符串上限 32768 字符。文件路径与修改状态固定输出 UNKNOWN。每个调用前写 BEGIN，返回后写 OK，异常写 ERROR 和 HRESULT；所有记录带 UTC 并立即 Flush。
5. **一致性检查**：结束前再读一次 GetWorkspaceCount；发生数量变化即判定快照不稳定并停止。即使数量不变，也不声称这是原子快照或工作区身份从未变化。
6. **外部 15 秒总限时**：从辅助进程创建起计时，包含脚本准备和所有 COM 读取。采用先前验证过的 Popen 句柄及 wait(timeout) 监督；只在超时时终止本次亲自创建的辅助进程，绝不按历史 PID 或进程名批量终止，也不终止 SystemVue。保存最后一个 BEGIN/OK/ERROR；不自动重试。
7. **返回结果**：保存新日志/状态文件，报告每个名称、可用接口元数据、UNKNOWN 字段和耗时；只读检查应用响应与辅助进程退出。若超时或疑似弹窗阻塞，保留应用与恢复文件，等待用户处理，不追加自动化。

任何步骤异常、界面需求或超时均在本阶段停止。没有 FileOpen、RunScript、RunAnalysis、Save、SaveAs、CloseWorkspace、Quit、对象属性写入或安全设置持久修改。

## 为何暂不扩大到参考验证

既有 [inspect-reference-workspace.ps1](../scripts/reference/inspect-reference-workspace.ps1) 第 289 行起明确拒绝在非空实例中打开参考副本；已有本地记录确认打开可能替换当前工作区并触发保存提示。IManager.FileOpen(String) 的存在不能证明它会以非破坏方式另开工作区。IWorkspace 还暴露 Save、SaveAs、Update、DeleteAllInternal 等方法，均不在本次只读清单内。

现有 preserve-close-reference.ps1 会保存并关闭工作区，不适合用于判断当前未知工程是否安全。本轮不会调用它，也不会把恢复文件视为可以清理的临时物。

因此，**下一次批准范围仅为上述只读工作区清单**，不包括打开参考副本、运行小案例或导出新仿真数据。只有当前工程的身份、完整路径、保存状态及安全切换方式有明确证据后，才能另行制定有界参考验证方案。当前不能承诺无损打开参考副本。

## 待批准范围

仅授权一次独立 Windows PowerShell 子进程使用 RemoteSigned，执行上述固定只读清单，总时限 15 秒；超时时只结束该辅助进程，不操作 SystemVue 工程，不修改永久安全策略。上次授权不复用；本方案在用户再次确认之前保持待执行。

本计划只在本地保存。任何推送前，先汇总拟推送的提交、验证结果及未完成项，再取得用户确认。
