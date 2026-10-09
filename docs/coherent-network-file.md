# 相干网络 JSON 与命令行工作流

rfmodel.coherent-network version=1 描述显式分组的 RF 输入和完整线性网络。
它把频变器件、内部反射、多输入合路及相干功率合并连接成可批量执行的模型文件。
所有 S 参数计算、网络求解、复幅度传播和相干合并调用原生 C++ 核心；
Python 负责文件解析、端口映射与逐频调度。

## 输入契约

顶层必填 format、version、spacing_hz、network、inputs、output_port；
可选 reference_ohms（默认 50）和 sources。间隔与实参考阻抗必须有限且为正数。

network 必填 devices、external_ports，可选 connections、terminations。
字段沿用 rfmodel.linear-network，但不包含频率表、噪声或外部激励边界：

- devices 每项包含唯一 id 和 s/model 二选一；
- s 是固定复数 S 矩阵；model 支持既有 transmission_line、rlgc_line、
  linear_amplifier、touchstone；
- external_ports 是 [device_id, local_port_index] 数组，局部端口从零开始；
- connections 每项连接两个端点；terminations 按现有语法指定 port 和 reflection；
- external_ports 的列表顺序只决定内部矩阵排列，不改变实际输入/输出端口；
- s_samples 被拒绝，避免对由输入 bin 自动推导的频率表作隐含顺序解释。

inputs 每项含 port 和 component。port 必须在 external_ports 中；
component 包含 bin、kind、bandwidth_hz、coherence_group、amplitude，
与 rfmodel.coherence 使用同样的类型和校验规则。kind 为 source、harmonic、
intermod；幅度可为实数或 [real,imag]，单位 sqrt(W)。
output_port 也使用 [device_id, local_port_index]，必须为选中的外部端口。

允许同一输入端口有多项相干贡献，先在该端口内合并。不同输入端口必须分别传播
之后才能合并，即使频率和组 ID 相同，也不能在进入网络之前跨端口抵消。
输入和观察端口可以相同，此时包含该端口的反射波。

拒绝未知字段，包括设备噪声、温度、signal_boundaries 和 noise_boundaries。
独立噪声协方差及外部源失配不属于本工作流，不能静默忽略这些字段。
未提供 sources 时，相干组 ID 由调用者明确提供；提供 sources 时由原生源/参考时钟解析器生成。
源定义模式仅接受确定性 source 分量，具体字段与限制见 [源相干关系](source-coherence.md)。

## 逐频求解与结果

频率表由输入中的唯一 bin 排序并乘 spacing_hz 得到。每个频率重新计算器件、
提取完整网络的外部 S 矩阵，再通过原生多端口相干接口传播这一频率的分量。
输出 rfmodel.coherent-network-result version=1 包含：

- spacing_hz、reference_ohms、external_ports、output_port；
- components：传播并合并后的组，保留类别、带宽、组 ID 和复幅度；
- power_by_bin：各 bin、物理频率和离散总功率；
- total_power_w：各组功率总和。

相消到零的组仍保留。浮点运算可能留下极小残差，不人为截断或宣称严格零功率。
最多 4096 个输入分量、2048 个不同 bin、1024 个网络总端口。
无输入时仍在 0 Hz 校验器件和拓扑，返回空组、空功率表和零总功率；
不支持 DC 的 Touchstone 使用默认越界拒绝策略时，空输入也会失败。
显式 out_of_band=clamp 可采用端点值，规则与线性网络接口一致。

Touchstone 相对路径以输入 JSON 所在目录为基准。CLI 禁止输出覆盖输入模型、
原生库或该网络引用的 Touchstone 文件。分析失败时不会替换原有结果文件。
重复 JSON 键和非标准 NaN/Infinity 由已有严格加载器拒绝。

## 可运行示例

examples/coherent-split-delay.json 使用一个理想等分器和一个理想合路器；
一路直接连接，另一路经过 0.5 ns、50 ohm 的传输线。源在 1 GHz 与 2 GHz
分别输入幅度 1 sqrt(W)，预期功率分别约 0 W 与 1 W。

    python -m rfmodel examples/coherent-split-delay.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/coherent-split-delay-result.json

Python 也可直接调用：

    from rfmodel import Library
    from rfmodel.model_file import load
    from rfmodel.coherent_network_file import analyze_coherent_network

    result = analyze_coherent_network(
        Library("build-msvc/Release/rfmodel_c.dll"),
        load("examples/coherent-split-delay.json"),
        base_directory="examples",
    )

这是解析回归示例，不能代替单源功分再合路的 SystemVue 实测。
当前实测边界仍见 systemvue-coherent-network.md：已有同相双源合路，
反相/独立时钟及完整厂商 SPLIT/HYBRID 器件验收未完成。

## 验证

tests/test_coherent_network_file.py 的八项测试检查：
逐频功分延时再合路、乱序端点与跨端口合并顺序、重复组与同端口反射、
内部失配反馈/终端、严格字段与资源上限、空输入拓扑、Touchstone 插值和
默认 DC 策略，以及 CLI 对输入数据和已有结果文件的保护。

2026-10-09 验证记录：MSVC 原生 Debug/Release 配合 Python 3.10，全套 CTest
各 63/63。另用独立 Python 3.12/setuptools 构建 wheel，从 wheel 直接导入模块并
执行八项文件接口回归，全部通过；没有以源码导入冒充分发包验证。
SystemVue 自带 Python 的 setuptools 缺少 _distutils_hack，故本地 wheel 使用独立
构建环境；未修补厂商目录。本轮未修改 C++/C ABI，不涉及重新编译原生接口。
