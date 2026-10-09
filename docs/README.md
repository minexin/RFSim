# RFModel 文档索引

- [M1 建设目标](m1-goal.md)
- [总体架构](architecture.md)
- [核心接口](api.md)
- [外部边界反馈下的噪声](loaded-noise.md)
- [C ABI 线性网络接口](c-api.md)
- [Python 线性网络接口](python-api.md)
- [C/Python 非线性与混频频谱接口](spectrum-api.md)
- [基波压缩与饱和模型](saturating-fundamental.md)
- [单音基波与限幅谐波模型](single-tone-amplifier.md)
- [多音总功率限幅与分阶输出接口](multitone-amplifier.md)
- [互调来源项在线性后级中的传播](term-propagation.md)
- [匹配单向频谱链路文件](spectrum-model-file.md)
- [线性网络 JSON 文件与批处理](linear-model-file.md)
- [能力边界与验收矩阵](acceptance-matrix.md)
- [有阻抗失配的均匀传输线](transmission-line.md)
- [分布参数 RLGC 传输线](rlgc-transmission-line.md)
- [理想功分/合路器和正交耦合器](multiport-devices.md)

- [SystemVue 不等功率双音交叉验证](systemvue-unequal-two-tone.md)

- [SystemVue 复幅度与相干语义验证](systemvue-amplifier-phase.md)

- [显式相干分组与离散功率合并](coherence.md)

- [多端口线性网络中的相干传播](coherent-network.md)

- [SystemVue 双源相干合路参考](systemvue-coherent-network.md)

- [相干网络 JSON 与命令行工作流](coherent-network-file.md)

- [源与参考时钟的相干关系](source-coherence.md)

- [RF/LO 相干混频与镜像抑制链路](coherent-mixer.md)

- [相干 RF 系统前馈图与命令行](coherent-system-file.md)

- [相干载波共同基波压缩及系统图节点](coherent-compression.md)

- [SystemVue 多源共同压缩与噪声控制实测](systemvue-shared-compression.md)

[同频多源非线性放大器](coherent-amplifier.md) 已接入 C++/C/Python 与系统图；
21 组 SystemVue 记录的 206 个来源谱项通过比较，递归多级非线性仍待完成。

[失真来源级联](coherent-amplifier-cascade.md) 新增 cascaded_amplifier：传播前级谐波/互调，计入共同驱动，并与本级同源失真相干合并。八组 SystemVue 对照中六组通过，两组 0 dB 增益差异保留；次级失真再混频仍未完成。

[RFAMP 单位增益诊断](systemvue-unity-gain-diagnostic.md)：17 组受控采集支持等效功率增益 1.00000023 的行为解释；名义增益验收仍保留失败，诊断不算兼容性通过。

[九阶相干多项式](coherent-polynomial.md) 新增 C++/C/Python 接口，可由谐波、互调输入继续生成带本地来源的 RF 项；已做独立卷积验证，尚未完成 RFAMP 高阶系数、递归身份及子谱合并的兼容验收。

[递归来源与多项式系统图](recursive-mixing-origins.md) 新增原始源因子展开、同源失真归并及显式全局来源阶数限制。带混频器的来源表达式之和与 SystemVue 高阶子谱语义仍待完成。
