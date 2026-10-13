# 1 dB ATTN_Linear 离线复核（2026-10-12）

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

本轮从已有归档重放 2026-09-24 的单器件失败，没有新增 SystemVue 仿真或采集。原始四项比较仍为三项通过、输出噪声密度失败，整体未通过；原 `1e-7` 相对阈值和 SI 玻尔兹曼常数均未改变。独立诊断成功不表示厂商兼容通过。

## 交付物和确切目录映射

- [目录项评估](../validation/systemvue-2023-attn-linear-assessment.json)：`rfdesign/Attenuator_Part.html` → `rfdesign/ATTN_Linear.html`，记录参数、默认值、单位、端口、API、证据与未覆盖行为。
- [可重放证据摘录](../validation/systemvue-2023-attenuator-offline-evidence.json)：原归档的五个测量向量、选定噪声分量与 V/Z、保存参数、实际回读、帮助页及输入文件 SHA-256。
- [机器诊断报告](../validation/systemvue-2023-attenuator-offline-diagnostic.json)：独立数学、原比较结果、分量检查及仍缺的数据。
- [阶段验证与耗时](../validation/attenuator-offline-stage-20261012.json)：本地测试和执行时间。
- [诊断脚本](../scripts/reference/diagnose-attenuator-1db.py)和[专项回归](../scripts/reference/test_diagnose_attenuator_1db.py)。

保存工作区 XML 的 SHA-256 与原参考 `source_sha256` 完全一致；XML 中 Attn 的 Model 为 ATTN_Linear。官方帮助页哈希与已有模型目录一致。其他 Attenuator 模型不随此评估获得支持状态，目录生成清单保持不变。

## 参数、单位和参考面的核对

| 项目 | 已确认的证据 | 边界 |
|---|---|---|
| L | 保存表达式 Atten 对应 1 dB；后续 1 dB 原扫描实际 Data 为 1.2589254117941673，即线性功率损耗比 | 传入 RFModel 前须用 10*log10(Data)，不能把 Data 直接当 dB |
| 温度 | 原分析 RoomTemp 与器件 Ta 结果均为 16.85°C，换算为 290 K | Ta 参数 Data 为空；继承规则由保存默认设置和有效温度结果共同支持 |
| 源和频率 | 保存源 Freq=100、Pwr=-160，参考记录为 100 MHz、1e-19 W；两端 CF=1e8 Hz | 原回读未覆盖所有源属性，保存配置不能替代同次运行的完整回读 |
| 阻抗 | 保存 Zref/ZIN/ZOUT 使用默认值，帮助表为 50 Ω；原频谱 Z 接近 50 Ω | 原参数 Data 为 null；不能宣称逐个运行时属性已确认，更不能外推失配行为 |
| 输出 | CGAIN/CNF 为线性比，CND 为 W/Hz，DCP 为 W | 原比较器取第二个路径节点；未混用 dB/dBm |
| RFModel 映射 | MatchedTransmissionModel、rfmodel_matched_transmission_s、Library.matched_transmission、JSON matched_transmission；噪声另用被动热噪声接口 | 只是匹配、恒定损耗原语，未映射独立 ZIN/ZOUT、频率参数表和文件 |
| 默认值 | 官方 L 默认 3 dB，RFModel JSON loss_db 默认 0 dB | 本用例显式使用 1 dB，不依赖相同默认值 |

数值格式、两个节点维度、运行时间与数据集时间戳由原验证器核验。原归档与精简参考的五个向量逐项完全相同，不通过近似匹配放行。后续扫描另有独立采集时间及哈希，不冒充原始运行。

## 独立数学检查

以 50 位 Decimal 计算匹配衰减器的功率关系，未调用 RFModel 噪声实现：

```
g = 10^(-L_dB/10)
Nsource,out = k*T*g
Ndevice,out = k*T*(1-g)
Ntotal,out = k*T
F = 1 + (1/g-1)*T/290
Psignal,out = g*Pavailable
k = 1.380649e-23
```

实际 C++ reference_attenuator 探针四项输出均与该独立预期在 1e-13 数值检查阈值内一致。该阈值仅用于内部独立数学检查；厂商比较仍固定为原来的 1e-7。

| 比较项 | 原相对误差 | 原结论 |
|---|---:|---|
| 功率增益 | 4.9999970e-9 | 通过 |
| 噪声因子 | 1.4110104e-15 | 通过 |
| 输出噪声密度 | 9.8915667e-7 | 失败 |
| 输出信号功率 | 2.9999999e-8 | 通过 |

当前 Debug/Release 的噪声因子为 1.258925411794167，较历史探针结果高一个 double ULP（2.220446049250313e-16）；其厂商相对误差为 1.2346341e-15，仍通过。机器报告保留该差别，不将重放描述为所有数值逐位相同。原始厂商值、容差及通过/失败判定不变。

