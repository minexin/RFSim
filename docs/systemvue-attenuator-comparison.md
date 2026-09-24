# SystemVue 2023 首个实测对照：1 dB 衰减器

后续进展见 [四点衰减量扫描](systemvue-attenuator-scan.md)：噪声密度差异持续存在，0 dB 点另有输出信号功率差异。本文保留首个用例的原始结论。

2026-09-24 已实际运行本机 SystemVue **2023.0.0.11903**，取得新生成的结果，而非仅比较样例缓存。范围为参考匹配的 ATTN_Linear：100 MHz、衰减 1 dB、290 K、50 Ω、源可用信号功率 1e-19 W。

## 输入和运行证据

官方样例为 `Examples/RF Architecture Design/RF Design Kit/Noise/Attenuator Noise.wsv`。项目内使用 `build-reference/RFModel_AttenuatorNoise.wsv` 副本，未提交厂商工程。源文件 SHA-256、运行起止 UTC、数据集时间戳和测量向量保存在 `validation/systemvue-2023-attenuator-1db.json`。

独立实例 PID 15416 的 `Designs/System1.RunAnalysis()` 于 08:43:49.1488488Z 开始、08:43:50.1776220Z 返回。Manager.GetErrors 为空；数据集时间戳 1790239430 对应本次运行结束秒。分析后从 `Designs/System1_Data_Folder/System1_Sch1_Data_Path1/Eqns/VarBlock` 提取 CF、CGAIN、CNF、CND、DCP，每个向量保留源节点和输出节点两项。

## 比对结果

RFModel 的 `reference_attenuator` 可执行程序调用实际 C++ 匹配衰减器、线性路径、热噪声协方差及噪声因子接口。输出噪声同时包含匹配源和衰减器自身贡献。统一相对误差阈值为 1e-7。

| 测量 | RFModel | SystemVue | 相对误差 | 结果 |
|---|---:|---:|---:|---|
| 增益（线性比） | 0.7943282347242817 | 0.7943282307526429 | 5.00e-9 | 通过 |
| 噪声因子（线性比） | 1.2589254117941668 | 1.2589254117941686 | 1.41e-15 | 通过 |
| 输出噪声密度（W/Hz） | 4.0038821e-21 | 4.003886060470589e-21 | 9.89e-7 | 未通过 |
| 输出期望信号功率（W） | 7.943282347242817e-20 | 7.943282108944365e-20 | 3.00e-8 | 通过 |

完整差异见 `validation/systemvue-2023-attenuator-1db-comparison.json`，总体 `passed=false`。RFModel 使用 k=1.380649e-23，官方样例方程使用 1.3806503e-23。差别与噪声误差同量级，但样例方程不能证明内部仿真器常数，不能据此确认全部误差原因。后续需温度/衰减扫描及噪声源分解；当前保留 RFModel 常数和未通过阈值。

官方 `sim/Spectrasys_Cascaded_Gain.html` 指出 CGAIN 起点使用内部源参考并包含首级失配。本用例为参考匹配，不能证明任意失配下路径定义完全一致，更不代表整个器件或库已经兼容。

## 复现比对

```powershell
./scripts/build-msvc.ps1 -Configuration Release
python scripts/reference/test_compare_attenuator.py
python scripts/reference/compare-attenuator.py `
  validation/systemvue-2023-attenuator-1db.json `
  build-msvc/Release/reference_attenuator.exe `
  validation/systemvue-2023-attenuator-1db-comparison.json
```

最后一条当前返回 **1**，表示已知的噪声密度差异。验证器拒绝参数不符、运行错误、旧时间戳、错误载频、缺失/重复测量、向量尺寸不符和非有限值。验证器回归 4/4 通过；MSVC Debug/Release 核心各 34/34 通过，63 个 C++ 文件格式检查通过。CI 执行验证器回归并构建比对程序；已知不通过的完整差异比对未伪装成通过的门禁。

## 重新采集参考

无现有 SystemVue 实例时，按本机已注册的文件打开方式，使用 SystemVue.exe 加参考副本绝对路径启动。不要对含用户工程的实例调用 Manager.FileOpen：本机确认该操作可能替换当前工作区并弹出保存提示。最初打开尝试因此没有完成；用户随后手动关闭旧实例，之后才成功启动独立参考实例。

独立实例加载完毕后执行：

```powershell
& 'C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe' `
  -NoProfile -NonInteractive -ExecutionPolicy RemoteSigned `
  -File scripts/reference/inspect-reference-workspace.ps1 `
  -WorkspacePath D:/project/RFModel/build-reference/RFModel_AttenuatorNoise.wsv `
  -RunAttenuatorAnalysis > build-reference/attenuator-run.json
```

脚本限定 build-reference 内的 RFModel_*.wsv；执行分析时要求实例仅含指定衰减器工作区。去掉 RunAttenuatorAnalysis 只读检查。stdout 为对象和数据，stderr 为阶段、运行时间及管理器错误。更新参考快照时必须同时更新此次运行元数据，不可沿用旧时间戳。脚本不启动/关闭实例，也不保存工程；OpenCopy 在已有工作区时拒绝执行。
