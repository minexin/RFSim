# SystemVue 2023 双源相干网络参考

本机 SystemVue 2023.0.0.11903 已完成两类采集：早期 30 rad 传输线的同相基准，
以及新增 pi/6 rad、共时钟/独立时钟各 0°、90°、180° 的六组扫描。

**新增扫描的路径复幅度和源时钟相干关系通过，但 RFPwrIn 比较有五组失败。**
这说明此前同相一点的吻合不能证明 RFPwrIn 与相干合路驱动功率普遍等价。
不修改 RFModel 数值模型、不放宽容差，也不把已知差异标记成兼容通过。

## 参考电路与参数

参考副本为 build-reference/RFModel_PhaseCombiner.wsv，来自官方
RF Design Kit/Phase/Spectrum Phase.wsv。该电路是两个 MultiSource，
各经过 TLE 后通过理想三端 tee 合路，再经过 5 dB 匹配衰减器到 Port_3；
不是 SPLIT/HYBRID 器件的完整参考。

两源均为 1 GHz、0 dBm CW，50 ohm，Phase 为 0°/扫描相位。源的 BW 参数回读
为 1 MHz，实际 CW 输出以中心频率 ±0.5 Hz 两个边界点表示。RefClk 同为
RFModelClock 时实测相干编号相同；两个 RefClk 为空时编号不同。
两条线均为 50 ohm、无损、1 GHz 标定，电长度回读为 pi/6 rad。
早期基准的实际长度为 30 rad，仍独立保存，没有改写成 30 度。

TLE 可调参数 L.Set 以原生弧度存储；普通源 Phase.Set 以显示角度解析。
归档器核对命令、参数回读、数据集新鲜度、版本和来源 ID，不把设置意图当成
参数已生效的证据。

## 路径复幅度和相干关系

比较器为每个来源构建两条线、三端 tee 和衰减器的完整 RFModel 网络。
由 SystemVue V3/Z3 还原 sqrt(W) 波幅，先检查其与 P3 自洽，再比较原生预测。
预测来源组由 RFModel 源/参考时钟解析器生成，不使用 SystemVue 编号作为输入。

六组、两来源、两个 CW 边界共 24 个复幅度比较全部满足原有相对 1e-7 阈值，
最大相对误差约 1.645e-8。六组时钟关系也全部符合。相位旋转是实测电压变化，
没有用功率数据反推相位；不同软件的编号数值不需要相等，只比较分组关系。

## RFPwrIn 的已观察差异

官方本机帮助 sim/Spectrasys_Total_RF_Power_Entering_a_Part.html 将 RFPwrIn
定义为所有器件端口上进入器件的总 RF 功率。原先将它直接解释为相干合路驱动
的证据不足；这次扫描显示它在下述配置间几乎不变：

| 时钟 | 源 2 相位 | RFModel 合并功率 W | SystemVue RFPwrIn(Attn1) W | 比较 |
|---|---:|---:|---:|---|
| 共时钟 | 0° | 0.001777777778 | 0.001777777726 | 通过 |
| 共时钟 | 90° | 0.000888888889 | 0.001777777726 | 不通过 |
| 共时钟 | 180° | 近零 | 0.001777777726 | 不通过 |
| 独立 | 0° | 0.000888888889 | 0.001777777726 | 不通过 |
| 独立 | 90° | 0.000888888889 | 0.001777777726 | 不通过 |
| 独立 | 180° | 0.000888888889 | 0.001777777726 | 不通过 |

沿用相对 1e-7 / 绝对 1e-15 W 下限，不作拟合。各来源传播相位与时钟 ID 均正确，
因此不能用“相位或时钟未设置成功”解释这些结果。

对独立时钟 180° 另行调用官方对象公开的 ClearModelCache 后重算，仍有同样差异。
同时回读 CoherentIM=1、ShowTotals=0、CalcNoise=0、NeedRun=0。
该控制实验不支持“旧模型缓存未更新”这个解释，但尚不能确定 RFPwrIn 内部语义
或其他分析配置的具体影响；不据此断言厂商算法有错。

官方 Composite_Spectrum_Tab.html 说明 Show Totals 显示节点各方向的总谱。
后续已成功采集方向总谱，见下一节。RFPwrIn 的五组差异仍未消除，
不能将该指标直接替代方向总功率。

## 方向总谱实测

2026-10-09 修正脚本语言后，显式设置 ShowTotals=1，重新完成共时钟/独立时钟
各 0°、90°、180° 六组采集。按 IDNo/IDName 定位输出节点的
`Node Total from 'Attn1'`，不依赖谱线排列位置或固定数值 ID。

| 时钟 | 源 2 相位 | SystemVue 输出总功率 W | 原生比较 |
|---|---:|---:|---|
| 共时钟 | 0° | 0.000562182677166 | 通过 |
| 共时钟 | 90° | 0.000281091338583 | 通过 |
| 共时钟 | 180° | 4.81e-35 至 1.71e-34 | 通过 |
| 独立 | 0°、90°、180° | 0.000281091338583 | 均通过 |

