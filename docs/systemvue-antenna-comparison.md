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
  build-reference/antenna-run-001/capture.json validation/systemvue-2023-antenna.json
python scripts/reference/compare-antenna.py compare `
  validation/systemvue-2023-antenna.json validation/systemvue-2023-antenna-comparison.json `
  --executable build-msvc/Release/reference_antenna.exe
```

比较程序当前返回 1，表示差异已记录。仅需重放时执行第二条，不必启动 SystemVue。
此项结果不等同于完整 RF System Analysis 或 RF Design 库兼容验收。

本轮 MSVC Debug/Release 完整 CTest 各 36/36 通过，两个配置的探针 JSON 完全一致。
新采集比较校验测试 3/3 通过，覆盖旧数据、歧义路径、错误载频、真实数值偏差与 NaN。
66 个 C++ 文件格式检查通过。
