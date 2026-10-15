# SystemVue 2023 兼容性矩阵

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

状态以实现和证据为准，不以版本计划代替验收。已完成部分 SystemVue 单器件与
链路采集，存在通过点和已知差异；下表不表示任一整类能力已全面兼容。

## 当前 C++ 能力摘要（2026-10-15）

权威机读索引为 [能力与证据映射](../validation/compatibility-evidence.json)，
[字段 schema](../validation/compatibility-evidence.schema.json) 与
[阅读/校验说明](compatibility-evidence.md) 分别约束结构和判定范围。
实现、独立数学测试、归档厂商通过/失败以及发布门禁分开记录。

| 能力 | 当前实现 | 主要剩余项 |
|---|---|---|
| Linear Analysis | C++ N 端口、F/S/ZPORT、独立复参考物理接线、S/Z/Y、W/Hz 相关噪声及二端口噪声参数 | 厂商 ZPORT/CS/噪声归一化与逐频对照；[接地 DC、解析 G/C 和冻结小信号已实现](dc-bias-small-signal.md)，厂商 DC/RC 未验收；MNA/理想电压源/电感/瞬态仍不支持 |
| 非线性工作点 | 直接/共轭 Jacobian、阻尼求解、仿射偏置、[混合复参考工作点](conversion-operating-point-power-wave.md)、[接受工作点本征噪声提供器](conversion-operating-noise.md)与冻结 C/P | 厂商反馈/噪声语义、实际器件噪声律与跨器件本征相关 |
| RF System Analysis | 类型化相干/转换图、来源传播、结构扩频、加载噪声、显式频带与相噪测量 | 路径/驱动功率/预算、完整调制谱、PLL/PNCP 与厂商测量约定；保留 RFPwrIn 失败 |
| 线性模型 | RLC、理想器件、表格 S/噪声、频率相关电缆及滤波器等原生模型 | 完整目录逐器件参数与行为验收；通用回调不是器件兼容通过 |
| 显式非线性模型 | 十一阶电压多项式、公共压缩/限幅、[高阶适配器](model-adapters.md)工作点/相干入口 | 自动 RFAMP 系数、合并总谱、次级失真、完整 PHD 文件及变泵浦工作点；原生固定切片不替代这些能力 |
| 原生 PHD 固定泵浦 | [显式单幅度 RMS 功率波 F/S/T 切片](phd-coefficient-contract.md)、相位冻结、严格 DC gauge、参考面、拥有数据的受限 Jacobian、[有界固定泵确定性网络](phd-fixed-pump-network.md)；独立数学测试 | 不支持泵浦扰动或完整泵浦导数；无通用网络/工作点适配器、厂商系数到模型转换/提取/插值、噪声推断或 SystemVue2023 数值兼容结论 |
| PHD/X-parameter 原始数据工具 | [受限 typed-text reader](xparameter-data.md)与[version-2 raw catalog/精确选择](xparameter-catalog.md)已实现 | 原始安装文件未全量云端解析；不提供系数归一化、模型转换、缺失值填充、插值或厂商数值验收；raw行目录不是243项器件覆盖 |
| Mixer | 固定泵转换与 C/P；[三端口双线性适配器](model-adapters.md)以实际求解 LO 驱动且保持固定归一化 | 厂商压缩、泄漏/杂散、噪声、自动选频/削减；三端口适配器不虚构上述能力 |
| 噪声数值稳定性 | PSD 因子域传播、补偿及缩放算术，见[阶段证据](../validation/noise-factor-propagation-validation-20261014.json) | 输入截断、可表示性和条件数限制仍在；不新增厂商通过项 |
| AM/PM | 显式校准 PointDerivative 相位律与导数组合；有限无告警归档通过 | 实际输出功率须由调用者给定；完整网络 AM-AM、多音、DataPoints 与连续旁路律未验收 |
| 显式节点噪声 | 冻结参考加载网络传播调用方给定完整节点电流协方差 | 不从 G 推断噪声、温度或端接噪声；无厂商验收 |
| 原生表格 AM/AM、AM/PM | [显式斜率 Hermite 表格、解析导数、model/coherent adapter](tabulated-amplifier.md) | 无外推或自动噪声；不等于 SystemVue DataPoints 验收 |
| 参数任务 | C++ 类型化有界优化、参数绑定、相关 Monte Carlo、统计及重放 | 厂商约定对照；已退役 JSON 字段分发不是当前产品要求 |
| 交付与 GUI | header-only C++17、可选 C ABI、原生 S CSV CLI；[内部工程 SDK 归档](cpp-sdk.md)含实际 Git 身份、哈希及独立消费者；GUI 后置 | Python 产品入口已退役；归档不是公开发布，许可与分发决策未完成；不新增厂商兼容通过项 |

