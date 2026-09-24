# 衰减器源功率扫描

2026-09-24 UTC 10:06:41–10:06:47，对专用 RFModel_AttenuatorNoise 副本执行
−120、−60、0、−160 dBm 四点实测，固定 100 MHz、1 dB、290 K、50 Ω。
最后恢复 −160 dBm。所有分析错误列表为空，数据集时间戳通过校验。

采集脚本增加 SourcePowerDbm 参数，并读取 Source/ParamSet/Pwr 的实际值。
其 Data 为瓦特，采集器按 `10^((dBm - 30)/10)` 校验，而不是仅记录命令参数。
旧捕获没有源节点时仍可重放；明确指定源功率的扫描必须存在唯一且匹配的回读值。
频率沿用固定参考工程，并由两端 CF=100 MHz 校验；端口阻抗仍是参考工程默认条件，
尚未扩展为可配置阻抗扫描。

| 源功率 dBm | 源信号相对设定值 ppm | DCP 输出/输入相对理想增益 ppm | 源噪声相对 SI kT ppm |
|---:|---:|---:|---:|
| -120 | -0.025000 | -0.005000 | +0.989016 |
| -60 | -0.025000 | -0.005000 | +0.989016 |
| 0 | 0 | -0.030000 | +0.989016 |
| -160 | -0.025000 | -0.005000 | +0.989016 |

四点源噪声密度均为 4.00388605990284e-21 W/Hz，输出均为
4.00388606047059e-21 W/Hz。当前采样不支持信号功率导致噪声偏差的解释。
0 dBm 的源节点信号归一化有所不同，但不影响上述噪声观察；尚未确定内部原因。

绝对比较保持相对容差 1e-7：16 项中 12 项通过，4 项输出噪声密度失败，
误差约 0.989157 ppm。增益、噪声因子和信号功率均通过。
不改变物理常数、核心算法或容差。本结果仅限该参考电路。

## 证据和复现

原始精简数据、绝对比较、残差报告位于 validation 目录中的
`systemvue-2023-attenuator-power.json`、
`systemvue-2023-attenuator-power-comparison.json`、
`systemvue-2023-attenuator-power-residuals.json`。

探针用法现在为 `reference_attenuator [loss_db] [temperature_k] [source_available_w]`。
省略参数的行为不变。以下采集要求专用参考副本已经打开：

```powershell
New-Item -ItemType Directory -Force build-reference/power | Out-Null
foreach ($power in @(-120, -60, 0, -160)) {
    powershell.exe -NoProfile -NonInteractive -ExecutionPolicy RemoteSigned `
      -File scripts/reference/inspect-reference-workspace.ps1 `
      -WorkspacePath D:/project/RFModel/build-reference/RFModel_AttenuatorNoise.wsv `
      -RunAttenuatorAnalysis -LossDb 1 -TemperatureK 290 -SourcePowerDbm $power -CaptureRun `
      > "build-reference/power/attenuator-power-$power.json"
    if ($LASTEXITCODE) { throw 'Reference capture failed' }
}
python scripts/reference/collect-attenuator-scan.py build-reference/power `
  validation/systemvue-2023-attenuator-power.json --powers-dbm -120 -60 0 -160
python scripts/reference/compare-attenuator.py `
  validation/systemvue-2023-attenuator-power.json `
  build-msvc/Release/reference_attenuator.exe `
  validation/systemvue-2023-attenuator-power-comparison.json
# 当前返回 1 表示有差异；以下诊断单独执行。
python scripts/reference/analyze-attenuator-residuals.py `
  validation/systemvue-2023-attenuator-power.json `
  validation/systemvue-2023-attenuator-power-residuals.json
```

工具回归 9/9 通过。MSVC Debug/Release 探针构建通过、四点输出一致；
非法功率输入被拒绝，旧温度扫描重放的检查数值不变。
63 个 C++ 文件格式检查通过。未修改核心仿真头文件，本轮未重新执行完整 CTest。

后续保留这项量化差异，扩展链路级噪声接口及多器件参考测试，
避免将单个匹配衰减器的通过项等同于 RF System Analysis 完成。
