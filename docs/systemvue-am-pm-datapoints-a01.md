# A01：AM to PM Basics 的 DataPoints 有界辨识

2026-10-12：已完成本轮 **24 次分析**，复现两个原始基线，并取得 21 个诊断点。DataPoints 完整契约仍未闭合，不宣称厂商兼容，也未把观察到的异常写入 RFModel 内核。变更仅含离线诊断、证据、测试和文档，本地提交，不推送。

## 结论与反例

| 项目 | 实测结果 | 可作出的判断 |
|---|---|---|
| 原始基线 | PointDerivative、DataPoints 各 37 点与各自历史数据匹配；最大功率相对差分别 2.49e-16、6.96e-16；最大相位差分别 9.94e-17、3.19e-14 deg | 两个数值基线已复现；DataPoints 保留非单调点被忽略的官方警告，不能称为无警告基线 |
| 插值 | `[0,10,20] dBm → [10,40,-20] deg`，名义 5/15 dBm 得到约 33.4375/18.4375 deg | 排除本轮比较的分段线性候选；非零点与 dBm 横轴自然三次样条候选接近 |
| 跨圈 | 170→−170 的中点约 0°；170→190 的中点约 180°；10→370 的中点约 −170°，即 190°模360° | 在测试范围内保留输入角度圈数，未按最短相位路程统一处理 |
| 端外 | −5/25 dBm 处分别约 10/−20° | 所测两端外点保持首/末相位，不等于证明任意端外范围 |
| 源相位 | 源 30°，常值表 20°，输出约 50° | 本例表现为相位相加 |
| **名义 0 dBm 反例** | 四种表均返回 0°，原始电压相位也为 0°；表首值分别为 10°或170° | 不是单纯 DCPH 显示问题，不能删除这些点后宣布契约成立；零 dBm 是约 1 mW，不是零瓦功率 |
| **近零 DCP 反例** | 名义 0.001 dBm：DCP 报表为 0 dBm，原始频谱功率约 0.0009998914 dBm；相位 10.0052494252° | DCP 与原始功率相差约 2.3021e-4（相对值）。预登记 DCP 横轴不能被悄悄替换，内部查表功率定义仍需厘清 |

自然三次样条是在看到首组数据后提出的候选，**不是官方文档给出的算法**。在剩余预算内，先保存[补充计划与预测](../validation/am-pm-datapoints-followup-plan-20261012.json)，再测 2.5/0.001 dBm。2.5 dBm 预测 22.7734375°，实测 22.7734369701°；0.001 dBm 的名义预测 10.00524999998°，实测 10.00524942518°。

按预登记的 **DCP 功率轴**，该候选匹配 16/21 点；另列的 **原始频谱功率轴诊断**匹配 17/21 点，匹配点最大相位残差约 3.30e-6°。后者的四个失败点均为名义 0 dBm；前者还包含 0.001 dBm 报表反例。两个完整集合均失败，没有改变 `1e-5 deg` 容差，没有把失败点从总评中排除。有限样本也不能唯一证明任意表格都采用自然样条。

## 来源、参数与边界

官方 `AM to PM Basics.wsv` 的 SHA-256 为 `ead90a212c786eff0cfa782dab47dbfe14c421b3f43b32c573bb990cb8348877`。专用副本 `build-reference/a01-datapoints-20261012/RFModel_AMPM_A01.wsv` 与原文件始终逐字节相同，没有保存仿真中的参数修改，也未提交官方工作区全文。帮助来源沿用[来源记录](../validation/systemvue-2023-am-pm-table-contract.json)。

原始示例内 `SimulatedAMPMData` 的描述标记为 2015 年；PointDerivative 和 DataPoints 历史扫描数据分别来自 2020 年 3 月、9 月，三者不能当成同轮结果。输出功率尾部有 5 个非递增区间；本轮不修剪它们，而是保留官方警告和历史基线。新鲜基线分别对照自己的历史记录。

[原始预登记](../validation/am-pm-datapoints-plan-20261012.json) 在本轮任何 RunAnalysis 前保存。诊断为单音 100 MHz，G=20 dB、OP1dB=100 dBm、OPSAT=103 dBm、OIP2=140 dBm、OIP3=130 dBm，均核验运行时 SI 值。输出相位表按原始数值角度传入；原始频谱中的电压、阻抗和功率满足 `P=0.5*|V|²*Re(1/Z)`，相位与 DCPH 一致。

| 诊断配置 | 表相位 deg | 输出功率测试点 dBm |
|---|---|---|
| 三节点 `[0,10,20] dBm` | 10,40,−20 | −5,0,5,9.999,10,10.001,15,20,25 |
| 两节点 `[0,10] dBm` | 170,−170 | 0,5,10 |
| 两节点 `[0,10] dBm` | 170,190 | 0,5,10 |
| 两节点 `[0,10] dBm` | 10,370 | 0,5,10 |
| 三节点常值表，源相位30° | 20,20,20 | 5 |
| 观察后预登记的补充点 | 10,40,−20 | 2.5,0.001 |

