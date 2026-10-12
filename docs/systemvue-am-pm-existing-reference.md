# A01 已有捕获的补充离线诊断

2026-10-12：未新增 SystemVue RunAnalysis，使用上一轮 A01 的既有捕获，对真实 RFModel C API 和 Quick Sweep 相位来源做补充核验。**Point Derivative 当前未通过厂商数据对比，DataPoints 样条候选也未通过完整观测集合。** 原始两基线复现只说明 SystemVue 重现历史结果，不代表 RFModel 与其相等。

## 真实 C API 对比

通过 Python `Library.am_pm_response` 调用重新确认构建的 Debug/Release 原生库；以厂商 DCP 输出功率（W）作为自变量，将 DCPH 输出相位减去输入相位后与原生返回值比较。参数为 −5 degrees/dB、10 dBm，原始 SI 读回 `AMtoPM=-5, PwrAMtoPM=0.01`。固定容差沿用 `1e-5 deg`，不按数据拟合或放宽。

| 检查 | 两种配置的结果 |
|---|---|
| 原生 Point Derivative | 7/37 匹配，30/37 不匹配 |
| 最大圆周相位残差 | 0.00616264490543°，输出约10 dBm |
| 此处 SystemVue / RFModel 相位 | −36.1973689511° / −36.1912063062° |
| 完整模型兼容 | 未通过；本次也没有检验完整模型 |

仅为定位差异，令 `u=sqrt(Pout/0.01)`，在看到数据后选择索引7、17拟合 `a*u²+b*u³`。得到 `a≈−65.1626193791, b≈28.9652499090`，其余35点最大残差约 `3.90e-9 deg`。该拟合对应的导数峰值约 −5.00000500057 degrees/dB、峰值功率偏移约 −0.00122528871 dB。它提示幅度三次形式与这组数据接近，但参数映射仍有差异；不能把这两个拟合系数当成通用厂商常数。

两个索引是事后选择，其余点也已被观察，不称为独立保留集。本扫描最高输出约13 dBm，未覆盖解析公式约13.522 dBm开始的高功率恒相位支路；正峰值、其他参考功率、多音和不同版本行为仍未证实。拟合不改变原生对比的失败结论，本轮未修改核心模型。

## Quick Sweep 的独立相位来源

本机官方帮助 `sim/Quick_Sweeps.html` 说明：Quick Sweep 会断开路径输入端的原理图源，使用内部 CW 激励；快速扫描保留路径数据，主数据集中的原始频谱和未扫描路径仍属于普通仿真。`sim/Compression_Curve.html` 另说明其快速扫描按需要启用噪声、互调和谐波。帮助页提取自本机 SystemVue2023 的 `Help/systemvue.qch`，页面 SHA-256 随归一化证据保存。

从四种表格配置各选择一次既有捕获，读取 Path2 的37点 DCP/DCPH，共148点；不把同一配置重复运行算作新的独立扫描。源相位从每点输入节点 DCPH 读取，均为0°。普通源设为30°的常值表配置，扫描输出约20°，普通仿真输出约50°，二者有不同的激励来源。

以 DCP 功率的 dBm 值、原始表角度、自然三次样条和常值端外延伸预测：**144/148 匹配，四个0 dBm点全部失败**。四种配置为 `unwrapped-equivalent`、`full-turn`、`source-phase-offset`、`natural-cubic-followup`。这组扫描从约0 dBm到103 dBm，多数点远在表尾之外，不能把148点称为密集的内部插值验证。未使用普通仿真的复电压来替代扫描每一步的原始电压，也没有该类逐步电压证据。

## 可复跑证据与验证

- [归一化输入、运行时间、数据集路径和来源哈希](../validation/am-pm-existing-reference-evidence-20261012.json)
- [Debug 全部比较点](../validation/am-pm-existing-reference-comparison-Debug-20261012.json) / [Release 全部比较点](../validation/am-pm-existing-reference-comparison-Release-20261012.json)
- [阶段验证记录](../validation/am-pm-existing-reference-validation-20261012.json)
- [比较器](../scripts/reference/diagnose-am-pm-reference.py) / [回归测试](../scripts/reference/test_diagnose_am_pm_reference.py)

```powershell
# 原生库须先构建；输出文件须不存在。数据不匹配时退出码1，JSON仍保存。
python scripts/reference/diagnose-am-pm-reference.py validation/am-pm-existing-reference-evidence-20261012.json build-msvc/Release/rfmodel_c.dll build-reference/am-pm-existing-replay.json
# 若原始 build-reference 捕获仍在，追加 --raw-root . 核对哈希、原始数组与参数。
python scripts/reference/test_diagnose_am_pm_reference.py build-msvc/Release/rfmodel_c.dll
```

默认只读取随提交保存的归一化数据；可选原始重放校验捕获哈希、时间、分析路径、74元素数组布局、100 MHz、节点顺序、逐点数值和参数。原始全文与帮助页只在本地，哈希不等于提交了这些原始文件。两种配置已执行原始重放，结果 `raw_sources_verified=true`。

MSVC 原生库 Debug/Release 目标构建通过；相关 CTest 各6/6：C API、Python API、AM/PM 数学、AM/PM 工作点、新旧离线诊断。新增诊断包含5项测试，覆盖30个原生反例、四个零dBm点、两种源相位、无效/不完整证据和CLI失败保留结果。首次测试中的30°严格浮点相等断言已改为数值容差；它不改变厂商比较容差。首次无绝对路径的构建命令因工具不在 PATH 中未运行，随后使用缓存内 Visual Studio CMake 路径完成验证。

这些是本阶段的相关回归，不是新一轮全量CTest、clean-first构建、安装验证或远程CI。本地提交、不推送；先前8个未提交文件均核对哈希后保留。
