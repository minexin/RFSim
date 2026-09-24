# 衰减器温度扫描实测

2026-09-24 使用本机 SystemVue 2023.0.0.11903，在专用参考副本
RFModel_AttenuatorNoise 中固定 100 MHz、1 dB、50 Ω、源功率 1e-19 W，
依次设置环境温度 250、300、350、290 K。最后恢复 290 K、1 dB。
所有运行的错误列表为空，UTC 运行区间为 10:00:21–10:00:27。
每点校验实际 RoomTemp、衰减值、数据集时间戳和五种测量向量。

原始精简数据、绝对比较、残差分解分别位于：

- `validation/systemvue-2023-attenuator-temperature.json`
- `validation/systemvue-2023-attenuator-temperature-comparison.json`
- `validation/systemvue-2023-attenuator-temperature-residuals.json`

| 环境温度 K | SystemVue CNF（线性） | 源 CND（W/Hz） | CND / 温度（W/Hz/K） |
|---:|---:|---:|---:|
| 250 | 1.22321156189157 | 3.45162591370935e-21 | 1.38065036548374e-23 |
| 300 | 1.26785387426982 | 4.14195109645122e-21 | 1.38065036548374e-23 |
| 350 | 1.31249618664807 | 4.83227627919309e-21 | 1.38065036548374e-23 |
| 290 | 1.25892541179417 | 4.00388605990284e-21 | 1.38065036548374e-23 |

源噪声随环境温度成比例变化，四点相对 SI kT 偏差均约 +0.989016 ppm。
输出/输入噪声比相对 1 偏差约 +0.000142 ppm。
这支持优先检查源噪声基准，而不是增加衰减器噪声经验修正。
噪声除以温度得到的是整个源节点计算后的有效比例，不能证明内部常数定义。
还需检查源功率、端口模型和历史常数/数值处理的影响。

RFModel 探针现在接受 `reference_attenuator [loss_db] [temperature_k]`，
默认仍为 1 dB、290 K。源热噪声和器件物理温度一起变化；
噪声因子参考温度保持 290 K，理想预期为 `F = 1 + (L - 1) * T / 290`，
其中 L 为线性损耗。实测 CNF 在当前阈值下符合此定义。

固定相对容差 1e-7，16 项中 12 项通过、4 项未通过。
各点增益误差约 0.005 ppm、信号功率误差约 0.030 ppm，噪声因子通过；
输出噪声密度误差约 0.989157 ppm，均未通过。不修改 SI 常数或放宽容差。
范围仅是单个衰减器的温度对比，不代表 RF Design 库兼容完成。

## 复现

先打开独立参考副本；脚本拒绝在存在其他工作区时运行分析。
TemperatureK 接受 (0, 1000] K，并转换为 RoomTemp 所用的摄氏度；
不会保存工作区。以下命令在项目根目录执行：

```powershell
New-Item -ItemType Directory -Force build-reference/temperature | Out-Null
foreach ($temperature in @(250, 300, 350, 290)) {
    powershell.exe -NoProfile -NonInteractive -ExecutionPolicy RemoteSigned `
      -File scripts/reference/inspect-reference-workspace.ps1 `
      -WorkspacePath D:/project/RFModel/build-reference/RFModel_AttenuatorNoise.wsv `
      -RunAttenuatorAnalysis -LossDb 1 -TemperatureK $temperature -CaptureRun `
      > "build-reference/temperature/attenuator-temperature-$temperature.json"
    if ($LASTEXITCODE) { throw 'Reference capture failed' }
}
python scripts/reference/collect-attenuator-scan.py build-reference/temperature `
  validation/systemvue-2023-attenuator-temperature.json --temperatures 250 300 350 290
python scripts/reference/compare-attenuator.py `
  validation/systemvue-2023-attenuator-temperature.json `
  build-msvc/Release/reference_attenuator.exe `
  validation/systemvue-2023-attenuator-temperature-comparison.json
# 比较程序返回 1 表示已记录的差异，随后可单独运行诊断。
python scripts/reference/analyze-attenuator-residuals.py `
  validation/systemvue-2023-attenuator-temperature.json `
  validation/systemvue-2023-attenuator-temperature-residuals.json
```

本轮工具回归 8/8 通过，探针 MSVC Debug/Release 编译通过且四个温度结果一致；
0、负温度、超范围、NaN、Inf、尾随字符均被拒绝。
原四点损耗扫描重放的检查数值保持不变。63 个 C++ 文件通过格式检查。
核心仿真头文件未改动，本轮没有重新执行全套核心 CTest。