边界外推的普遍性、折点导数、任意节点数量/间距、精确零瓦功率、非法表、多音和 Compression Curve 均不在已证明范围。完整契约须解释两个零附近反例，并使用不同表格与独立保留点继续检验，不能从本轮结果直接生成“SystemVue 兼容”内核。

## 恢复过程与采集限制

早期打开遇到旧工作区保存提示；用户点击不保存后报告应用崩溃。按用户要求重启，正斜杠路径的 OpenWorkspace 调用又返回内部错误，用户提供了崩溃报告窗口。用户关闭崩溃报告和下一次启动引导后，Windows 原生路径调用成功。此过程只说明修正后的打开成功，不能单独证明崩溃根因；未发送崩溃报告。

初期关闭快速扫描导致官方 `DiffPhase` 方程缺少 Path2 数据。错误已归档，使用预留的一次分析恢复原始基线；首组与跨圈组仍采用旧扫描开关版本，后续组保留快速扫描。版本切换时一次静态替换失败导致跨圈组继续使用旧脚本，实际执行哈希和恢复结果如实保留。没有静默清除日志或把警告重分类为无错误。

共启动 **24 次** RunAnalysis：2 次基线、1 次恢复基线、19 个原定点、2 个补充点。每个采集助手限时120秒。最后 0.001 dBm 点期间，监督器把通用 “Simulation Status” 窗口视为未知窗口并停止了调用助手；SystemVue 未被终止。随后仅查询现有完成状态，成功取回结果并恢复参数，没有第25次分析。该点保留 `run_returned_utc=null` 和真实完成观察时间，不能声称它有完整的同步返回日志。

所有修改的参数表达式与原扫描开关已恢复，恢复后没有再仿真；屏幕上的最新数据仍属于诊断参数，不能当成恢复后的基线结果。原文件、副本和此前8个未提交文件的哈希均保持不变。没有新推送、系统安全设置修改或额外内核变更。

## 证据与复算

- [阶段记录、基线及文件哈希](../validation/systemvue-2023-am-pm-datapoints-a01.json)
- [21 点归一化证据、原始复电压摘要及所有候选残差](../validation/am-pm-datapoints-observations-20261012.json)
- [原始捕获清单与 SHA-256](../validation/am-pm-datapoints-capture-manifest-20261012.json)
- [归档校验器](../scripts/reference/archive-am-pm-datapoints.py) 与 [候选比较器](../scripts/reference/analyze-am-pm-datapoints.py)

原始完整捕获及经实际执行的 PowerShell/Python 助手留在阶段记录列出的 `build-reference/a01-*` 目录。归一化证据随提交保留；重新读取完整捕获需要这些本地文件，不能把仅有哈希称为包含完整原始档案。

```powershell
python scripts/reference/test_analyze_am_pm_datapoints.py
# 仅使用已提交的归一化证据，复算36个线性候选：
python scripts/reference/analyze-am-pm-datapoints.py validation/am-pm-datapoints-capture-manifest-20261012.json build-reference/a01-linear-replay.json --observations validation/am-pm-datapoints-observations-20261012.json
# 本机完整捕获仍在时，重新核验原始证据并计算两种样条诊断：
python scripts/reference/archive-am-pm-datapoints.py . validation/am-pm-datapoints-capture-manifest-20261012.json build-reference/a01-full-replay.json
```

输出路径必须不存在，工具不覆盖证据。21 项测试覆盖数学区分、异常保留、实测横轴、非法表、空/部分观测、时间新鲜度、关键节点唯一性、单位和频率、原始电压/功率一致性、恢复采集的完成证据与输出不覆写；已接入 CTest。当前验证范围是这些离线工具，不冒用历史全量 CTest 或 CI 结果作为本阶段证据。

最终验证：Python 3.12 单独运行 21/21；CTest 使用 SystemVue 附带 Python 3.10，Debug/Release 均为新增测试 1/1（内部21项）。首次 Python 3.10 检查发现 Windows 七位小数 UTC 解析问题，已修复并保留失败日志；只在秒级新鲜度核验时截到微秒，原始时间文字不改。完整原始捕获重放与已提交归一化证据逐字段一致。

后续补充：[既有捕获的真实 C API 与 Quick Sweep 离线诊断](systemvue-am-pm-existing-reference.md)。原始基线复现与 RFModel 匹配是不同检查；补充诊断发现 Point Derivative 原生对比30/37点不匹配，并保留快速扫描的四个零dBm反例，没有新增分析调用。
