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

## RFModel 逐器件预算

`validation/systemvue-2023-antenna-noise-budget.json` 在统一源密度的同一次历史数据
重放中增加 RFModel 贡献，比较判定及所有检查数值保持不变（14/20）。
每个输出节点都包含各上游器件对该节点负载噪声的贡献；这不是沿途局部噪声功率的累加。
最终 RFAmp2 输出的预算为：

| 来源 | W/Hz | 总噪声占比 |
|---|---:|---:|
| Source | 6.714546423e-17 | 15.29497% |
| Attn1 | 1.043439532e-16 | 23.76836% |
| RFAmp1 | 2.554072133e-16 | 58.17885% |
| Attn2 | 2.442389619e-18 | 0.55635% |
| RFAmp2 | 9.664543000e-18 | 2.20147% |

贡献检查要求名称和顺序正确、功率有限非负，且每个节点的分项之和在 1e-12 相对误差内
等于总噪声。该检查不替代 SystemVue 差异判定。尚未采集 SystemVue 的逐器件归因，
所以这些占比只能作为 RFModel 侧的敏感度线索，不能定位 SystemVue 内部误差来源。

本轮比较校验测试 6/6 通过，Debug/Release 探针输出一致，66 个 C++ 文件格式检查通过。
重放命令与上一节相同，只需将输出文件改为上述预算报告名。

## 2026-09-29 后台复测与跨平台验证

本机 SystemVue 2023.0.0.11903 在无已有实例时以隐藏窗口方式启动，随后由
run-systemvue-reference.py 连接 COM，只打开 build-reference 内的天线工程副本。
分析时间为 UTC 08:12:36.5209524–08:12:37.4433842，Manager 错误列表为空，
数据集时间戳及 19 项器件/源参数校验通过。原始输出保存在
build-reference/antenna-run-003，未上传厂商工作区。

归档为 validation/systemvue-2023-antenna-run-003.json；该文件保留采集 SHA256。
统一输入噪声后的比较为 validation/systemvue-2023-antenna-run-003-comparison.json。
全部 20 个 SystemVue 测量数值与上一份统一源密度报告完全相同，仍为 14/20 通过，
相对容差保持 1e-7。此次重跑证明偏差可重复，尚不能据此确定偏差原因。

| 未通过项 | 相对误差 |
|---|---:|
| Attn1 CND | 6.0719622e-7 |
| RFAmp1 CND | 8.4815160e-7 |
| Attn2 CND | 8.4412359e-7 |
| RFAmp2 CND | 6.6993137e-7 |
| RFAmp2 CGAIN | 1.8847316e-7 |
| RFAmp2 DCP | 2.1347316e-7 |

```powershell
python scripts/reference/compare-antenna.py compare `
  validation/systemvue-2023-antenna-run-003.json `
  build-reference/antenna-run-003-replay.json `
  --executable build-msvc/Release/reference_antenna.exe --align-source-noise
```

预期退出码为 1，表示报告中仍有差异，不能当作采集失败，也不能标记兼容验收通过。
下一步需要受控参数变化来区分器件噪声基准和 RFAMP 近似的影响，不能对上述误差
添加经验修正。此次案例也不覆盖新增 Touchstone 噪声导入或完整 RF Design 库。

实现提交 b47b6daad4f0b619cb38c0ae5a5119eaee72271d 的
[GitHub Actions](https://github.com/minexin/RFSim/actions/runs/36541004349)
已完成 Windows、Ubuntu、macOS × Debug/Release 六个作业；核心/CLI 测试、Python
wheel 构建和独立安装消费者步骤均成功。逐作业、逐步骤记录在
validation/ci-b47b6da.json。CI 不安装 SystemVue，不能替代本机厂商比对。

## 载波功率敏感度：−60 dBm

2026-09-29 受控实验仅将源 Pwr 的第一个载波项从 −50 改为 −60 dBm，
第二个噪声源条目保持 −50，NoisePower、频带及所有器件参数保持原值并通过回读校验。
归档为 validation/systemvue-2023-antenna-minus60.json 及同名前缀 comparison.json。
比较使用 RFModel 线性探针，将信号瓦数按输入功率比缩放，不改变增益、噪声或容差。

结果为 16/20：末级 CGAIN 相对误差从 1.8847316e-7 降到 3.6847316e-8，
DCP 从 2.1347316e-7 降到 6.1847314e-8，均进入原有 1e-7 容差；
四个器件节点 CND 仍未通过，末级 CND 误差为 8.2154658e-7。
这提供了信号偏差随功率变化的证据，但不足以唯一归因于 RFAMP 压缩或任何具体算法。
需要更多功率点及对应器件参数验证，不能通过该实验改变模型系数或放宽噪声容差。

```powershell
python scripts/reference/run-systemvue-reference.py antenna `
  build-reference/RFModel_AntennaNoise.wsv build-reference/new-power-run `
  --source-power-dbm -60 --timeout 120
