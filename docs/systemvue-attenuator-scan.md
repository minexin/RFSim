# SystemVue 衰减量扫描：0 / 1 / 3 / 10 dB

2026-09-24 在同一独立 `RFModel_AttenuatorNoise` 参考实例完成四个衰减量的实际 System1 仿真。频率保持 100 MHz、温度 290 K，50 Ω 匹配源/负载及 1e-19 W 可用信号功率沿用初始样例条件。最后一次设回并运行 1 dB；未保存修改后的工程文件。

## 采集与校验

`inspect-reference-workspace.ps1` 增加 LossDb 和 CaptureRun 参数。仅在明确运行分析时允许设置有限的 0–100 dB 衰减量；通过厂商参数 Set 接口修改 `Designs.Sch1.PartList.Attn.ParamSet.L`。CaptureRun 把运行起止时间、管理器错误和对象数据一起输出，避免手动拼接时间记录。

官方 COM 的 L.Data 返回**线性损耗因子**，例如 3 dB 返回 1.9952623149688795。归档脚本校验该值等于 10^(LossDb/10)，并核对分析温度、载频、结果数组尺寸和本次数据集时间戳，不能把“传入参数”当成“参数已生效”。对象树中存在无关的同名 DigNumAvg 节点，因此只对实际使用的测量路径要求唯一，遇到目标路径歧义会失败。

紧凑原始快照为 `validation/systemvue-2023-attenuator-loss-scan.json`；每点保留运行元数据、观测参数、两节点测量和完整采集文件 SHA-256。差异报告为 `validation/systemvue-2023-attenuator-loss-scan-comparison.json`。

## 结果与尚未解释的差异

阈值保持相对误差 1e-7，即 0.1 ppm。16 项检查中 11 项通过、5 项未通过。

| 衰减 dB | 增益误差 ppm | 噪声因子 | 输出噪声密度误差 ppm | 输出信号功率误差 ppm |
|---:|---:|---|---:|---:|
| 0 | <1e-6，通过 | 通过 | 0.983640，未通过 | 1.030311，未通过 |
| 1 | 0.005，通过 | 通过 | 0.989157，未通过 | 0.030，通过 |
| 3 | 0.005，通过 | 通过 | 0.996485，未通过 | 0.030，通过 |
| 10 | 0.005，通过 | 通过 | 1.006515，未通过 | 0.030，通过 |

噪声密度偏差接近固定量级，但并非严格恒定。零损耗点还出现独立的信号功率偏差，而其增益比通过；这要求继续核对源节点归一化、零损耗模型的数值处理及噪声分量，不能仅归因于 Boltzmann 常数。当前没有修改 RFModel 核心常数、增加经验修正或放宽阈值，也不声明 ATTN_Linear 完整兼容。

## 源节点与传输误差分解

后续用同一批已采集向量做独立诊断，报告为 `validation/systemvue-2023-attenuator-residuals.json`。本步骤没有重新运行 SystemVue，也没有改变原始绝对比对的失败状态。

| 衰减 dB | 源信号相对设定值 ppm | DCP 输出/输入相对理想传输 ppm | CND 输出/输入相对 1 ppm | CGAIN 相对 DCP 比值 ppm |
|---:|---:|---:|---:|---:|
| 0 | -0.024933 | -1.005377 | -0.005441 | +1.005378 |
| 1 | -0.025000 | -0.005000 | +0.000142 | 0 |
| 3 | -0.025000 | -0.005000 | +0.007470 | 0 |
| 10 | -0.025000 | -0.005000 | +0.017500 | 0 |

源节点噪声本身已比 SI kT 高约 0.989 ppm；各衰减点输出噪声与源节点噪声之比的偏差均小于 0.018 ppm。这支持优先检查噪声基准和源模型，但不足以证明 SystemVue 内部使用的常数，尤其不能根据单温度数据把噪声除以温度的结果当成常数定义。

零损耗信号差异仍未消失：该点 DCP 输出/输入为约 0.9999989946，而报告 CGAIN 为 1；其余三点的 CGAIN 与 DCP 比值一致。下一步应单独复测零点及邻近极小正衰减值，并检查对应的源、端口功率定义和零损耗数值处理；目前不能把这项差异归为单纯源功率偏移。

诊断脚本使用两端测量做比值，属于附加证据，**不会替代可用源功率基准，也不会把原有未通过项改为通过**。新回归检查公共源比例偏移可以在比值中抵消、真实传输偏移不会被抵消、零分母明确拒绝。采集/比较/诊断工具测试累计 7/7 通过。

```powershell
python scripts/reference/analyze-attenuator-residuals.py `
  validation/systemvue-2023-attenuator-loss-scan.json `
  validation/systemvue-2023-attenuator-residuals.json
```

## 重新采集和比对

独立参考工作区已打开时，执行以下命令采集；参数设置仅作用于该副本。已有其他工作区时脚本会拒绝分析，不要使用 FileOpen 替换用户工程。

```powershell
foreach ($loss in @(0, 3, 10, 1)) {
    & 'C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe' `
      -NoProfile -NonInteractive -ExecutionPolicy RemoteSigned `
      -File scripts/reference/inspect-reference-workspace.ps1 `
      -WorkspacePath D:/project/RFModel/build-reference/RFModel_AttenuatorNoise.wsv `
      -RunAttenuatorAnalysis -LossDb $loss -CaptureRun `
      > "build-reference/attenuator-scan-$loss.json"
    if ($LASTEXITCODE) { throw 'Reference capture failed' }
}
python scripts/reference/collect-attenuator-scan.py `
  build-reference validation/systemvue-2023-attenuator-loss-scan.json
python scripts/reference/compare-attenuator.py `
  validation/systemvue-2023-attenuator-loss-scan.json `
  build-msvc/Release/reference_attenuator.exe `
  validation/systemvue-2023-attenuator-loss-scan-comparison.json
```

最后一步当前返回 1，正确表示存在比对差异。重放已提交快照只需最后一步，不需要 SystemVue。比对程序现接受一个 0–100 dB 衰减参数，默认仍为 1 dB；它不包含温度或频率扫描能力。

本轮采集/校验器回归 6/6 通过；C++ 比对程序 Debug/Release 在四个点输出一致，非法数值和尾随垃圾参数被拒绝。63 个 C++ 文件通过格式检查。核心仿真头文件未改动，先前 34/34 的核心回归记录继续保留，不将本轮定向检查说成重新执行了全套。
