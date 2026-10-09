# SystemVue 2023 RFAMP 四阶与五阶诊断

## 结论与验收边界

六组新的 SystemVue 2023.0.0.11903 采集支持第一放大器四阶生成项的公开截点
规则：23 个已记录来源项全部在原有 `1e-7` 复幅度相对误差内吻合。
五阶系数只用一个单音工况的第五谐波识别；另外 27 个来源项作为保留验证，
在同一阈值内吻合。它们不是 27 个独立采集工况。

双音结果仍缺少 12 个多项式预测的来源标签（四阶 2 个、五阶 10 个）。
报告保留 `all_predicted_origins_recorded=false`，固定为 `diagnostic_only`、
`eligible_for_compatibility=false`。未记录的项不当作零，也不计入数值通过项。
本阶段没有把拟合系数加入生产 RFAMP 接口，完整高阶子谱兼容仍未验收。

## 参考来源与实验设置

依据本机安装的官方帮助 `systemvue.qch`：RFAMP 是 RFAMP_HO 的包装模型；
公开帮助给出四阶截点经验关系 `IP4=P1dB+13 dB`，并明确五、七、九、十一阶
系数由 OP1dB、OPSAT、OIP3 经专有算法求出，没有给出可直接实现的精确公式。
[帮助来源记录](../validation/systemvue-2023-highorder-help-provenance.json)
保存 QCH 和页面 SHA256、页面路径及摘要，不分发厂商帮助全文。
将四阶关系用于输出参考截点是本诊断的解释，需由实测检验。

沿用专用副本 `Source → RFAmp1 → RFAmp2 → Port_2`，实际比较节点是
第一放大器输出对应的原生 `F3/P3/ID3/V3/Z3`。第二级输出没有纳入本次验收。
两级增益均为 10 dB，OP1dB=20、OPSAT=23、OIP2=40、OIP3=30 dBm，
参考阻抗和输入输出阻抗均为 50 Ω，RISO=140 dB。最大分析阶数 5，
通道宽度 1 MHz，关闭噪声与次级谱再混频，UseWithin=−50 dB。
源频率为 1 GHz，双音另有 1.1 GHz；每个源的带宽为 1 Hz。

| 输入配置 | 四阶已记录项 | 五阶已记录项 | 未记录预测项 |
| --- | ---: | ---: | ---: |
| 单音 −30 dBm，0° | 1 | 0 | 4 |
| 单音 −20 dBm，0° | 2 | 3 | 0 |
| 单音 −10 dBm，0°（五阶识别） | 2 | 3 | 0 |
| 单音 −10 dBm，45° | 2 | 3 | 0 |
| 单音 −10 dBm，90° | 2 | 3 | 0 |
| 双音各 −10 dBm，0° / 90° | 14 | 18 | 12 |

每个已记录来源项检查两侧带边的复波、频率和带宽；表中按来源项计数，
不把两个带边当作两个独立来源。−30 dBm 的五阶数值结论为 `null`，不报告通过。
所有采集均经过拓扑、参数回读、时间戳、版本、警告和来源字段检查。
采集状态中的 `eligible_for_compatibility=true` 只表示数据可用于比较，
不表示最终数值兼容，最终解释应以诊断报告为准。

## 系数与误差

功率波使用 `|w|²=P`；50 Ω 下电压换算因子为 `c=sqrt(R/2)=5`。
对于四阶双音 `3f1−f2`，排列数为 4。根据输出参考截点 33 dBm、
输入参考截点 23 dBm 及小信号幅度增益 `sqrt(10)`，得到：

`a4 = sqrt(10) / (4 × P_IIP4^(3/2) × c³) = 0.07096267784671509`

四阶生成系数由该关系事先确定，不从本次观测拟合。23 项的最大复波相对误差
约 `5.00000019e-8`，保持既有 `1e-7` 阈值，没有为本实验放宽容差。

只用 −10 dBm、0° 单音的第五谐波，按 `a5 = w_H5 / (c⁴ × w_in⁵)`
识别出实系数 `−0.07786106164494842`。识别工况的三个五阶来源项不计入
独立保留验证；其余工况的 27 个五阶来源项最大相对误差约 `8.42e-16`。
该数值只属于当前固定放大器参数，不能推广为任意 OP1dB/OPSAT/OIP3 的公式。

## 未完成项与下一步

双音的 12 个未记录标签及对应 RF bin 全量保存在报告中，包括四阶的
`[-(Source.Source1)+3x(Source.Source1)],RFAmp1`。同样阶次在单音 −10 dBm
时有记录，因此还不能把双音缺项简单解释为低功率门限。
当前诊断也没有把缺项合并进其他已记录标签或 NodeTotal。

下一步需针对这些 RF bin 检查 SystemVue 的子谱存储、显示、相干组和总谱
之间的关系，再确定来源合并规则；之后扩展压缩区、不同放大器参数和多级高阶
对照。当前不验收第二级高阶生成、次级再混频、总谱、噪声或完整压缩响应。

## 重放与回归

```powershell
python scripts/reference/diagnose-highorder-amplifier.py build-msvc/Release/rfmodel_c.dll validation/systemvue-2023-highorder-captures.json build-reference/highorder-replay.json
```

退出码 0 仅表示已记录的四阶项及五阶保留验证项满足数值假设，不能作为完整
兼容性通过信号；调用方必须同时读取报告分类和来源覆盖字段。
七个回归测试覆盖真实六组数据、缺失项、缺失/重复实验、配置不受控、
四阶相位损坏、五阶识别误差、非实五阶系数和带宽损坏。

- [六组受控采集](../validation/systemvue-2023-highorder-captures.json)
- [本机 Release 诊断结果](../validation/systemvue-2023-highorder-diagnostic.json)
- [九阶通用多项式接口](coherent-polynomial.md)

实验后已成功重新采集并恢复专用 SystemVue 副本：单音 −30 dBm、0°、
最大阶数 3、第二级 10 dB、RISO=100 dB。采集器明确使用 VBScript，
提交文本以 Dim/Set 开头，并显式传入 VBScript 语言枚举。

本阶段重新配置后，MSVC Debug / Release 完整 CTest 均为 77/77；新增诊断
包含 7 个回归测试。归档报告的采集文件与 Release 动态库 SHA256 已复核，
两个新增 Python 文件通过 Black 格式检查。核心 C++ 与 ABI 本轮没有改动。