六组各两个 CW 边界，共 12 项总功率比较通过；非零点最大相对误差约 3.20e-8，
沿用相对 1e-7 / 相消绝对 1e-15 W 阈值。两端点描述同一 CW 频谱边界，
不能把它们作为两个音调相加。24 项路径复幅度和六组时钟关系也通过。
独立来源的合成总电压不作为唯一物理相位进行验收，仅检查功率及电压模长自洽。

这补齐了当前双源、线性 tee/衰减器案例的相干相消及独立功率叠加证据，
不扩展到单源功分、多音非线性或 LO 相干关系。
新报告 `direction_total_power_passed=true`，但因 RFPwrIn 五项差异，
整体 `passed=false`、命令仍返回 1。ShowTotals 官方定义为显示选项，
此次实测也未改变 RFPwrIn。

## 数据、报告与重放

- 早期同相基准：validation/systemvue-2023-coherent-network-captures.json。
- 新六组扫描：validation/systemvue-2023-coherent-phase-scan-captures.json。
- 清缓存控制：validation/systemvue-2023-coherent-cache-control-captures.json。
- 新方向总谱：validation/systemvue-2023-coherent-total-scan-captures.json。
  对应报告：validation/systemvue-2023-coherent-total-scan.json。
- 相位扫描、缓存控制及总谱报告均为 passed=false，
  path_wave_and_clock_relation_passed=true，rfpwrin_agreement_passed=false。

采集保留原始数据 SHA256、参数、频率/功率/电压/阻抗、相干编号与时间戳。
source_workspace_sha256 明确是归档时磁盘上的源工作区副本散列，不是内存中
修改参数后的整个工作区快照。数值报告另保留归档文件和 DLL 的 SHA256。

```powershell
python scripts/reference/run-systemvue-reference.py coherent build-reference/RFModel_PhaseCombiner.wsv build-reference/coherent-new-run --coherent-locked --coherent-phase-deg 90 --coherent-length-rad 0.5235987755982988
python scripts/reference/archive-coherent-captures.py build-reference/coherent-new-run --workspace build-reference/RFModel_PhaseCombiner.wsv --systemvue-version 2023.0.0.11903 --output build-reference/one-capture.json
python scripts/reference/compare-coherent-network.py build-msvc/Release/rfmodel_c.dll validation/systemvue-2023-coherent-phase-scan-captures.json build-reference/phase-scan-report.json
```

最后一条命令目前应返回 1，表示保留了真实比较失败，不能作为“已通过”执行。
归档工具只接受已正常结束的采集，且要求命令显式给出相位和线长；独立时钟省略
--coherent-locked。新方向总谱采集追加 --coherent-show-totals。
比较器仍拒绝过期、重复、错误模式及相干编号矛盾的数据。

16 项比较/归档回归覆盖基准、已观察差异、缓存控制、篡改波幅、错误分组、
不完整采集和命令/回读不一致。CI 通过表示这些验证工具和既有模型回归通过，
**不表示五项测量差异消失**。

## 后台语言问题及恢复

用户提供的实际提示为：脚本看起来像 Python，却以 VBScript 执行，是否切换语言。
因此之前的 Error Running Script 不能作为 SetProperty 不受支持的证据。
coherent-locked-phase180-totals-001 已在 2026-10-09 04:18:52 UTC 返回，
随后因数据集不新鲜而失败；保留原 timeout_unresolved 状态及日志，不纳入验收。

修复使用明确的 VBScript：Dim 声明变量，Set 获取工作区对象，Call 调用方法；
ShowTotals 使用 CByte 变量传入 SetProperty。最小探针在约 18 ms 内返回、
错误列表为空、ShowTotals 从 0 回读为 1，确认该接口可用。
正式后台采集随后连续完成六组总谱和一组恢复基准，不需要界面操作。
采集结果记录提交脚本及语言，便于核查。

`--coherent-show-totals` 显式开启总谱；省略时设为 0，避免继承前一次设置。
归档同时核对命令、分析设置回读及谱线布局。末次恢复同相、共时钟、
线长 pi/6、ShowTotals=0，专用 SystemVue 仍保持打开。

## 待完成

1. 解释 RFPwrIn 与方向总谱/原生相干驱动的语义差异；
2. 单源真实功分后经过不同支路再合路的实测；
3. SPLIT/HYBRID 全复数矩阵、有限隔离与阻抗参数；
4. 非线性产物及 Mixer 相干传播的实测，而不仅是原生/系统图解析回归。

本阶段本地 CTest Debug/Release 各 66/66 通过；比较/归档 16 项、后台运行器
12 项单元回归通过。原生库、Python 绑定与数值容差均未改动。方向总谱子项通过，但相位扫描、缓存
控制及方向总谱报告整体仍为未通过；远端 CI 只用于工程回归，不替代该兼容性结论。
