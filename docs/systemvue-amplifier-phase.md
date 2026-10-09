# SystemVue 复幅度与相干语义验证

2026-10-09 基于已安装 SystemVue 2023 的本机帮助和专用 RFAMP 工作区，
将原来的单项功率对照扩展为复幅度对照。数值模型和 1e-7 阈值未调整。

## 官方定义与当前边界

本机帮助文件为 C:/Program Files/Keysight/SystemVue2023/Help/systemvue.qch。
相关条目：

- sim/Coherency.html：相干项按电压与相位合并；不相干项按功率合并。
- sim/Node_Measurements.html：V2 为节点 2 的复电压，独立变量是 F2 和 ID2；
  IDNo/IDName 映射每个频谱身份。
- sim/Voltage_Measurements.html：电压测量采用峰值电压。
- sim/Spectrasys_Desired_Channel_Phase.html：DCPH 仅包含指定路径方向的期望信号，
  不代表各互调、谐波项的相位。
- rfdesign/MultiSource.html：源 Phase 参数单位为度，本机求值后的参数数据为弧度。

相干性要求参考时钟/源关系、谱类别、中心频率、带宽等条件一致；混频还涉及 LO。
同频率不意味着相干，来源信号与互调谱也不能只因频率重合就相干合并。
IDName 开头花括号中的编号表示相干组，数值只在本次仿真中有意义；
它与 IDNo 中的频谱记录 ID 不同，均不能作为跨次运行的稳定身份。

目前 AmplifierMixingTerm 只携带本级输入频率来源，不包含完整时钟、谱类别、
带宽和跨级相干关系。现有分阶谱的复数和是数学分解结果，不能自动解释为
SystemVue 的节点总功率。新增报告保存实测相干编号供后续验证，但没有据此
隐式合并、修改核心模型或宣称已实现完整相干传播。

## 复电压换算与校验

专用实例采集的 V2、Z2 在 COM 输出中为实部/虚部交错的一维数值数组，
长度必须为 ID2 的两倍。每项通过完整来源表达式匹配，随后验证两个 CW 边界。
两个边界的复幅度必须一致。只接受实数、约 50 ohm 的负载阻抗。

在该匹配参考条件下，用实测 R=Re(Z2) 校验 P=|Vpeak|²/(2R)，并换算
功率波 a=Vpeak/sqrt(2R)。实测阻抗约 49.99999975 ohm，使用原始求值结果，
不强制取整以掩盖差异。电压和功率一致性相对阈值为 1e-10，
两个边界幅度一致性阈值为 1e-12。非实阻抗、错误形状、非有限值或不一致都拒绝。

原生输入由每路功率和相位分别构造。比较同时要求功率误差与
|a_RFModel−a_SystemVue|/|a_SystemVue| 不超过 1e-7；报告另列相位差，
没有相位拟合、逐项翻转或额外校准。功率比较仍可单独运行，但不能支持相位结论。

## 实测结果

| 参数组 | 两路输入 dBm | 两路相位 deg | 谱项数 | 最大复幅度相对误差 |
| --- | --- | --- | ---: | ---: |
| sample | −3 / −12 | 0 / 0 | 16 | 3.6228647e-8 |
| sample | −3 / −12 | 30 / −45 | 16 | 3.6228648e-8 |
| sample | −3 / −12 | 120 / 90 | 16 | 3.6228648e-8 |
| limiter | 3 / −6 | 30 / −45 | 16 | 3.3843953e-8 |

64 项的功率和复幅度全部通过，最大相位差约 1.81e-14 度。
包含直接项、差频共轭、二阶项、三阶负系数以及载频处的独立三阶项。
零相位组复用上一轮原始采集中的 V2/Z2，其余三组为本轮新采集。
每组均验证源相位弧度、完整参数、空 manager 消息及两个数据集的新鲜度。

精简证据为 validation/systemvue-2023-amplifier-phase-captures.json；
报告为 validation/systemvue-2023-amplifier-phases.json，保留原始采集、精简文件和
DLL 的 SHA256。对应原始目录记录在各 capture 的 raw_capture_directory。

## 复现与回归

采集器增加 --compression-first-phase-deg 和 --compression-second-phase-deg，
只允许受控双音模式，有限值范围 −360..360 度；省略时各路为零。
进入单音模式显式恢复零相位。例：

    python scripts/reference/run-systemvue-reference.py compression build-reference/RFModel_AmplifierCompression.wsv build-reference/new-phase-run --source-power-dbm -3 --compression-two-tone --compression-second-power-dbm -12 --compression-first-phase-deg 30 --compression-second-phase-deg -45

复幅度重放必须显式开启：

    python scripts/reference/compare-amplifier-terms.py build-msvc/Release/rfmodel_c.dll validation/systemvue-2023-amplifier-phase-captures.json build-reference/phase-replay.json --complex-amplitudes

source_phases_deg 元数据缺省为 [0,0]，有值时必须严格匹配实测弧度参数。
实验唯一键包含 profile、两路功率和两路相位，允许比较相同功率下的相位变化。

来源比较器现含 13 项回归：实测复幅度通过；翻转两边界电压符号时功率仍通过、
复幅度恰有该项失败；缺失 V2、错误维度、布尔值、NaN、非实阻抗、电压与功率
不符、单边界相位变化、错误相位元数据均拒绝。后台 runner 10 项通过。

最终恢复为原 sample 的 −30 dBm 单音，原始目录
build-reference/compression-phase-restored-001；新数据、参数、零相位及 H2/H3
均核验通过。后续工作需要相干组数据结构、分支合路与多级非线性验证，以及
不同频距下的交叉验证；本轮不代表完整 RF System Analysis 已交付。

本轮 MSVC Debug/Release 全套 CTest 各 58/58 通过；数值核心及 C ABI 未修改。
跨平台结果绑定本阶段提交的 CI，不能从此推断全部相干传播已通过对照。

后续 [显式相干合并接口](coherence.md) 已提供给定分组后的离散功率合并，
相干关系仍由调用者或参考数据明确提供，不依赖同频率假设。
