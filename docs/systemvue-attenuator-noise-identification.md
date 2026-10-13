# ATTN_Linear 噪声差异辨识（2026-10-12）

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

本阶段取得两个新的非等温用例。固定源/分析温度 290 K、损耗 1 dB、源功率 −160 dBm 和频率 100 MHz，仅把衰减器 Ta 分别设为 300 K、350 K。源噪声不变，器件自身噪声随 Ta 变化。独立斜率得到有效系数 **1.380650399999844e−23 J/K**。预登记的分量预测在两个新用例中均成立，最大相对残差 **1.46e−14**。

这确认了该条件下的有效噪声分解，不证明 Spectrasys 的内部常量或数值正则化实现。没有发现应修改 RFModel SI 常数的物理错误；原 1e−7 比较仍失败，没有提升器件兼容状态。

## 证据和重放

- [新实验机器证据](../validation/systemvue-2023-attenuator-noise-identification-evidence.json)：实际表达式/值、有效温度、路径结果、原始 V/Z/P 摘录、采集哈希、时间戳、恢复回读、预登记预测。
- [50 位 Decimal 诊断](../validation/systemvue-2023-attenuator-noise-identification.json)：逐分量预测、候选常数、SI 比较及历史反例。
- [阶段耗时及保护检查](../validation/attenuator-noise-identification-stage-20261012.json)。
- [离线重放脚本](../scripts/reference/identify-attenuator-noise.py)与[六项证据回归](../scripts/reference/test_identify_attenuator_noise.py)。
- [后续调用与比较流程](systemvue-scripting.md)的 2026-10-12 更新节。

    python scripts/reference/identify-attenuator-noise.py validation/systemvue-2023-attenuator-noise-identification-evidence.json build-reference/noise-identification-replay.json

    python scripts/reference/test_identify_attenuator_noise.py

输出必须使用新文件名。脚本不连接 COM、不调用 SystemVue；退出 0 表示诊断检查通过，vendor_noise_subset_passed 仍为 false。非等温用例以独立数学为参考，没有将等温 reference_attenuator 探针冒充为非等温探针。

## 官方依据与预登记

本机 SystemVue2023/Help/systemvue.qch 的 rfdesign/ATTN_Linear.html 明确说明 Ta 使用 °C，非空时覆盖分析温度；有效输出 Ta 与参数回读均确认两个覆盖生效。官方 Algorithm 库的 AddNDensity、Amplifier、GaussianNoiseGen、Oscillator 帮助页列出 k=1.3806504e−23。RF ADC 帮助页及原样例另使用 1.3806503e−23。资料中的数字必须区分所属库，不能把 Algorithm 文档直接当作 Spectrasys 源码证明。

