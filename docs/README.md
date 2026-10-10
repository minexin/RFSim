# RFModel 文档索引

- [M1 建设目标](m1-goal.md)
- [总体架构](architecture.md)
- [核心接口](api.md)
- [通道噪声积分与载噪比](channel-noise-measurements.md)
- [变频网络热负载与噪声功率流](conversion-loaded-noise.md)
- [变频网络参考温度噪声分析](conversion-noise-analysis.md)
- [多器件转换网络与线性器件接入](conversion-network.md)
- [固定泵频率转换、反馈与相关噪声](frequency-conversion.md)
- [理想无损 Chebyshev I 滤波器](chebyshev-filter.md)
- [理想无损 Butterworth 滤波器](butterworth-filter.md)
- [理想无源器件的 C/Python/JSON 接口](passive-model-interfaces.md)
- [独立复参考阻抗与功率波](power-wave-references.md)
- [独立复参考下的二端口噪声参数](power-wave-noise.md)
- [线性 JSON 的逐频点噪声分析](linear-noise-analysis.md)
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

[十一阶相干多项式](coherent-polynomial.md) 新增 C++/C/Python 接口，可由谐波、互调输入继续生成带本地来源的 RF 项；已做独立卷积验证，尚未完成 RFAMP 高阶系数、递归身份及子谱合并的兼容验收。

[递归来源与多项式系统图](recursive-mixing-origins.md) 新增原始源因子展开、同源失真归并及显式全局来源阶数限制。混频多来源传播已在后续阶段接入；SystemVue 高阶子谱语义仍待完成。

[来源表达式之和](origin-expressions.md) 新增实际复波贡献的求和、共轭和分配展开，C++/C/Python 均保留相消后的来源项；已用于系统图中的混频与多项式联合传播。

[混频与多项式的多来源系统图](mixed-polynomial-graphs.md) 支持混频折叠后的复波贡献经线性网络继续非线性展开；独立数值对照通过，共同压缩已接入，SystemVue 全局阶数/高阶系数仍待验收。

[多来源公共压缩响应](common-amplifier-response.md) 新增 C++/C/Python 工作点接口，按真实相干总功率共同压缩并保留各来源贡献，支持零驱动相消及既有失真级联。

[RFAMP 四阶与五阶诊断](systemvue-highorder-diagnostic.md)：六组实测中，公开四阶规则的 23 个已记录来源项吻合，单点识别的五阶系数通过另外 27 项检查；双音 12 个标签缺项已由后续频谱削减开关对照定位，完整高阶兼容仍未验收。

[频谱削减配对诊断](systemvue-spectrum-reduction.md)：三组双音功率配置关闭 UseSpecReduction 后均恢复完整的 44 个四、五阶来源项；开启时较弱重叠项缺失，共有的 92 项复波不变。五阶系数仍属固定参数识别，总谱与完整削减算法尚未验收。

[显式高阶截点转换](polynomial-intercepts.md)新增二至十一阶 C++/C/Python 参数接口，指定双音参考项、截点参考面及实系数符号，可接入现有多项式系统图；SystemVue 四阶诊断已使用此接口，自动高阶 RFAMP 系数仍待完成。

[十一阶多项式与 ABI 兼容](eleventh-order-polynomial.md)：C++/Python、显式 IP2..IP11 及系统图已扩展到十一阶；新增 C v2 符号，旧九阶结构与函数保留。证据为独立数值与工程回归，SystemVue 完整十一阶兼容尚未完成。

[RFAMP 四至十一阶实测诊断](systemvue-eleventh-order-diagnostic.md)：七组采集验证公共限幅与固定参数奇数阶系数；807 个奇数阶校准外来源项通过，八/十阶小残差及 10 个低功率缺项保留，自动系数算法仍未实现。

[高阶输入端口参考诊断](systemvue-highorder-input-reference.md)：从原采集归档九个源的实际输入电压，1413 个独立高阶评分项在原容差下通过；八/十阶名义残差由输入参考差异解释，完整网络输入求解和自动奇数阶系数仍未完成。

[显式高阶放大器](coherent-highorder-amplifier.md)：二至十一阶显式系数、共同压缩/限幅的 C++/C/Python/JSON 接口，以及失真传播、来源阶数裁剪和归档重放证据。

[IMN 输出互调功率参数](intermod-levels.md)：官方参考互调项、输出功率换算、显式符号和十一阶系统图示例。

[小角度相噪源](phase-noise.md)：相位相关边带的 C++/C/Python/JSON 接口、反射反馈传播与相位检波解析回归。

[共享参考相噪](shared-phase-noise.md)：全局相关源 C/P、多载波参考相位增益及跨支路共同相噪相消回归。

[RF/LO 工作点线性化](mixer-linearization.md)：双线性乘积导数、LO 相噪及共享时钟和/差传播的 C++/C/Python/JSON 接口。

[绝对波仿射网络](affine-conversion.md)：确定性偏置、给定混频工作点的网络一致性检查及 C++/C/Python/JSON 接口。

[自动非线性工作点](conversion-operating-point.md)：C++ 通用核心及双线性混频/公共基波压缩的 C/Python/JSON 接口、真实残差与收敛噪声。

[公共基波压缩导数](amplifier-linearization.md)：放大器 C++/C/Python/JSON 工作点接口、跨频点 A/B、混合器件与反馈噪声验证。

[高阶多项式工作点导数](polynomial-linearization.md)提供零至十一阶 C++/C/Python/JSON 接口、完整频点规划、DC/谐波/互调 A/B 与混合网络反馈噪声。

[共同限幅高阶工作点](highorder-linearization.md)新增既有高阶放大器的 C++ 链式导数、生成频点选择、传播失真与反馈噪声验证；专用跨语言网络入口仍待接入。
