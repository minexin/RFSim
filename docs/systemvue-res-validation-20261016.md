# SystemVue 2023：同一 RES 电路的 Linear / RF System 首次采集

2026-10-16，在本机 SystemVue **2023.0.0.11903** 中完成两次独立提交：
`RES_Linear` 一次、`RES_RFSystem` 一次。两次均完成且应用错误为空。
这次推进把此前的方案变成了可重放的实际结果，并增加了使用安装版 C++17 SDK 的独立探针。
最终 **5 项通过、0 项数值失败、5 项身份未闭合**，最小实验整体验收为否。
不能把数值接近解释为身份缺口已经解决。

## 冻结范围与结果

采用[采集前冻结方案](systemvue-linear-rfsystem-minimal-plan-20261016.md)，
原始 JSON 的 SHA-256 始终为
`4000c4549f5ed0a129f1f5ccf1069d4ef4c36be6da9c5de04d86b441f9d4a30e`。
没有修改门限、增加验收量，或用厂商结果反推解析基准。

电路为两个串联的 50 Ω `RES`，单频 100 MHz，源可用功率 1 mW，源、负载均为 50 Ω。
分析温度 26.85 °C，器件 Ta 空白，噪声及非线性谱生成关闭。
RF 通道宽度固定 1 kHz。应用载入后重编号为 **IN=1、MID=3、OUT=2**；
真实端子接线与 `NodeNames=[1,3,2]` 一致，不能沿用生成前的节点顺序。

Linear 七个候选点为 F、两个复数 ZPORT 和四个复数 S；RF 三个候选点为首节点 DCP、
输出 DCP、输出 DCV。诊断变量、形状检查和单位检查不另计验收点。
最终逐项状态及原始值见[机器结果](../validation/systemvue-res-validation-20261016.json)。

| 观测量 | 本次实际回读 | 解释 |
|---|---|---|
| F | 100,000,000 Hz | 原始 Data/GetValue；显示单位为 MHz |
| S11、S12、S22 | 0.5 + j0 | 各自具有 `S(:,i,j)` 公式及完整标量回读 |
| S21 | 0.5000000000000001 + j0 | 与 0.5 相差一个 binary64 ULP；保留差异 |
| 两个 ZPORT | 原始复数数组保留，未自行解码 | 端口轴与实虚轴缺少明确映射；两项 `not_comparable` |
| 首节点 DCP | 0.001 W | 源内部可用功率参考面；不是 DUT 实际送入的 0.00075 W |
| 输出 DCP | 0.00024999999124999993 W | 已确认节点与前向 desired 测量身份；源类型绑定仍待补齐 |
| 输出 DCV | 0.15811388024142597 V peak | 已确认 peak 定义；不当作 RMS，不对原始值再做 dBV 转换 |

RF 三项保留为 `not_comparable`：本次 `MultiSource/SrcType=0` 已回读，
但官方帮助及可读模型定义没有提供明确的 **0 → CW** 枚举映射。
自定义名称 `RES_CW`、谱名和输出数值都不能替代这项身份凭据。
Normal 模式有本次运行日志支持；路径、desired/forward、完整通道覆盖、
首节点参考面与 V peak 定义的其余核查已完成。

`ExportS` 另输出 Touchstone `MHz / S / MA / R 50`，顺序按官方文档解释为
S11、S21、S12、S22。该文本把 S21 最后一位舍去，只用于交叉核对，
正式比较使用完整精度的标量回读。没有从四个 S 值相近推断数组轴。

## 采集、失败保留与证据边界

两次唯一 `RunAnalysis` 的 UTC 提交时间分别为 12:00:16.9594993 与 12:01:03.5267098。
Linear/RF 完整快照分别读取 305/456 个对象，保存数组维度、下界、显式索引、
CLR 类型、binary64 位模式、往返文本、前后 XML 和应用错误。
快照前后 XML 与错误相同；参数 getter 仍可能求值，不能称为无副作用或原子快照。

准备阶段曾尝试规范化 Path Definition 的读回尾引号，遇到表达式错误、字符串 SetValue 类型错误，
以及在直接改 Param XML 后重载时的应用崩溃。所有尝试和错误保留，期间没有提交分析。
用户关闭错误报告窗口后重新启动应用，从此前保存的初始化输入恢复，随后完成上述两次分析。
这些记录不证明一般性的 SystemVue 缺陷，也不把尾引号本身判定为无效。