对应本机页面 SHA256 已归档。官方旧版手册也可检索到相同 Algorithm 常数：[Keysight SystemVue Algorithm Design Library](https://edadownload.software.keysight.com/eedl/systemvue/2010_07/pdf/algorithm.pdf)。本阶段候选以本机 2023 文档为依据。部分帮助示例的 kT 数字与写出的 k 并不完全一致，诊断只使用明确常数进行独立计算。

04:31:36.754387 UTC 写入预测，早于首次分析 04:31:47.7178341 UTC。350 K 是预先指定的留出用例，没有采集后回调系数。源与路径因子来自此前 1 dB 信号/噪声档案，是待验证的经验关系，不是已证明的内部机制：

    g = 10^(-1/10)
    k_candidate = 1.3806504e-23
    Nsource,out = k_candidate * 290 * g * (1 - 2.5e-8)
    Ndevice,out = k_candidate * Ta * (1-g)
    Npath,out = (Nsource,out + Ndevice,out) * (1 - 5e-9)

原始源端 P1 与路径输入 CND 也不是同一参考量，不能混合代入输出端源分量公式。

## 结果与排除项

| 器件温度 | 源输出分量 W/Hz | 器件自身分量 W/Hz | 路径 CND W/Hz | 相对 SI 非等温预期误差 |
|---|---:|---:|---:|---:|
| 290 K 已有基线 | 3.180399745999790e−21 | 8.234863344902218e−22 | 4.003886060470589e−21 | 9.89157e−7，失败 |
| 300 K 新实验 | 3.180399745999790e−21 | 8.518824149898930e−22 | 4.032282140828280e−21 | 9.89297e−7，失败 |
| 350 K 新实验/留出 | 3.180399745999790e−21 | 9.938628174881904e−22 | 4.174262542616675e−21 | 9.89967e−7，失败 |

SI 预期为 k_SI*[290*g+Ta*(1-g)]。器件温度差分消除了不变源项，三个事先指定候选中只有 1.3806504e−23 满足 1e−12 辨识检查。这一阈值用于区分假设，不替代原厂商兼容阈值 1e−7。

源项不变排除了“修改 Ta 同时改变源温度”；有效 Ta 和器件斜率排除了“Ta 覆盖未生效”；只校正源噪声、保持器件 SI 热噪声，在 300/350 K 的剩余输出误差约 2.14e−7/2.41e−7，仍失败。所有选定端点均满足 P=|V|²*Re(1/Z)/2，分量求和与总谱一致；总谱到路径约有 −5e−9 比例差。MHz/Hz、°C/K、dBm/W、dB/线性损耗比和节点索引均按回读核对。

默认阻抗参数仍为空，数据集给出接近 50 Ω 的有效阻抗，未宣称完整的逐项默认解析。CNF、增益和信号向量也一并保留；本阶段非等温数值判断针对 CND，不扩张为全器件验收。

## 适用范围的反例

没有重跑历史扫描，仅作事后交叉检查，不能把旧数据叫作新留出集。公式在旧 250/300/350/290 K 全局温度、源功率和 1/3/10 dB 用例中吻合；在 0、1e−8、1e−6、1e−4 dB 不满足 1e−12 辨识标准。1e−8 dB 残差约 3.18e−7，甚至超过原兼容阈值。因此两个经验因子不是通用修正，近零损耗下的算法处理仍未解释。

## 采集、恢复与停止

300 K 完整调用/返回时间为 04:31:47.7178341 至 04:31:47.9370084 UTC，数据集时间戳 1791779507。helper 总耗时 5.313 秒。

350 K 于 04:32:03.9372198 UTC 启动；监督器遇到 #32770 窗口 “System1 Simulation Status” 后在 helper 3.422 秒处终止了该 helper，SystemVue 没有被终止。该窗口是进度窗口候选，不能凭窗口类名判为错误或模态故障。没有重新仿真。随后查询 IsAnalysisDone=1、Manager 错误为空，回收时间戳 1791779524 的结果；04:32:28.7808355 UTC 记录完成观察，原 RunScript 返回时间保留为空。回收/恢复 helper 耗时 4.875 秒。自动化首次事务中断与服务端仿真完成分开记录。

所有 helper 使用普通 Windows PS5.1 文件调用，无 Process 策略覆盖；120 秒上限只作用于本次 helper。最终回读确认 Ta 恢复空白，1 dB、290 K、−160 dBm 和其余源/负载局部参数与基线一致。没有保存工作区、覆盖原文件或另跑基线；界面数据集仍属于最后的 350 K 实验，不能当作恢复后的新基线结果。

六项离线回归覆盖真实预测、源项污染、旧时间戳、错误有效温度、恢复不符和事后预测登记。没有修改原生模型，本阶段不重跑此前完整构建/CTest/wheel/CI。到此结束本轮有界辨识，不继续扫描、不拟合核心常数。

后续限定实验见[近零损耗两点辨识](systemvue-attenuator-near-zero-identification.md)：固定 −5e−9 路径因子可进一步解释为实际谱阻抗对应的参考功率换算；这是测量关系重构，不能替代对原始谱量生成机制的调查。
