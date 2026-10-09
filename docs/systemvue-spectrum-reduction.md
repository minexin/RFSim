# SystemVue 2023 高阶来源缺项与频谱削减

## 已定位的差异

此前等功率双音中缺少的 12 个四、五阶来源标签，与原生分析设置
`UseSpecReduction=1` 有直接关系。仅关闭该设置并重新运行，12 项全部恢复；
三组功率配置的配对采集都支持这一结论。

关闭削减时，每组完整记录 16 个四阶和 28 个五阶来源项，全部通过原有
`1e-7` 复波相对误差阈值。开启削减时，当前三组实验只保留每个同频、同带宽
组中幅度最大的项，等幅最大项均保留。这是受控工况的观测规律，尚不是可以
推广到任意器件、源谱形、阶数、相干组和阈值的完整削减算法。

这使先前的“12 个标签缺失原因未知”收敛为“参考软件开启频谱削减”。
通用多项式核心保留全部解析生成项，符合关闭削减时的这组实测；本轮没有
在核心中删除较弱项，也没有将拟合五阶系数当成通用 RFAMP 公式。

## 实验与来源

SystemVue 版本为 2023.0.0.11903，仍用专用 Source → RFAmp1 → RFAmp2
副本，只比较第一放大器输出的原生 `F3/P3/ID3/V3/Z3`。
两音频率 1 / 1.1 GHz，带宽各 1 Hz，相位 0° / 90°。两个放大器均为
G=10 dB、OP1dB=20、OPSAT=23、OIP2=40、OIP3=30 dBm，50 Ω，
RISO=140 dB。最大分析阶数 5，通道宽度 1 MHz，关闭噪声和次级谱再混频。

除配对开关外，以下设置都显式写入并回读验证：

- `UseSpecReduction`：配对为 1 / 0。
- `ElimSpec=0`：关闭另一项“输出时按峰值差删除谱线”的功能。
- `IgnorePwrLvl=1e-23 W`：绝对阈值 −200 dBm。
- 第二音功率：独立于第一音控制，命令、元数据和原生 Pwr 数组必须一致。

| 第一音 / 第二音 | 开启削减的四阶 / 五阶 | 关闭削减的四阶 / 五阶 | 恢复标签数 |
| --- | --- | --- | ---: |
| −10 / −10 dBm | 14 / 18 | 16 / 28 | 12 |
| −10 / −20 dBm | 12 / 18 | 16 / 28 | 14 |
| −20 / −10 dBm | 12 / 18 | 16 / 28 | 14 |

六次采集均无警告、数据时间戳新鲜。合计 86 个四阶、138 个五阶已记录来源项
通过复幅度检查；开关两侧共有的 92 项复波完全相同。按来源项计数，
每项实际检查两个带边，不能将它们算成两项独立来源。

官方 Output Tab 帮助将冗余候选限定为同频、同带宽，并区分仿真传播阶段
削减与仅输出阶段删除。原始记录的具体保留行为仍由本轮实验确认，不能只凭
帮助文字推断。[来源摘要及哈希](../validation/systemvue-2023-spectrum-reduction-help-provenance.json)
保留文档定位依据，不分发厂商页面全文。

## 如何排除其他解释

等功率时，四阶 2f1 上的跨源项排列数为 12，单源项为 4，后者不记录；
将第二音降低 10 dB 后，单源项成为更强者，标签随之切换。交换两音功率后
相应行为也交换，因此现有证据不支持“固定忽略某个源或某种标签”的解释。

在 38 个重叠来源观测上，记录复波对应保留项本身，并不等于同阶同频组中
全部候选项的复波之和。官方 Intermod Sub-Spectrum 对图形主导标签的描述，
不能直接替代原生来源记录的数值语义。这里没有推断 NodeTotal 或路径预算
应该如何重构；这些仍需单独验证。

四阶系数沿用先前的公开截点规则，没有重新拟合；最大相对误差约 5.01e-8。
五阶沿用另一组单音第五谐波识别的系数，本轮六组双音均不参与识别；
最大相对误差约 8.50e-16。由于五阶仍是固定器件参数下的实验识别，
报告保持 `diagnostic_only`、`eligible_for_compatibility=false`。

## 重放与接口保护

```powershell
python scripts/reference/run-systemvue-reference.py cascade build-reference/RFModel_CascadeIntermods.wsv build-reference/my-reduction-off --source-power-dbm -10 --cascade-two-tone --cascade-second-power-dbm -20 --cascade-phase-deg 90 --cascade-max-order 5 --cascade-riso-db 140 --cascade-disable-spectrum-reduction

python scripts/reference/diagnose-spectrum-reduction.py build-msvc/Release/rfmodel_c.dll validation/systemvue-2023-highorder-captures.json validation/systemvue-2023-spectrum-reduction-captures.json build-reference/reduction-replay.json
```

采集命令只连接唯一的专用工作区，输出目录必须新建。第二音功率只允许双音
模式且范围为 −60 到 −10 dBm。开关省略时显式启用削减，避免上次实验状态
泄漏到下次采集。旧归档保持可读；新削减实验必须包含明确的设置及其回读。
旧三阶级联和六组高阶诊断继续拒绝不在各自验证范围内的新配置。

诊断命令退出 0 表示这六组开关对照支持当前有界假设，不代表全库兼容。
七项新回归覆盖真实配对、缺少/重复工况、设置回读、丢失恢复项、相位损坏、
命令与元数据不一致，以及旧比较器的适用范围。既有后台运行器测试也覆盖
新增命令参数的有效传递和错误拒绝。

- [六组采集](../validation/systemvue-2023-spectrum-reduction-captures.json)
- [本机 Release 诊断报告](../validation/systemvue-2023-spectrum-reduction-diagnostic.json)
- [前一阶段四、五阶诊断](systemvue-highorder-diagnostic.md)

实验后已重新采集并恢复单音 −30 dBm、0°、最大阶数 3、第二级 10 dB、
RISO=100 dB、频谱削减开启的专用基线。下一步应继续识别不同器件参数下的
高阶系数，并独立核实 NodeTotal、路径预算及多级次级失真。

本阶段工程验证：MSVC Debug / Release 完整 CTest 各 78/78，通过后新增报告
计数字段对应的诊断又在两种配置复查通过；后台运行器 17/17，新配对诊断 7/7。
本轮没有修改 C++ 核心或 ABI。