[线性输出细表](linear-analysis-compatibility.md)保留官方帮助来源。
[当前提交绑定验证](compatibility-evidence.md)区分云端阶段与最终集成门禁；
历史计数见[构建记录](build-validation.md)。[243 项目录](rf-design-catalog.md)
只是库存基线，不是已实现或兼容数量。

2026-10-15 更新：[RFAMP A/B/C/D 与 380/380 独立 C 来源 holdout](rfamp-op1db-pilot-results-20261015.md)已完成，D 只作恢复控制，仍无自动专有系数律。
[DC/RC 八次分析](dc-bias-systemvue-followup-20261015.md)已有实际执行但零项比较验收，原失败不改判。
[PHD 安装格式](systemvue-xparameter-format-evidence-20261015.md)已本地检查，原始文件未全量云端解析，尚无系数/参考面/数值兼容验收。本次摘要刷新自身不执行 SystemVue。

## 保留的分阶段证据与历史接口

下列记录保留原阶段范围、通过点和失败点。其中 C/Python/JSON、wheel 等字样
描述当时入口，不表示当前仍交付这些已退役产品接口；模型后续实现状态以本页
当前摘要与机读映射为准。以下各历史记录的执行范围保持当时原貌；本次摘要刷新不执行 SystemVue，也不更改原容差或失败判定。

基波压缩/饱和的七点参考、原生接口与边界见 [饱和基波模型](saturating-fundamental.md)。
谐波小信号及压缩区差异见 [截点模型](intercept-amplifier.md)，不能以基波吻合替代
完整非线性频谱兼容。

[双音参考](systemvue-two-tone.md) 已覆盖等功率 1.0/1.1 GHz 的三组输入：小信号
互调通过，高功率的未限幅差异保留。[原生分阶接口](multitone-amplifier.md) 已实现
总 RF 功率限幅并通过指定 36 项功率比对；重叠产物传播/合并语义尚待完善，
不能据此宣称完整多音兼容。

本级互调来源现可在 C++/C/Python/JSON 中逐项展开：三组等功率双音共 48 项
功率验证通过，包括 12 个载频处独立三阶项。跨级身份传播、相位及合并测量
规则仍待实现/验证，详见 [来源契约](multitone-amplifier.md)。

本级来源项已可按实际频率经过线性后级并保留路径快照，见 [项传播接口](term-propagation.md)。
现有验证为解析/接口回归；多级非线性反馈、分支合路身份和厂商链路对照尚未完成。

[不等功率双音](systemvue-unequal-two-tone.md) 已新增四组输入共 64 个来源功率点，
全部通过原 1e-7 阈值，覆盖强弱互换与 20 dB 不平衡。模型参数未调整；独立
相位、频距、更多器件参数组和多级非线性仍待交叉验证。

独立 limiter 参数组（G=10、OP1dB=15、OPSAT=18 dBm）的三组双音新增 48 项
功率通过。按已核验参数构造模型及按 profile 判定总输入上限已纳入回归，见
[交叉验证记录](systemvue-unequal-two-tone.md)。原生模型未因本组观测而调整。

[复幅度与相干规则](systemvue-amplifier-phase.md) 已按实测 V2/Z2 验证 64 个
独立项，最大复幅度相对误差 3.63e-8。完整相干组、分支合路和多级非线性仍待
实现/验证；同频率不是充分的相干条件。

[显式相干合并](coherence.md) 已贯通 C++/C/Python/JSON：同组复幅度求和、
异组功率求和，保留抵消零组。基于参考软件相干编号的 8 个载频节点总功率
通过对照；自动时钟/跨级身份生成、分支合路与多级非线性仍待实现。

[相干混频](coherent-mixer.md) 已将 RF/LO 来源分组接入理想变频和合路；镜像抑制为解析回归，SystemVue Mixer 实测及完整器件兼容仍未完成。

[相干系统前馈图](coherent-system-file.md) 已贯通多输出线性子网络、混频和后级合路；当前采用匹配阶段边界，共同基波压缩节点已接入，跨阶段反馈、递归多级非线性、噪声图及 SystemVue 路径预算仍未完成。


[双源相位/时钟扫描](systemvue-coherent-network.md) 新增六组实测，24 个路径复幅度
和六组来源关系通过；随后六组方向总谱的 12 项功率比较也通过，包含反相相消。
RFPwrIn 比较五组失败，清缓存及启用总谱后仍有差异。相关报告保留
passed=false，完整相干合并功率不能列为已验收。