没有发现本用例中把损耗比当 dB、把噪声因子当 dB、遗漏源噪声或错误选取路径输出节点导致失败的证据。原比较器无需为消除本次差异而修改。

## 本轮进一步定位的证据

原归档包含两个频谱端点 99,999,999.5 和 100,000,000.5 Hz 的源噪声、衰减器自身噪声和总噪声。每个端点上，两项出射贡献的和与归档总量在 1e-13 内一致；八个归档谱项均满足峰值电压功率关系 `P=|V|²*Re(1/Z)/2`。这验证了原始谱分量的内部一致性，不把其参考面直接等同于路径 CND。

| 诊断量 | 数值 |
|---|---:|
| 路径源噪声相对 SI kT 的有符号偏差 | +0.989015846 ppm |
| 路径输出噪声相对 SI kT 的有符号偏差 | +0.989157645 ppm |
| 原始输出端源噪声相对 SI 分量预期 | +0.989015848 ppm |
| 原始输出端器件自身噪声相对 SI 分量预期 | +1.014015877 ppm |
| 路径 CND / 原始输出总噪声 − 1 | 约 -5e-9 |
| 只替换源噪声、保持器件 SI 噪声时的剩余相对误差 | 2.035542323e-7，仍超限 |

因此，现有证据支持共同噪声基准及小幅数值处理差异；**只修正源端不足以消除失败**。不能把原始频谱 P 与路径 CND 的微小归一化差别归给衰减器物理损耗。

保存样例方程明确使用 `1.3806503e-23`，其相对 SI 常数偏差约 +0.941586167 ppm。仅作为诊断假设，使用该值计算 kT 后，原输出 CND 的剩余相对差为 4.7571431e-8。这个事实没有证明仿真器内部使用该常数，也没有解释其余差别；没有用它替换核心常数、重算原验收或拟合任何参数。

## 重跑

以下只运行 RFModel 探针，完全不连接 SystemVue。新诊断返回 0 表示证据和诊断检查有效，报告中的 vendor_subset_passed 仍为 false。

```powershell
python scripts/reference/diagnose-attenuator-1db.py replay validation/systemvue-2023-attenuator-1db.json validation/systemvue-2023-attenuator-1db-comparison.json validation/systemvue-2023-attenuator-offline-evidence.json build-msvc/Release/reference_attenuator.exe build-reference/attenuator-offline-replay.json
python scripts/reference/test_diagnose_attenuator_1db.py build-msvc/Release/reference_attenuator.exe
python scripts/reference/test_compare_attenuator.py
ctest --test-dir build-msvc -C Release -R "^offline_attenuator_diagnostic_test$" --output-on-failure
```

如需从本机保留的原归档重新生成摘录：

```powershell
python scripts/reference/diagnose-attenuator-1db.py extract build-reference/attenuator-inspection.json build-reference/attenuator-scan-1.json build-reference/RFModel_AttenuatorNoise.wsv "C:/Program Files/Keysight/SystemVue2023/Help/systemvue.qch" build-reference/attenuator-offline-evidence-rebuilt.json
```

该命令只读 XML、SQLite 帮助及 JSON；摘录仅保留事实字段，不提交完整厂商工作区或帮助正文。原始归档不在源码包中，重放命令则只需要已提交摘录和 RFModel 探针。诊断拒绝覆盖输入及固定历史参考文件。原始字节 SHA-256 用于本次归档溯源；提交后的参考 JSON 另以排序后的规范化内容摘要校验，以允许 Git CRLF/LF 换行差异，任何数值变化仍会拒绝。

## 验证和停止边界

新增 10 项离线诊断测试通过，覆盖原失败保持、独立热噪声预算、单位/时间戳/模型身份、谱分量损坏、原文件保护和 CLI 失败保留结果。Debug/Release 的 CTest 专项各 1/1 通过，内部各执行这 10 项；原比较器 9 项回归通过。两种配置均重跑原比较器，确认仍返回 1、且保留原三通过一失败。没有修改原生仿真实现，本阶段未重跑全套 CTest、安装消费者或 wheel。

要确定厂商内部根因，最小缺项是：

1. 同次运行的已解析 Model、Zref/ZIN/ZOUT、源/负载阻抗、有效温度和分析噪声设置；当前保存默认值及空 Data 不能替代它们。
2. 内部热噪声常数/归一化定义的权威证据，或独立控制源噪声与器件噪声的参考，同时记录路径 CND 和原始 V/Z/P。

失配、频率数组、Ta 覆盖和全器件兼容不属于本次已验证子集，继续开放。本轮在这些数据依赖处停止，不为得到通过而继续拟合或放宽阈值。
