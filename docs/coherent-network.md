# 多端口线性网络中的相干传播

本接口把已明确相干关系的离散 RF 输入送入完整线性网络，求指定外部端口的
出射功率波。它连接了多端口 S 参数求解与显式相干分组合并，可表达功分后经不同
相位路径再合路，以及多个独立源同时进入一个合路器。

## 端口与相干语义

输入 PortCoherentComponent 包含 input_port 和 CoherentComponent。
input_port、output_port 都是网络全局端口编号，不是 external_ports 中的位置。
改变 external_ports 的排列不改变物理结果；输入和输出必须出现在该列表中。
允许输入出现在观察端口上，此时贡献通过 S(out,out) 反射到出射波。

选中的外部端口均采用匹配入射边界。其他端口必须已连接或终端化，终端的独立
源必须为零。内部反射及线性反馈由已有网络求解器处理；不额外叠加隐含源，
不表示外部源失配下的净输入功率，也不同时求噪声。

对每个频率先提取整个网络的外部 S 矩阵。每个输入组的输出幅度为
S(output,input) × incident_amplitude。随后按
(bin,kind,bandwidth_hz,coherence_group) 合并复幅度，不同键只相加功率。
同一输入端口上的重复相干贡献先合并，再进行传输。

输出 CoherentReduction 保留抵消后的零幅度组。组 ID、类别、频率和带宽在
线性传播中保持，不按频率重新生成相干 ID。调用者仍负责确认源/时钟关系。
跨级非线性产物的身份推导、部分相关统计量和变频后相干分组不属于此接口。

## C++ / C / Python

C++ 头文件 rfmodel/coherent_network.hpp：

    auto result = rfmodel::transmit_coherent_network(
        spacing_hz, port_components, external_ports, output_port,
        reference_ohms, build_network_at_frequency);

回调接收频率 Hz，返回完整 LinearNetwork；应保持端口编号含义一致。
每个输入 bin 调用一次，零幅度组仍进行网络校验。空输入在 0 Hz 调用一次，
用于校验拓扑和参考阻抗；若模型不支持 DC，空输入也会报错。
网络参考阻抗必须等于请求的正实参考阻抗。

C 函数 rfmodel_network_transmit_coherent 接收既有网络句柄和
rfmodel_port_coherent_component 数组，输出组数组、每 bin 功率数组、两个计数和
总功率。它使用句柄中固定的 S 参数，没有频率回调；频变模型应逐频构造网络。
容量和指针校验失败不会修改任何输出。缓冲区必须互不重叠。
新增符号保持 ABI 版本 1，原有结构不变。

Python Network.transmit_coherent 对应固定 S 参数的 C 接口，持有网络锁，
遵循现有 close/上下文管理器生命周期。返回 CoherentReduction：

    from math import sqrt
    from rfmodel import Library, CoherentComponent, PortCoherentComponent, SpectrumKind

    library = Library("build-msvc/Release/rfmodel_c.dll")
    source = CoherentComponent(10, SpectrumKind.SOURCE, 1., 7, 1.)
    with library.network() as network:
        k = 1 / sqrt(2)
        network.add([[0, 0, k], [0, 0, k], [k, k, 0]])
        inputs = [
            PortCoherentComponent(0, source),
            PortCoherentComponent(1, source._replace(amplitude=-1.)),
        ]
        result = network.transmit_coherent(1e8, inputs, [2, 1, 0], 2)
        assert result.total_power_w == 0.
        assert len(result.components) == 1

把第二路 coherence_group 改为 8，两个独立源的输出总功率为 1 W；
把它保持为 7 并令 amplitude=1，两路同相输出总功率为 2 W。
这些输入幅度单位都是 sqrt(W)。

上限为 4096 个输入分量、2048 个不同频率和 1024 个外部端口；
所有输入元数据、频率与功率必须有限有效。容量限制在调用频率回调前检查，
传播后的幅度功率及总功率溢出整体失败。输出排序和零项规则见 coherence.md。

## 回归与待完成工作

coherent_network_test 包含：
- 同相相加、反相相消、独立源功率相加及乱序外部端口映射；
- 两器件内部失配反馈，解析幅度 .5×.4/(1−.2×.3)；
- 实际功分器、两条支路及合路器连接，一路延迟 0.5 ns，在 1 GHz 抵消、
  在 2 GHz 相加；
- 抵消输出驱动基波压缩模型时保持零输出；
- 非法端口、重复端口、错误参考阻抗、空输入拓扑、资源上限和数值溢出。

C 回归检查不足的两类输出容量和非法输入均不修改输出；Python 检查闭合句柄、
布尔索引、显式分组、内部反馈和隐含源拒绝。安装消费者分别调用 C 导出函数
和安装后的 C++ 头文件。

当前证据是解析计算和原生 API 回归。此前 8 个 SystemVue 载频节点功率点验证的
是显式相干合并，不能替代本多端口传播接口的实测验收。
后续应采集 SystemVue 分支/再合路及相位扫描。频变网络 JSON/CLI 现已通过独立的
rfmodel.coherent-network 格式提供，见 [文件接口](coherent-network-file.md)；原
rfmodel.coherence 格式保持不变。当前不宣称完整 RF System Analysis 或多级非线性闭环兼容。

## 2026-10-09 本地验收

MSVC Debug/Release 清理重建后全套 CTest 各 61/61，内含 Python API 58 项测试。
两种配置的独立安装 C/C++ 消费者各 2/2。格式检查覆盖 94 个 C/C++ 文件。
构建及回归日志位于 build-reference/coherent-network-*.log，CTest JUnit 位于
build-msvc/Debug-results.xml 与 Release-results.xml。这些本地产物不提交到版本库。
跨平台结论以本次提交的 Windows、Ubuntu、macOS × Debug/Release CI 为准。

首个 SystemVue 双源同相合路参考已补充，见 [实测记录](systemvue-coherent-network.md)。已补充独立时钟和反相扫描：路径复幅度及源分组通过，RFPwrIn 存在五组差异；方向总谱六组、12 项功率比较已通过；单源功分再合路仍待完成。
