# 四级天线噪声链路首次后台对照

参考为 SystemVue 2023 官方 `Noise/Antenna Noise Temperature.wsv` 的本地副本。
2026-09-28 UTC 01:35:53.0314133–01:35:53.5159776，通过 Python 后台驱动调用
COM 完成 System1 分析，错误列表为空，目标数据集时间戳通过运行区间校验。
本次采集没有界面输入，数据不是此前无受控时间记录的读取结果。

链路顺序为 Source → Attn1 → RFAmp1 → Attn2 → RFAmp2。
输入是 5 GHz、−50 dBm 信号和覆盖载频的 50 K 噪声源，环境温度 290 K。
衰减器线性损耗分别为 1+77.7/290、1+453.6/290；放大器增益为 25、30 dB，
噪声因子分别为 1+150/290、1+700/290，反向隔离均为 50 dB。
这些器件条件来自官方工作区的参数和方程；载频及环境温度已实时回读验证，
但器件参数 Data 回读未完全解析，因此条件一致性的证据仍不完整。

RFModel 使用匹配小信号链路，将放大器的前向附加噪声设为 k·290·G·(F−1)，
仅表示这个匹配案例，不声称完整实现 RFAMP 的双端口噪声协方差、压缩或 AM/PM。
程序逐级提取信号和噪声结果，同时以独立标量 Friis 公式自检。

相对容差固定为 1e-7。五节点的 CGAIN、CNF、CND、DCP 共 20 项，13 项通过、7 项失败：

- CNF 五项全部通过。
- CND 五项均未通过，误差约 0.81–1.00 ppm。
- 末级 RFAmp2 的 CGAIN、DCP 未通过，分别约 0.19、0.21 ppm。
- 其余信号功率和增益通过。

末级差异可能涉及模型近似或数值处理，尚未定因。后续先完成器件参数的实际值回读，
再改变源功率检验末级偏差；不放宽容差或增加经验修正。

## 重放与证据

数据为 `validation/systemvue-2023-antenna.json`，比较报告为同目录
`systemvue-2023-antenna-comparison.json`。快照保留原始采集 SHA256 和运行时间。
官方工作区不上传，完整 COM 捕获留在忽略目录 `build-reference/antenna-run-001`。

```powershell
python scripts/reference/compare-antenna.py collect `
  build-reference/antenna-run-002/capture.json validation/systemvue-2023-antenna-parameters.json
python scripts/reference/compare-antenna.py compare `
  validation/systemvue-2023-antenna.json validation/systemvue-2023-antenna-comparison.json `
  --executable build-msvc/Release/reference_antenna.exe
```

比较程序当前返回 1，表示差异已记录。仅需重放时执行第二条，不必启动 SystemVue。
此项结果不等同于完整 RF System Analysis 或 RF Design 库兼容验收。

本轮 MSVC Debug/Release 完整 CTest 各 36/36 通过，两个配置的探针 JSON 完全一致。
新采集比较校验测试 3/3 通过，覆盖旧数据、歧义路径、错误载频、真实数值偏差与 NaN。
66 个 C++ 文件格式检查通过。

## 参数回读复测

2026-09-28 UTC 13:45:17.3676812–13:45:18.1892687 完成第二次后台采集。
此前空值的原因是部分 PartParam 没有 Data 属性。其 GetValue() 方法可读取求值结果；
采集器现在保留 DataEntry、data_source、evaluation_error，区分表达式与实际值。
两级损耗、放大器 G/NF/RISO/ZIN/ZOUT，以及源的功率、类型、启用状态、频率和噪声频段
共 19 项参数均已实际读取，并通过严格数值与形状校验。隔离度回读值 100000 是线性比值。

新证据为 `systemvue-2023-antenna-parameters.json` 与
`systemvue-2023-antenna-parameters-comparison.json`，旧首次快照仍保留。
参数一致后重跑仍为 13/20 通过；匹配小信号近似和完整 RFAMP 的差异尚未消除。
衰减器阻抗默认值仍返回空表达式，不能说所有默认参数都已数值确认。

官方工作区的 TempKtoNoisePwr_Watts 方程明确使用 1.3806503e-23，
50 K 源 NoisePower 实际回读为 6.903251499999993e-22 W/Hz；
RFModel 探针使用 SI 常数得到 6.903245e-22 W/Hz。
因此两边输入噪声基准确实存在可量化差别。此证据只说明示例方程的常数，
不能推断 SystemVue 内部器件噪声实现；下一步应以相同源噪声密度重比，
将输入条件差异与器件噪声差异分离，而不修改 RFModel 的 SI 常数。

新采集默认必须包含上述参数才能归档，旧快照仅允许历史比较重放。
比较校验测试增加参数失配/缺失检查，本轮 4/4 通过。

## 统一输入噪声密度

使用同一次受控采集的 Source/NoisePower 第二分量作为 RFModel 输入，
而不是用已经受端口处理影响的源节点 CND 反向拟合。新探针接受可选的
`reference_antenna [source_noise_w_per_hz]`，将密度换算为等效源温度传给链路 API。
器件物理温度、噪声因子参考温度和核心 SI 常数保持不变；默认调用保持原结果。

```powershell
python scripts/reference/compare-antenna.py compare `
  validation/systemvue-2023-antenna-parameters.json `
  validation/systemvue-2023-antenna-source-aligned.json `
  --executable build-msvc/Release/reference_antenna.exe --align-source-noise
```

报告明确记录使用的输入密度、模式和参考快照 SHA256，不替换原始比较。
该操作只重放此前实测，没有再次运行 SystemVue。容差仍为 1e-7，结果为 14/20 通过。

| 节点 | 输出噪声密度误差 ppm | 判定 |
|---|---:|---|
| Source | 0.025000 | 通过 |
| Attn1 | 0.607196 | 未通过 |
| RFAmp1 | 0.848152 | 未通过 |
| Attn2 | 0.844124 | 未通过 |
| RFAmp2 | 0.669931 | 未通过 |

末级 CGAIN/DCP 的判定和数值未变。源密度差异可以解释源节点的大部分误差，
但不能解释剩余器件噪声及末级信号差异；后续需继续检查器件噪声基准与 RFAMP 小信号近似。
不以输入对齐替代原始比较，也不将本结果视为完整兼容。

比较工具测试 5/5 通过，其中新增测试确认输入来自参数而非测量值。
MSVC Debug/Release 探针在统一输入下输出相同，非法密度参数被拒绝；
默认模式重放的全部原始检查数值未变。此轮未修改核心头文件。
