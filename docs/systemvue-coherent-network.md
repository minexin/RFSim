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
仍需成功采集方向总谱或另一独立总功率测量，才能对反相抵消做厂商测量验收。
当前相位扫描**不代表合并功率通过**。

## 数据、报告与重放

- 早期同相基准：validation/systemvue-2023-coherent-network-captures.json。
- 新六组扫描：validation/systemvue-2023-coherent-phase-scan-captures.json。
- 清缓存控制：validation/systemvue-2023-coherent-cache-control-captures.json。
- 对应后两份报告为同名去掉 -captures 的 JSON；其 passed=false，
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
--coherent-locked。比较器仍拒绝过期、重复、错误模式及相干编号矛盾的数据。

12 项比较/归档回归覆盖基准、已观察差异、缓存控制、篡改波幅、错误分组、
不完整采集和命令/回读不一致。CI 通过表示这些验证工具和既有模型回归通过，
**不表示五项测量差异消失**。

## 后台恢复与当前阻塞

旧 coherent-locked-phase0-003 在 2026-10-09 03:55:56 UTC 返回，随后因没有
新鲜数据集而失败；原采集进程已退出。只读 COM 探针确认唯一专用参考工作区
可用后才启动新运行。修正后的 Set 字符串采集成功完成六组扫描及缓存控制。

随后仅为启用总谱加入 SetProperty("ShowTotals", CByte(1))，使用 VBScript Call
语法，仍触发 Error Running Script。该次目录
coherent-locked-phase180-totals-001 为 timeout_unresolved；采集进程仍等待对话框，
不属于任何验收数据。具体错误文字仍待读取，不能猜测其原因。
该调用已从脚本撤回；已实际成功的 ClearModelCache 与分析设置回读保留。

电脑操作运行时再次因 Windows sandbox setup refresh 错误无法初始化，已请用户
提供错误文字并关闭提示。没有强制终止 SystemVue，也没有重复提交仍在运行的采集。

## 待完成

1. 获取可独立验证相干相消的方向总谱/总功率，并解释 RFPwrIn 差异；
2. 单源真实功分后经过不同支路再合路的实测；
3. SPLIT/HYBRID 全复数矩阵、有限隔离与阻抗参数；
4. 非线性产物及 Mixer 相干传播的实测，而不仅是原生/系统图解析回归。

本阶段本地 CTest Debug/Release 各 66/66 通过；比较/归档 12 项、后台运行器
12 项单元回归通过。原生库、Python 绑定与数值容差均未改动。新扫描和缓存
控制报告仍为未通过；远端 CI 只用于工程回归，不替代该兼容性结论。