另有一次 ZPORT 后处理的 sandbox 连接未取得运行对象；随后正常宿主权限取得对象并保存独立副本，
但创建公式对象返回 `0x8009000D`。没有调用 Calculate/GetValue，亦没有追加 RunAnalysis。
后续一次针对实际对象名 `InitGen` 的修正成功执行 SetName，但设置 Text 返回
`0x80020005`，仍未计算或读取新的 ZPORT 标量。三次后处理尝试均保留，
八份受保护的原始采集文件哈希不变；两个 ZPORT 继续不可比较。

实际运行使用的 runner 和 v2 collector 均有原始哈希绑定；早期 v1 collector 已按原哈希恢复。
初始化器旧源码也单独保存，但原初始化 receipt 未记录源码哈希，因此该关联只能说明保存的源码版本，
不能补称为当时已有的密码学绑定。后续加强的初始化器与 runner 单独标为当前版本，未重跑厂商分析。

公开[原始归档](../validation/systemvue-res-raw-20261016.zip)与
[逐文件清单](../validation/systemvue-res-manifest-20261016.json)保存自有实验工作区、原始读回、
失败记录、脚本版本、独立检查和帮助事实摘要。完整安装帮助、示例文件、二进制、许可证、
凭据及崩溃转储没有复制到仓库。安装示例的完整 XML 片段与帮助正文仍留在本机，公开副本仅保留事实和来源哈希。

## 原生实现及重放

[C++17 探针](../scripts/reference/behavioral-res-probe/README.md)用同一组 `ModelAdapter` 和连接表，
分别调用 `analyze_linear` 与 `analyze_conversion_simulation`，并列出源可用、DUT 实际送入及负载收到的功率。
它是两个翻译单元、只引用独立安装 SDK 的消费者。没有添加 Python 产品入口，也没有修改核心 C++ 求解器。

本地 MSVC 19.44 的 Release/Debug 各通过 99 项解析、身份和形状检查；
12 个错误结果变异被拒绝。99 是检查数，不是 99 个独立物理案例。
两种配置结果文件的 SHA-256 都为
`b34a16bcc77da106c3e385442ab2356e11b3bfdd220c8ea971768c95c6e6d570`。
原生 oracle 使用基本串联电路方程，未调用 SystemVue 或从其结果生成期望值。

归档重放的 21 个错误证据变异被拒绝，覆盖错误单位、数据集绑定、源码哈希、
索引下界、位模式、器件接线与反射符号。输入生成测试 4/4；PowerShell 5.1 的解析、
C# 编译与托管假对象检查 40 项、纯文件预检 42 项、最终绝对路径修复专项 7 项通过，
这些离线检查没有连接应用或追加分析。开发检查本身的早期失败也保留，不记为厂商物理失败。

以下离线命令验证归档哈希、观测量身份和原门限，不启动 SystemVue：

```text
python scripts/reference/replay-systemvue-res-evidence.py --archive validation/systemvue-res-raw-20261016.zip --manifest validation/systemvue-res-manifest-20261016.json
python scripts/reference/test_systemvue_res_evidence.py --archive validation/systemvue-res-raw-20261016.zip --manifest validation/systemvue-res-manifest-20261016.json
python scripts/reference/test_systemvue_res_input.py
```

CI 增加上述归档重放、输入生成检查与安装版 RES 探针，沿用原来的 18 作业矩阵。
具体提交的运行状态及原始 REST 记录在完成后单独登记，不能借用此前提交的成功状态。

## 后续范围

先闭合当前缺失的身份凭据，再按新方案分别验证 100 Ω 物理负载、无歧义旁支、
坐标参考变化与真实源/负载变化；噪声实验需要独立的归一化和门限。
本次不验收一般 CP/CV/CNV、RF 路径电流方向、ZIN/ZOUT 边界条件、复参考或完整器件族。
既有 DC/RC 八次分析仍为零项比较验收；四个安装 X 文件的格式回放仍是零次厂商仿真。
RFModel 的长期兼容目标保持进行中。