[相干载波共同压缩](coherent-compression.md) 已贯通 C++/C/Python 和系统图，
通过共享 P1dB/饱和预算及压缩后镜像抑制的解析回归。后续
[多源放大器实测](systemvue-shared-compression.md) 的 64 个直接基波复幅度通过；
四项小信号 RFPwrIn 差额由噪声开关对照定位，但报告保留比较失败，
完全相消两组为诊断。饱和共同驱动与完整输出频谱仍未验收。

[同频多源非线性放大器](coherent-amplifier.md) 已贯通 C++/C/Python 和系统图，
同频独立源保留 15 个 RF 项，锁相合并后保留 4 个；21 组实测的全部 206 个
确定性来源项复幅度、功率及相干分组通过。并行放大支路合路为解析验证，
递归非线性及总谱/噪声驱动/路径预算仍未完成。

[失真来源级联](coherent-amplifier-cascade.md) 新增 cascaded_amplifier：传播前级谐波/互调，计入共同驱动，并与本级同源失真相干合并。八组 SystemVue 对照中六组通过，两组 0 dB 增益差异保留；次级失真再混频仍未完成。

[RFAMP 单位增益诊断](systemvue-unity-gain-diagnostic.md)：17 组受控采集支持等效功率增益 1.00000023 的行为解释；名义增益验收仍保留失败，诊断不算兼容性通过。

[十一阶相干多项式](coherent-polynomial.md) 新增 C++/C/Python 接口，可由谐波、互调输入继续生成带本地来源的 RF 项；已做独立卷积验证，尚未完成 RFAMP 高阶系数、递归身份及子谱合并的兼容验收。

[递归来源与多项式系统图](recursive-mixing-origins.md) 新增原始源因子展开、同源失真归并及显式全局来源阶数限制。带混频器的来源表达式之和已实现；SystemVue 高阶子谱语义仍待完成。

[混合多项式系统图](mixed-polynomial-graphs.md) 已支持线性网络、理想混频与多项式递归链路，按流保存不同来源贡献并保持物理相干归并；证据为独立数值卷积与工程回归。含共同压缩的混合图已由[公共工作点](common-amplifier-response.md)实现并做数值回归；SystemVue 的高阶系数、LO 阶数、子谱合并尚未验收。

[RFAMP 四阶与五阶诊断](systemvue-highorder-diagnostic.md)：六组实测中，公开四阶规则的 23 个已记录来源项吻合，单点识别的五阶系数通过另外 27 项检查；双音 12 个标签缺项已由后续频谱削减开关对照定位，完整高阶兼容仍未验收。

[频谱削减配对诊断](systemvue-spectrum-reduction.md)：三组双音功率配置关闭 UseSpecReduction 后均恢复完整的 44 个四、五阶来源项；开启时较弱重叠项缺失，共有的 92 项复波不变。五阶系数仍属固定参数识别，总谱与完整削减算法尚未验收。

[显式高阶截点转换](polynomial-intercepts.md)新增二至十一阶 C++/C/Python 参数接口，指定双音参考项、截点参考面及实系数符号，可接入现有多项式系统图；SystemVue 四阶诊断已使用此接口，自动高阶 RFAMP 系数仍待完成。

[十一阶多项式与 ABI 兼容](eleventh-order-polynomial.md)：C++/Python、显式 IP2..IP11 及系统图已扩展到十一阶；新增 C v2 符号，旧九阶结构与函数保留。证据为独立数值与工程回归，SystemVue 完整十一阶兼容尚未完成。

[RFAMP 四至十一阶实测诊断](systemvue-eleventh-order-diagnostic.md)：七组采集验证公共限幅与固定参数奇数阶系数；807 个奇数阶校准外来源项通过，八/十阶小残差及 10 个低功率缺项保留，自动系数算法仍未实现。

[高阶输入端口参考诊断](systemvue-highorder-input-reference.md)：从原采集归档九个源的实际输入电压，1413 个独立高阶评分项在原容差下通过；八/十阶名义残差由输入参考差异解释，完整网络输入求解和自动奇数阶系数仍未完成。

[显式高阶放大器](coherent-highorder-amplifier.md) 已统一二至十一阶系数、共同压缩与输入限幅，贯通 C++/C/Python/JSON。新原生入口用实测输入电压重放既有归档的 1413 个独立来源项通过；仅为显式系数模型和来源项证据，自动高阶 RFAMP 参数、直接/合并总谱、次级失真再混频仍未验收。

[IMN 参数转换](intermod-levels.md) 已按 RFAMP_HO 官方公式和互调表支持 IM1..IM11 输出功率输入，系数符号显式指定；证据为独立数值与接口回归，尚未完成 RFAMP_HO 厂商实测、12..20 阶或完整器件兼容。