python scripts/reference/compare-antenna.py collect `
  build-reference/new-power-run/capture.json build-reference/new-power-run/validated.json `
  --source-power-dbm -60
```

归档时的功率声明必须与实际参数相符，否则拒绝归档；比较从已校验的归档读取功率。
旧归档缺省仍为 −50 dBm。运行结束后已恢复 −50 dBm 并重新采集，
build-reference/antenna-power-restored-001 的 20 个测量值与 run-003 完全相同。
本次首次连接在 COM attach 阶段失败退出，随后连接同一仍响应实例成功；未重启实例。
采集目录分别保留失败与成功日志，没有把失败结果用于比较。

比较工具 7 项及后台驱动 3 项回归通过。本次仅扩展参考脚本和证据，未修改数值核心。

## 四点载波扫描

继续采集 −70 和 −40 dBm，所有既定器件参数和噪声源回读校验通过。
归档分别为 validation/systemvue-2023-antenna-minus70.json、
validation/systemvue-2023-antenna-minus40.json；对应 comparison 文件分别为 16/20、
14/20 通过。恢复 −50 dBm 后，20 个测量值再次与 run-003 完全一致，
恢复记录保存在 build-reference/antenna-power-restored-002。

新增 summarize-antenna-power.py 对四份已验证采集按功率排序，以最低输入功率的
SystemVue 结果为参考，报告逐节点增益和噪声密度变化，并保留数据哈希及运行时间。
它验证只有载波源功率变化，拒绝重复功率、重复采集、未验证参数和噪声源变化。
结果为 validation/systemvue-2023-antenna-power-sweep.json，不修改兼容容差或核心模型。

```powershell
python scripts/reference/summarize-antenna-power.py `
  validation/systemvue-2023-antenna-minus70.json `
  validation/systemvue-2023-antenna-minus60.json `
  validation/systemvue-2023-antenna-run-003.json `
  validation/systemvue-2023-antenna-minus40.json `
  --output build-reference/antenna-power-summary.json
```

末级增益与噪声密度均随输入功率增大而小幅下降；这是同一设备条件下的实测趋势，
不证明完整 RFAMP 非线性模型，也不构成对残余噪声偏差的唯一解释。
新增两项汇总回归已通过并接入六配置 CI；尚未核验这次提交的 CI 结果。

## RFAMP 非线性参数回读核验

对上述四个功率点的原始 capture.json 追加只读核验，不需要重启或重新运行分析。
audit-antenna-nonlinearity.py 先验证原有器件、源和数据新鲜度条件，再核验两级
RFAMP 各 8 项参数；结果及原始采集 SHA256 保存于
validation/systemvue-2023-antenna-nonlinear-settings.json。

四点中两级放大器参数均相同：

| 参数 | DataEntry 表达式 | GetValue 实际回读 |
|---|---:|---:|
| OIP2 | 80 | 100000 W |
| OIP3 | 70 | 10000 W |
| OP1dB | 60 | 1000 W |
| OPSAT | 63 | 1995.2623149688789 W |
| AMtoPM | −5 | −5 |
| AMtoPM_Mode | 0 | 0 |
| PwrAMtoPM | 10 | 0.01 W |
| EnablePN | 0 | 0 |

功率参数的表达式与瓦数回读对应 dBm 换算；模式枚举的完整语义仍需官方帮助核对。
该证据排除了这些设置在四点扫描间发生变化，并确认原有小信号 RFModel 探针尚未
使用的非线性参数确实存在。不能仅凭其数值推定内部多项式阶数、压缩函数或
AM-to-PM 曲线，也不能把已有 matched cubic amplifier 当作完整 RFAMP 替代。

复核入口示例（其他功率点用重复 --capture 参数加入）：

```powershell
python scripts/reference/audit-antenna-nonlinearity.py `
  --capture -50 build-reference/antenna-run-003/capture.json `
  --output build-reference/nonlinear-settings-replay.json
```

工具对缺失、重复、读取失败、非有限及不符合基准的参数报错。两项回归通过并接入 CI，
此轮未修改核心模型或兼容阈值。后续需要核对官方 RFAMP 算法定义，并设计隔离器件案例。
