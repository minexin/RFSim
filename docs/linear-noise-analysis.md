# 线性 JSON 的逐频点噪声分析

在线性网络文件中添加 noise_analysis 对象，命令行会在每个频点输出 NF、NFmin、GammaOpt 与物理噪声电阻 Rn。数值计算调用 C++ 的[复参考二端口噪声接口](power-wave-noise.md)。

可运行文件 examples/linear-noise-analysis.json 包含两个频点、不同的复输出参考和不同的物理源阻抗：

~~~json
"noise_analysis": {
  "reference_temperature_k": 290,
  "source_impedance_samples_ohms": [50, [25, 30]]
}
~~~

~~~powershell
python -m rfmodel examples/linear-noise-analysis.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/linear-noise-analysis-result.json
~~~

## 输入约定

noise_analysis 必须是对象，允许空对象，其默认物理源阻抗为顶层 reference_ohms，默认参考温度为 290 K。

| 字段 | 要求 |
|---|---|
| reference_temperature_k | 有限、严格正的噪声参考温度；不会修改器件实际温度 |
| source_impedance_ohms | 可选，所有频点共用的物理源阻抗 |
| source_impedance_samples_ohms | 可选，每个频点一个物理源阻抗；与上一项互斥 |

阻抗采用实数或 [实部, 虚部]，必须有限且实部严格为正。逐频点数组必须精确覆盖 frequencies_hz。null、布尔值、未知字段、重复字段或两种源阻抗输入同时出现均报错。

必须恰好有两个 external_ports，方向是列表中第一个端口→第二个端口。更改列表顺序会改变噪声分析方向。必须显式提供内生噪声：统一器件温度、全局协方差或逐器件噪声模式均可；Touchstone 噪声也可。没有噪声数据时明确报错。要表达理想无噪声器件，需要显式 noise.noiseless=true 或零协方差。

## 输出与参考

每个 samples 项新增 noise_analysis：

| 输出字段 | 含义 |
|---|---|
| noise_figure_db | 指定物理源阻抗下的 NF，dB |
| minimum_noise_figure_db | 最小 NF，dB |
| optimum_source_reflection | GammaOpt，复数 [实部, 虚部] |
| noise_resistance_ohms | 物理 Rn，Ω |
| source_impedance_ohms | 当频点使用的物理源阻抗 |
| reference_impedances_ohms | 当频点两个端口的功率波参考 |
| reference_temperature_k | 本次分析的噪声参考温度 |
| wave_definition | 固定 power |
| source_reflection_convention | 固定 a_over_b，表示源边界 a=Gamma*b |

请求输出参考转换时，先同步转换 S 与内生噪声，再做噪声后处理；因此 GammaOpt 与该频点结果 S 的波坐标一致。源阻抗是物理值，不随输出参考的更改而更改。默认源阻抗也保持顶层公共参考的物理值，避免仅改变输出坐标就改变 NF 的测量条件。

signal_boundaries 和 noise_boundaries 对应实际已加载网络，另行生成 signal/loaded_noise。它们不会覆盖 noise_analysis 的独立源阻抗，也不会把负载或源自身热噪声混入内生协方差。此分析使用无噪声输出终端，不能用其 NF 替代带噪负载的整体信噪比。

## 错误和验证

任一频点的噪声参数不存在或无法可靠计算，例如正向传输为零或最优源仅在单位圆边界达到，整次文件分析失败。错误包含频率；命令行在全部频点成功前不写结果，因此后续频点失败不会覆盖已有结果文件。

新增九项回归覆盖解析无源损耗、复杂参考及源阻抗扫描、端口反向、参考温度、显式无噪声、加载边界独立性、Touchstone 噪声输入、严格格式校验、频率错误定位和 CLI 保留输出。默认不添加 noise_analysis，保持旧结果格式。

本阶段未增加 SystemVue 实测证据。其 CS 单位、GammaOpt 复参考约定及退化行为仍待核验。

## 本阶段工程记录（2026-10-09）

- 新增文件回归 9/9，通过本机 Python 3.10 与独立 Python 3.12 wheel 两条路径验证。
- Debug/Release CTest 各 87/87。本阶段只改 Python、测试注册与文档，使用上一阶段已干净构建通过的原生库，未重复声称进行了原生 clean-first 构建。
- 独立 wheel API 85/85、系统图 51/51，命令行样例输出的 NF/Rn 解析值检查通过。
- 跨平台构建、完整 CTest 与安装消费由本次 GitHub Actions 运行核验。
- 尚未完成的 SystemVue 高阶采集草稿继续留在工作目录，不随本阶段提交发布。