[绝对波仿射网络](affine-conversion.md)补齐给定混频工作点的名义波装配与网络一致性检查；自动工作点已提供[通用 C++ 核心](conversion-operating-point.md)，固定双线性及公共基波压缩模型已接入 C/Python/JSON，其他器件适配与厂商实测仍待完成。

[高阶电压多项式工作点](polynomial-linearization.md)提供 C++/C/Python/JSON 完整频点推导、DC/谐波/互调 Jacobian 和混合网络反馈噪声，证据为数学模型回归；RFAMP_HO 的厂商系数与限幅语义仍不由这些测试证明。

[共同限幅高阶 C++/C/Python/JSON 适配](highorder-linearization.md)已连接既有压缩/限幅/显式高阶公式与通用工作点求解器，包含传播失真对公共驱动的导数。该接口采用每频点已合成复波，尚未接入同频来源分解或厂商反馈验收。

[旧式峰值导数 AM-PM](am-pm.md)新增 C++ 相位律与输出通道组合，数学回归不等于厂商实测；AM-PM 整体仍为未验收。

[SystemVue 2023 显式 AM/PM 适配器](systemvue-am-pm-adapter.md)将冻结经验映射加入 C++ SDK：原生重放 549 个无告警登记点通过；37 个 10 GHz 告警点及 4 个额外端点完整单列。新增证据包含 915 MHz/2.45 GHz 压缩配置与 30 MHz 零相位对照。它只验给定实测输出功率的相位映射，不构成完整 AM-AM 网络、多音、DataPoints 或频率响应兼容验收。

[DataPoints 零 dBm / 首节点判别](systemvue-am-pm-datapoints-zero-study.md)完成七次分析、387 个登记扫描点和七个普通点，另保留七个额外终点。有限对照支持异常跟随名义零 dBm，否定跟随首节点；名义零旁路候选仅匹配扫描 DCP 的 383/387 点，四个失败均位于功率报表平台。普通原始功率七点通过不代表扫描或任意表格契约成立，产品公式未变，DataPoints 仍未验收。

[普通原始功率轴后续判别](systemvue-am-pm-datapoints-raw-axis-study.md)新增一个完整复现对照及十个普通预测检验，均取得原始 V/Z/P。原始连续与名义零旁路候选均为 9/10；非零目标 −0.000001 dBm 的零增量直接否定仅名义零旁路。+0.000001 dBm 则为非零增量，尚不能从有限点确定内部边界或对称性。671 个扫描观测属于重复控制，不能当作独立新增验证点；所有四个候选仍失败，产品公式与兼容状态未变。

[近零异常顺序复现](systemvue-am-pm-datapoints-order-study.md)的十次分析完成六个历史复现、两个新增预测目标与两个顺序复现，五个跨块目标均匹配。−0.000001 dBm 零增量在两个指定顺序下复现；新增 ±0.00001 dBm 为非零增量，原始功率样条通过 2/2，而 DCP 功率连续样条为 0/2。610 个扫描观测仍为控制；局部通过不消除旧候选反例，也不证明精确旁路边界或完整兼容。

[增益与源功率分配实验](systemvue-am-pm-datapoints-allocation-study.md)的十二次分析完成八个新配置及四个历史控制。冻结输出离散模式通过 8/8，输入离散模式为 2/8；六个分歧配置分别为 6/6 与 0/6。等名义输出的零/非零指纹保持 6/6，固定名义输入的零指纹为 0/2。732 个扫描点仍为辅助控制；原始功率近似相等不代表逐位相等，有限成功不证明未知函数、连续旁路区间或导数契约，产品公式与兼容状态未变。

[转换网络结构频率规划](conversion-frequency-plan.md)已在C++17中实现同频传播、双线性混频、十一阶多项式与物理接线的联合闭合，含扩张反馈失败和完整网络数值回归。后续C/Python/JSON已接入相同原生规划器，含种子矩阵映射和显式新增边界；厂商选频、频谱削减仍待完成。

C++ 结构规划新增 `FrequencyHighOrderRule`：全部 RF 输入直接传播，仅显式 generating_bins 生成二至十一阶产物，并与物理反馈联合闭合。8 通道反馈复波及完整输出 C/P 通过手工网格对照，独立标量根与时域 Fourier 投影核验信号；内部 DC 资源上限也被检查。高阶自动规划现已接入 C/Python/JSON，包含种子 C/P、初值、固定偏置及 AM-PM 选择映射；未新增厂商选频或频谱削减验收。

[A10 安装验证](installed-python-validation.md)新增隔离 wheel 导入和已安装原生库加载检查，覆盖全部 Python 绑定套件并接入六配置 CI；本地 MSVC 与 WSL/GCC 结果不能替代当前 macOS、远程执行或厂商兼容验收。
