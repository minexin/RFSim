# RFModel / RFSim

面向 SystemVue 2023 Linear Analysis、RF System Analysis 和 RF Design 器件能力的 C++17 射频仿真工程。当前处于原型开发期，**尚未达到完整兼容目标**。

## 已有实现

- Touchstone legacy S 数据、RI/MA/DB 转换、频率插值及表格器件接口。
- 同一正实数参考阻抗下的多端口连接、反射反馈、等效 S 提取和频率扫描；支持[各端口独立复参考阻抗](docs/power-wave-references.md)下的 S、内生噪声输出和物理 Z/Y 转换。
- 匹配衰减/延迟模型、功率与阻抗后处理、标量 Friis 级联噪声。
- S 数据 CSV 命令行导出与可安装 CMake 包。

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Windows 本机可运行 `scripts/build-msvc.ps1` 自动定位已有 VS 工具链。外部工程通过安装包中的 `RFModel::rfmodel` 接入，见 [安装说明](docs/installation.md)。

代码统一使用多行定义、4 空格缩进和显式控制流程花括号。提交前运行 `scripts/format-cpp.ps1 -Check`；批量整理可运行 `scripts/format-cpp.ps1`，详见 [代码风格](docs/code-style.md)。

## 验证与剩余工作

[三平台 CI](https://github.com/minexin/RFSim/actions/runs/35746795541) 已验证 Windows/MSVC、Ubuntu/GCC、macOS/AppleClang 的 Debug/Release，六组均通过 14 个核心/CLI 测试与 1 个安装消费者测试。证据绑定提交 `7fba1bf`，见 [验证报告](docs/build-validation.md)。

已运行 SystemVue 2023 衰减器的损耗、温度和源功率对照，噪声密度与极小衰减边界仍有差异，见 [实测报告](docs/systemvue-attenuator-power.md)。相关噪声协方差、双向线性链路和基础非线性/混频能力已进入核心；这些实现不代表完整 RF Design 库兼容。仍需完成复参考阻抗与 SystemVue 的数值对照、完整器件目录及更广泛的验证。

新增 [链路噪声接口](docs/linear-path-noise.md) 支持源/负载失配、器件内相关噪声和源噪声/器件噪声分解。

[多音放大器接口](docs/multitone-amplifier.md) 已提供总 RF 功率限幅以及直接、二阶、
三阶分谱结果，贯通 C++/C/Python/JSON；等功率双音的指定功率点已对照，完整
重叠谱合并、级联、相位和噪声语义仍待验证。

[长期路线](docs/roadmap.md) · [兼容矩阵](docs/systemvue-2023-matrix.md) · [线性分析输出契约](docs/linear-analysis-compatibility.md)

[同频多源非线性放大器](docs/coherent-amplifier.md) 已接入 C++/C/Python 与系统图；
21 组 SystemVue 记录的 206 个来源谱项通过比较，递归多级非线性仍待完成。

[失真来源级联](docs/coherent-amplifier-cascade.md) 新增 cascaded_amplifier：传播前级谐波/互调，计入共同驱动，并与本级同源失真相干合并。八组 SystemVue 对照中六组通过，两组 0 dB 增益差异保留；次级失真再混频仍未完成。

[RFAMP 单位增益诊断](docs/systemvue-unity-gain-diagnostic.md)：17 组受控采集支持等效功率增益 1.00000023 的行为解释；名义增益验收仍保留失败，诊断不算兼容性通过。

[十一阶相干多项式](docs/coherent-polynomial.md) 新增 C++/C/Python 接口，可由谐波、互调输入继续生成带本地来源的 RF 项；已做独立卷积验证，尚未完成 RFAMP 高阶系数、递归身份及子谱合并的兼容验收。

[递归来源与多项式系统图](docs/recursive-mixing-origins.md) 新增原始源因子展开、同源失真归并及显式全局来源阶数限制。混频多来源传播已在后续阶段接入；SystemVue 高阶子谱语义仍待完成。

[来源表达式之和](docs/origin-expressions.md) 新增实际复波贡献的求和、共轭和分配展开，C++/C/Python 均保留相消后的来源项；已用于系统图中的混频与多项式联合传播。

[混频与多项式的多来源系统图](docs/mixed-polynomial-graphs.md) 支持混频折叠后的复波贡献经线性网络继续非线性展开；独立数值对照通过，共同压缩已接入，SystemVue 全局阶数/高阶系数仍待验收。

[多来源公共压缩响应](docs/common-amplifier-response.md) 新增 C++/C/Python 工作点接口，按真实相干总功率共同压缩并保留各来源贡献，支持零驱动相消及既有失真级联。

[RFAMP 四阶与五阶诊断](docs/systemvue-highorder-diagnostic.md)：六组实测中，公开四阶规则的 23 个已记录来源项吻合，单点识别的五阶系数通过另外 27 项检查；双音 12 个标签缺项已由后续频谱削减开关对照定位，完整高阶兼容仍未验收。

[频谱削减配对诊断](docs/systemvue-spectrum-reduction.md)：三组双音功率配置关闭 UseSpecReduction 后均恢复完整的 44 个四、五阶来源项；开启时较弱重叠项缺失，共有的 92 项复波不变。五阶系数仍属固定参数识别，总谱与完整削减算法尚未验收。

[显式高阶截点转换](docs/polynomial-intercepts.md)新增二至十一阶 C++/C/Python 参数接口，指定双音参考项、截点参考面及实系数符号，可接入现有多项式系统图；SystemVue 四阶诊断已使用此接口，自动高阶 RFAMP 系数仍待完成。

[十一阶多项式与 ABI 兼容](docs/eleventh-order-polynomial.md)：C++/Python、显式 IP2..IP11 及系统图已扩展到十一阶；新增 C v2 符号，旧九阶结构与函数保留。证据为独立数值与工程回归，SystemVue 完整十一阶兼容尚未完成。

[RFAMP 四至十一阶实测诊断](docs/systemvue-eleventh-order-diagnostic.md)：七组采集验证公共限幅与固定参数奇数阶系数；807 个奇数阶校准外来源项通过，八/十阶小残差及 10 个低功率缺项保留，自动系数算法仍未实现。

[高阶输入端口参考诊断](docs/systemvue-highorder-input-reference.md)：从原采集归档九个源的实际输入电压，1413 个独立高阶评分项在原容差下通过；八/十阶名义残差由输入参考差异解释，完整网络输入求解和自动奇数阶系数仍未完成。

[显式高阶放大器](docs/coherent-highorder-amplifier.md) 将二至十一阶系数、共同基波压缩与输入限幅接入 C++/C/Python/JSON，保留相消来源历史并支持已有失真传播。统一原生入口重放通过 1413 个独立来源项；自动 RFAMP 系数、完整总谱及次级失真再混频仍待完成。

[IMN 输出互调功率参数](docs/intermod-levels.md) 接入 RFAMP_HO 文档规定的 IM1..IM11 功率到显式系数转换，贯通 C++/C/Python/JSON；系数符号由调用者明确给出，完整 RFAMP_HO 实测验收仍待完成。

新增[复参考二端口噪声后处理](docs/power-wave-noise.md)：C++/C/Python 支持 NF、NFmin、源端 GammaOpt、物理 Rn 及相关矩阵重建，SystemVue 对照仍待完成。

线性 JSON 的[noise_analysis 配置](docs/linear-noise-analysis.md)支持物理源阻抗扫描及逐频点 NF/NFmin/GammaOpt/Rn 输出，示例为 examples/linear-noise-analysis.json。

[理想无源器件参数化入口](docs/passive-model-interfaces.md)覆盖 R/L/C、匹配衰减与延迟、等功率/复分支隔离功分器和正交耦合器；可用于线性及相干网络 JSON，示例为 examples/passive-rc-network.json。

[理想无损 Butterworth 滤波器](docs/butterworth-filter.md) 已提供低通、高通、带通、带阻的复 S 参数和 C++/C/Python/JSON 接口；厂商 IL/Amax 等非理想参数与实测兼容仍待完成。

[理想无损 Chebyshev I 滤波器](docs/chebyshev-filter.md) 新增四类响应、独立纹波与边缘衰减、奇偶阶反射及 C++/C/Python/JSON 接口；SystemVue 非理想参数及实测仍待完成。

[固定泵频率转换与相关噪声](docs/frequency-conversion.md) 新增直接/共轭转换矩阵、跨频率反射反馈、DC 及 C/P 噪声统计量，贯通 C++/C/Python/JSON；厂商 Mixer 标定仍待完成。

[多器件转换网络](docs/conversion-network.md) 已支持线性器件与混频器按物理端口接线、反射往返、镜像噪声折叠及跨频率 C/P，提供完整 JSON/CLI 示例；频率自动扩展与非线性工作点联立仍待完成。

[变频网络噪声分析](docs/conversion-noise-analysis.md) 支持显式参考/热噪声频带、单边带和多频带归一化、复反射及等效输入噪声温度，保留原网络源条件；宽带、热负载和厂商 CNF 验收仍待完成。

[变频热负载噪声](docs/conversion-loaded-noise.md) 可输出入射/出射及交叉 C/P、端口净吸收噪声，支持显式终端温度，并通过热平衡和连接守恒验证。

[通道噪声与载噪比](docs/channel-noise-measurements.md) 支持指定带宽的 PSD 积分、DC 截边、所需谱线选择，以及转换网络的入射/出射通道测量；相噪与完整路径映射仍待完成。

已提供[小角度相噪源](docs/phase-noise.md)，可将相关上下边带接入固定泵转换网络；完整 LO/参考时钟相噪与厂商验收仍待完成。

[共享参考相噪与跨器件相关噪声](docs/shared-phase-noise.md)支持多载波相位增益和相关统计在转换网络中的传播。

[RF/LO 混频小信号模型](docs/mixer-linearization.md)支持给定工作点的 LO 扰动、共享相噪和阻塞波互易混频解析验证。
