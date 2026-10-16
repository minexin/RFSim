# 当前 C++ 能力与证据

[机读映射](../validation/compatibility-evidence.json)是当前兼容摘要的权威索引，
本轮工程源码基线为 `6ef3b0c0c840d2434b4b59e959b6c93eb4beb211`，不是全目录验收清单。
源码树为 `a305e404e004330dab4f69478aee17da4bad976a`。在既有原生能力上加入独立弱噪声块保留、
历史单/双音、共享来源及两级 source-cubic 归档重放，以及同一行为模型/接线表的系统 SDK 示例。
源基线精确 CI 38835703380 已从完整原始 REST run/jobs 核验全部 18 作业成功。
原示例提交 `350bc9f` 的 CI 38834090412 失败，严格 SDK 目标清单修复与原失败记录分列。
前次冻结基线 `5c5812e` 的 CI 38815413675 仍只证明该历史提交的 18 作业成功。
各阶段 CI 只属于所列精确源码提交；后续文档/证据校验覆盖层仍须单独核验，不能继承源码成功。
本覆盖层仅更新 docs/validation；现有严格 checker/schema 不变，不改数值实现、原阈值、验收条件或 SDK 示例。
先前 `3160ba` 覆盖层的工具与格式改动属于历史记录，不是本轮改动。
它分列实现范围、独立数值证据、厂商归档状态、剩余问题及发布门禁。
`implemented` 只表示所述 C++ 契约存在；`bounded_pass` 只表示指定厂商子集通过。
没有总兼容率或整体通过标志。243 项目录只作库存，不计作实现数量。

## 已实现的有界能力

- 基础 API：器件端口及噪声 provider 形状契约、有限插值结果与线性求解失败边界。
  冻结阶段严格 C++17 Debug/Release 各 157/157，重定位完整 SDK 各 15/15、纯 SDK 各 13/13。
- 确定性变频波：独立 `ConversionWaveResult` 与单 RHS 求解；Newton 候选不传播噪声，
  初始验证、接受工作点及最终噪声 provider 契约保持。`solve_waves` 不验证噪声，
  `analyze(false)` 仍计算出射 C/P。阶段各 159/159 完整、112/112 纯 SDK，
  完整/纯 SDK consumer 各 15/15、13/13。N=256 非线性端到端五次中位数约
  18.075→11.804 秒，仅为固定案例共享主机证据。旧静态成本包含噪声；不称纯 solver
  42 倍提速或渐近复杂度改善，稠密分解仍为三次复杂度。
- 原生 PHD：显式 F/S/T、单振幅、固定泵相位与实正参考的有界系数切片已实现，
  并有泵钳位扰动 Jacobian。2362 项专项检查及严格 C++17 阶段证据见
  [系数契约](phd-coefficient-contract.md)。这不是厂商系数到模型转换、系数提取、
  插值/外推、参考变换或完整泵 Newton 导数；不从 F/S/T 推断本征噪声。
  [固定泵网络](phd-fixed-pump-network.md)扩展为受约束确定性匹配/谐波反馈，
  保持泵源精确固定、禁止泵端接线，并要求逐非泵扰动上限；不证明任意负载的物理精度。
- SDK：内部工程归档、来源及哈希校验、提取与 installed-only 双 TU 示例已实现。
  示例实际扫描匹配 6 dB 衰减器和无损 50 Ω、1 ns 传输线并核对波与送达功率。
  历史 `3160ba` 中 `main.cpp` 仅补 clang-format 19.1.7 空白格式，`network.cpp` 不变。
  内部 ZIP 不代表正式发布；许可、版本、标签及 API 稳定性仍需用户决策。
- Touchstone：已有有界导入及新增 C++17 单端 S 流导出（2.0 / Hz / RI / Full，
  1–1024 端口、64 MiB payload 上限）。原 S-only 阶段拒绝非空噪声；后续 raw 两端口噪声
  序列化已集成，见下文。不采样模型、重归一化或导出 PHD。
  本地阶段完整各 161/161、纯 SDK 各 122/122；但原 writer 跨平台 CI 失败，不能据此称全平台通过。
  后续 decimal 可移植性修复 `a987c3d` 的独立运行已 18/18 成功，含 macOS Debug/Release；
  原 writer 失败保留，修复后成功不覆盖尚待冻结的最终覆盖层。
  调用方负责文件、同步和 flush/close；部分写入失败可能留下前缀，无原子性或持久性保证。

以上本地计数只属于各自冻结阶段，不是最终覆盖层重跑结果。精简可移植摘要见
[工程阶段记录](../validation/engineering-evidence-summary-20261014.json)，完整日志保留在外部审计归档。

均匀线共振修复采用独立 100 位 ABCD 固定 oracle；本地重绑定 Debug/Release 各
161/161 完整、149/149 纯原生（均启用开发参考测试）、16/16 完整安装消费者和
14/14 纯安装消费者，总计 680/680。该阶段局部候选与已集成提交源树一致；
精确源提交跨平台 CI 已 18/18 成功。一般频率延迟乘积的精度与溢出限制保留，
该历史均匀线阶段未覆盖 RLGC；随后 RLGC 有界修复及验证见下文。

## 最新原生阶段与集成入口

当前可直接使用纯 C++17 函数/类型 API；[SDK 接入](cpp-sdk.md)、
[确定性变频波](conversion-wave-solve.md)、[线性外部 S 提取](linear-external-s-factorization.md)、
[均匀线](transmission-line.md)、[RLGC 线](rlgc-transmission-line.md)、
[PHD 系数切片](phd-coefficient-contract.md)、[固定泵 PHD 网络](phd-fixed-pump-network.md)及
[Touchstone 导出](touchstone-writer.md)给出具体契约。开发用 Python 采集、oracle 和证据工具保留；
这不恢复已退役的 Python 产品接口。

- RLGC 缩放算术：避免归一化和散射转换中的可避免中间溢出，最终 a/b/x 不可表示仍拒绝。
  重绑定严格 C++17 Debug/Release 各 161 完整、149 纯原生（含开发参考）、16 完整安装和
  14 纯安装消费者，合计 680/680。独立 400 位审查 1485/1554；69 个巨大低损耗相位案例
  明确排除，不计通过。极大电长度仍受 binary64 相位限制。常数 RLGC 不代表频变材料律、
  几何提取、多导体耦合或厂商传输线验收。
- 线性外部 S：每频率一次分解、只求所选 RHS，稠密成本 O(n³ + mn²)；原 solve 和阈值不变。
  各配置 162 完整、150 纯原生、16/14 安装消费者，合计 684/684。独立物理 V/I oracle 与
  旧提取路径分别对照。七次交替暖机采样：96/32 端口中位数 36.917249→3.446269 ms，
  160/2 端口 9.699459→4.920278 ms；均未观察到矩阵差异。这是单机定例，不是普遍速度承诺。
  没有跨频缓存、稀疏求解或噪声重构。ASan/UBSan 使用 detect_leaks=0；ptrace 下 LSan 不可用，
  不声称泄漏检查通过。
- 固定泵 PHD 网络：各配置 163 完整、151 纯原生、17/15 安装消费者，合计 692/692；
  112 项严格/UBSan 专项检查，独立实 2×2、双反馈谐波、V/I 与立方 Fourier 小扰动 oracle。
  另有外部 18 项补充探针覆盖环境共轭、DC 接线/偏置等，严格/UBSan 和四个安装前缀通过，
  不并入冻结回归计数。泵入射精确固定、反射为零、泵物理端口禁接线；逐非泵扰动上限由调用方
  声明，不能当作物理精度证明。仅一片固定振幅，不支持任意负载非线性精度、变泵 Newton、
  厂商系数导入或噪声推断；没有噪声量不表示器件无噪声。

以上为各历史原生阶段的本地记录；精确集成提交的跨平台 CI 另列。后续已完成的厂商采集见下节，本轮文档刷新不运行 SystemVue。

## 2026-10-15 已集成能力与厂商证据

- [接地 DC 与冻结小信号](dc-bias-small-signal.md)：显式 I(V)/Q(V) 及解析导数、
  接受点 G/C 数值快照、逐频 Schur 约化和复功率波转换已实现。有限源专项属于
  `dddc059` 的 18/18 CI。该接地非线性 API 不支持理想电压源或电感；独立线性 MNA
  已支持 R/C/L 与独立电压/电流源，见下节。仍无非线性 MNA、DAE、瞬态或自动厂商器件律。
- 显式节点电流噪声：调用方提供完整 A²/Hz 协方差，冻结加载系统传播到出射 W/Hz；
  不从 G 推断噪声、温度或端接噪声，不重调 DC callback。原 CI 14 成功/4 失败保留；
  预抵消导纳尺度修复后 `a0218741` 的 CI 18/18 成功。
- [原生表格放大器](tabulated-amplifier.md)：拥有 AM/AM、未绕回 AM/PM 与显式斜率，
  cubic Hermite、解析直接/共轭导数及 model/coherent adapter 已实现；`71999c42` CI 18/18。
  无外推、自动噪声、新谐波生成或 DC 导电律；不是 SystemVue DataPoints 验收。
- [RFAMP A/B/C/D](rfamp-op1db-pilot-results-20261015.md)已完成四次无告警采集。
  B 系数和预测器在 C 前冻结，C 的 380/380 独立来源通过原 `1e-7` 复波阈值。
  760 条边缘观测不是 760 个独立预测；A/B 为辨识，D 仅恢复对照。
  24 个原始文件与恢复证据由 completion 记录绑定；CI 只重放归档，不运行 SystemVue。
  单 profile 成功不识别专有 RFAMP 参数律，也不覆盖原级联等失败。
- [DC/RC 厂商后续](dc-bias-systemvue-followup-20261015.md)：实际提交八次分析，
  零项 DC/小信号比较获得验收。控制及部分 RC 分析曾执行成功，不能写成零执行或全部运行失败。
  模型创建失败和原 RC 数值、频率、边界失败完整保留，未放宽容差。
  [147 文件原始 ZIP](../validation/dc-systemvue-followup-raw-20261015.zip)及
  [逐文件 manifest](../validation/dc-systemvue-followup-manifest-20261015.json)保留；十份厂商文档仅本地保存。
- [PHD 安装格式证据](systemvue-xparameter-format-evidence-20261015.md)：已本地检查四个样例
  （两个 legacy 文件内容相同）及安装帮助，记录格式、轴、哈希与所选系数。
  原始厂商文件尚未在云端全量解析；格式检查不等于系数转换、参考面或数值兼容验收。

## 原生 PLL、相位噪声与有限带宽 jitter

- [locked PI PLL](locked-pi-pll.md)：已锁定、连续时间、理想 PI、零环路延迟的有界小信号模型。
  五个复传递函数与五项独立一侧相位噪声预算，单位显式；另有单输出波边带桥接。
  不模拟锁定/捕获、PNCP、fractional-N 杂散或任意环路滤波器，不推断器件噪声律。
- [共享复相位响应](complex-phase-response.md)：调用方给定共同参考过程、一侧 rad²/Hz 与复 H，
  显式生成完整 C/P 和跨输出相关，并可转换到局部复参考。重叠边带先合并潜变量系数；
  不以 |H| 或单输出总 PSD 代替复互谱，也不由名称推断同钟。它不是 PLL 或稳定性证明。
- [有限频带 jitter](phase-jitter.md)：显式载频、完整覆盖的正频偏区间和 Hz 轴上的线性 PSD
  插值，返回相位方差、相位 RMS、时间 RMS。不外推、不默认全带，不自动从波噪声推断相位谱；
  不提供 period/cycle-to-cycle/peak-to-peak/differential jitter 或厂商验收。
- [PLL 相噪/jitter](../examples/pll-phase-jitter-sdk/README.md)和
  [冻结偏置 adapter](../examples/frozen-adapter-sdk/README.md)为两个 installed-only 双 TU 示例，
  以独立 oracle 检查现有接口组合。示例不增加厂商通过项，也不关闭 release/license/stable-ABI 门禁。

这些阶段已集成到 main 并通过各自精确提交的 18 作业 CI，完整绑定见下文；数学与工程通过不等于厂商 PLL/PNCP 兼容。

## 独立线性 MNA 与复参考 S

[有界线性 MNA](bounded-linear-mna.md)拥有 R/C/L、独立电压/电流源和数值结果，
支持实 DC 与正频率复 RMS AC。DC 电容精确开路、电感精确电压约束并保留支路电流。
它与接地非线性 DC/冻结小信号 API 分开，不是 SPICE、非线性 MNA、瞬态/DAE 或自动噪声模型。

[MNA 功率波 S](mna-power-wave-s.md)对去激励后的参考加载系统单次分解、逐列独立验收，
支持逐端口复参考、理想短路及物理接线。要求整个加载状态唯一；即使端口 S 可能唯一，
浮动差分 common-mode gauge、冗余理想支路电流、隐藏共振与孤立节点也拒绝。
没有 gauge 选择、gmin、伪逆、状态删除或前向精度保证；保留 256 未知数/4096 元件预算、
正规数或精确零的保守算术域及原物理/后向残差门禁。无 DC-S、仿射输出偏置、扫频或噪声 API。
原生独立电路/物理恒等式、失败边界和安装消费者证据不构成 SystemVue DC/RC/S 验收。

[等温 MNA→ModelAdapter→conversion 示例](../examples/mna-noise-sdk/README.md)将 source-free
R/L 电路、复参考 S 和调用方明确的均温被动热噪声组合，检查完整 off-diagonal C、物理负载、
级联与六通道 stationary snapshot。独立物理 RL/Johnson 源 oracle 负责验证；这不是 MNA
自动噪声推断，去激励或数值无源检查也不证明真实温度、热平衡或器件噪声律。
无新 solver、混频本征噪声、非均温/主动器件模型或厂商采集。已有协方差准入/截断与浮点
条件限制仍适用，不承诺任意动态范围或任意参考下的数值完全等价。

## Touchstone raw 两端口噪声导出

[writer 扩展](touchstone-writer.md)现可导出现有 raw noise samples：独立有序噪声网格、
Fmin dB、GammaOpt 幅角及物理 Rn Ω；空噪声的 S-only 输出保持逐字节不变，reader 未改。
不拟合、钳位、构造协方差、校验模型级物理可实现性或自动重归一化。GammaOpt 仍引用 option-line R。
Cartesian→MA 必有舍入，不承诺复数逐位往返、原角度/圈数/十进制拼写，或各分量相对精度；
轴附近小分量可显著失真，显式零角度下溢和保守范围拒绝仍保留。64 MiB 预算与部分写入失败边界不变。
精确 f603bcf CI 38813359905 的 18 作业成功仅是此格式阶段证据，不改写原 writer 失败或建立 SystemVue 导入验收。

## 有界 raw reader、raw catalog 与只读重放工具

[原生文本 reader](xparameter-data.md)已实现 legacy/version-2 的受限 typed-data 读取，
保留词法 token、来源和拥有数据，限制输入及表示规模；不是厂商系数到模型的 importer。
[version-2 raw catalog](xparameter-catalog.md)拥有行与稀疏端口元数据，提供 AN/FB 有限索引语法及
精确 typed 条件选择；歧义拒绝，不做容差/最近点选择、默认值填充或插值。
S/T 等其他后缀家族不解析，不推断物理单位、泵、参考面或 native model。
它是原始行目录，不是 243 项 RF Design 器件库存的实现统计。

重绑定本地 reader 门禁 678/678，catalog 686/686；catalog 每配置为 174 完整测试、19 完整安装消费者、
133 纯测试和17纯安装消费者，均含 Debug/Release。来源树及外部完整门禁 manifest 哈希见
[集成工程摘要](../validation/xparameter-integration-evidence-20261015.json)。原 focused validation 中
当时 pending/unrun 的文字保留；这些后续门禁不改变原历史，也不是本轮覆盖层的数值重跑。
reader 与 catalog 的精确源码 CI 均已核验 18/18 成功；覆盖层自身 CI 仍须独立核验。原始安装文件仍未在云端全量解析，
无新增 SystemVue 数值兼容结论。

[installed-SDK 重放工具](installed-xparameter-replay.md)已实现：只读四个固定厂商文件、
由 C++ reader 全量解析，以旧归档哈希及登记的格式/行列/样本字段严格评分，保留失败。
云端门禁使用 synthetic fixtures；实际四文件本地重放尚未运行。四个文件有三个不同内容哈希，
即使未来选定文件格式重放通过，也不独立验证所有系数值、模型数值、归一化或参考面。
工具不运行 SystemVue/COM，不上传完整厂商文件；Python 仅为开发编排与评分。

## 2026-10-16 行为级系统工作流与追加证据

主线是 RF Design 行为器件建模、共享器件与系统组网、Linear 外部 S，以及 RF System
节点/端口谱和路径性能。这是主要框架，不能替代完整 SystemVue 2023 规格。
底层算法服务明确的行为与测量；算法存在或测试通过不自动增加厂商覆盖，目录检索未命中也不证明能力不存在。

- [独立弱噪声块](noise.md)：保留可表示的独立弱块，同时保持旧因子结果与既有容差。
  本地 full/pure Debug/Release 及安装消费者合计 756/756；严格 focused 与负控另列。
  连通块内弱模式、次正规数、舍入底噪及上层矩阵重构限制保留，不推断物理噪声律或厂商验收。
- [单/双音原生归档重放](amplifier-archive-native-replay.md)：六个历史 case 的 42 项判定为
  22 通过、20 失败；原名义输入、scorer 和 1e-7 阈值不变。本地 affected native 各 8/8，
  工具通过表示正确报告差异，不是 RFAMP 全过，也不是新增采集。
- [共享来源重放](source-archive-native-replay.md)：同批 23 capture 中 21 正常 case；
  A 的 53 项为 49 通过/4 失败，B 的 206 项和 21 分组关系通过；两个相消 case 仅为排除诊断。
  64 次 direct 边界比较不另加为独立覆盖。严格 native 各 36/36，安装证据另列。
  B 的通过不覆盖 A 的 RFPwrIn 失败；原始 raw/workspace/library 字节声明仍有未核验部分。
- [两级 source-cubic 重放](cascade-archive-native-replay.md)：8 份旧 capture、230 项诊断判定为
  208 通过/22 失败，含 10 个仅功率阈值失败；原逐项状态保持，数值差异单列。
  缺少 spectrum-reduction 回读，全部为 diagnostic_only；43 条额外实测 port3 来源未评分且方向未知。
  不补零、不升级为完整端口谱、NodeTotal 或厂商物理验收。严格 native 各 39/39 与安装消费者
  是 affected gate，不是 full four-gate 数值矩阵。既有失真仅线性传导，不证明任意二次混频。
- [行为级系统 installed SDK 示例](../examples/behavioral-system-sdk/README.md)：同一不可变命名模型和
  单一物理接线表用于外部 S 扫频与加载后的内部端口测量；失配链和带旁支加载的 tee 都有独立解析 oracle。
  strict focused Debug/Release 各 1/1，重定位 full/pure SDK Debug/Release 四格各 1/1，
  双 TU、NDEBUG 独立审查和错误解析 S 负控分别保留。仅是现有 API 的工程组合，没有新求解器或器件算法，
  不是 full numerical gate 或 SystemVue 新实测。
- 严格 SDK 目标清单修复：原 `350bc9f` focused 选择未包含 sdk_configuration_test，新增示例使
  CI 检出 8 个目标而旧清单要求 7 个。`6ef3b0c` 仅同步两行目标名/数量，保持严格名称、
  最小配置零目标和 C/Python 隔离检查。本地 Debug/Release 各 3/3 affected tests，四格安装消费者
  各 1/1；额外目标和等数量错误名称两个负控均拒绝。
  旧运行 12 个失败 job 的完整解码日志均只列该 guard 失败，example/installed 两项合计 24/24 通过；
  这不改变原 job/run 失败结论，解码日志 hash 与原始 REST 快照字节 hash 分列。

示例端口参考阻抗不是测得节点阻抗；实际加载下 V/I 不是增量 looking-in 阻抗或已验证的 ZPORT。
V、I 均为 RMS 相量，电流朝器件内，净功率为 Re(V·conj(I))，不声称等同于 SystemVue CP/CNP/CV。
链路 helper 限定有序二端口链；分支 selected-cut 比值说明源、两侧负载、频率与方向，并保留旁支加载。
完整 2023 官方帮助正文在本轮核验中尚未可读；已有带哈希的
[历史通道测量帮助摘要](../validation/systemvue-2023-channel-noise-help-provenance.json)、
[谱约简帮助来源](../validation/systemvue-2023-spectrum-reduction-help-provenance.json)与新的运行实验必须分列，
不以摘要冒充完整规格，也不由访问限制推断厂商功能不存在。

[阶段工程摘要](../validation/engineering-stage-evidence-20261016.json)分列本地候选、正式提交、同树绑定、
逐源文件哈希及外部审计原件身份。外部原件未嵌入公开仓库，摘要哈希不是公开下载或独立可复核原件的承诺。
原始执行时间、发布绑定时间与 CI 采集时间各自保留；本摘要时间不替代它们。
本轮未执行 SystemVue，不新增厂商样本；三个重放路线也不恢复 Python 产品接口。

## 精确提交与运行

以下成功记录均为各自精确提交 18/18 作业，不覆盖后续提交：

- 高阶适配器 `1b1fb8511286285b6f63ea808cd68610e458fc83`：[38566887480](https://github.com/minexin/RFSim/actions/runs/38566887480)
- 双线性适配器 `31b291e3ccfbd111687aec4712dc0abf6b793bdb`：[38568395820](https://github.com/minexin/RFSim/actions/runs/38568395820)
- 因子域噪声 `f7f94f81d7f3b8d5d5931e3a7a8a7be51566b0ca`：[38571067267](https://github.com/minexin/RFSim/actions/runs/38571067267)
- 接受工作点噪声 `0d7afc5d6738147a407c2ea16c3d693a31343128`：[38572379978](https://github.com/minexin/RFSim/actions/runs/38572379978)
- 旧证据文档 `d64ac59c380bc1bd3367846c68646d60585da382`：[38574445472](https://github.com/minexin/RFSim/actions/runs/38574445472)
- 原生 PHD `ddd2e4be64a628f3e21cd954a6f752144f3e944c`：[38575586569](https://github.com/minexin/RFSim/actions/runs/38575586569)
- SDK 归档 `1a6d1bb60b2513aaff6648acd70c5e50788e9f65`：[38578171209](https://github.com/minexin/RFSim/actions/runs/38578171209)
- pilot LF 修复 `8b555eecb9e878949703b2640a0bfd039ba518c6`：[38581465433](https://github.com/minexin/RFSim/actions/runs/38581465433)
- 迁移修复 `0d18e464bd825a2851cd5bda2706395d5a6915d6`：[38583782458](https://github.com/minexin/RFSim/actions/runs/38583782458)
- 基础 API `89c93d57d23d9aeea150a21468662a7d00fe10d9`：[38595759547](https://github.com/minexin/RFSim/actions/runs/38595759547)
- 确定性 wave `1a0f5ffcd27d9efad09ecd59b238a8e622900367`：[38597218325](https://github.com/minexin/RFSim/actions/runs/38597218325)
- SDK 数值示例 `45bafd674b46f9a7f35353631f72c131a0edc5c1`：[38598650770](https://github.com/minexin/RFSim/actions/runs/38598650770)
- Touchstone decimal 可移植性 `a987c3dc116f31477f6c97fb23d0c3a4a22f2f3c`：[38607571844](https://github.com/minexin/RFSim/actions/runs/38607571844)
- 均匀线共振修复 `a2bbfd5dd3366082620cbd0ebf99dbf11628bd40`：[38615005226](https://github.com/minexin/RFSim/actions/runs/38615005226)

- 工程证据覆盖层 `3160ba57b4dcb228dca775300a09fb66429389e8`：[38617787622](https://github.com/minexin/RFSim/actions/runs/38617787622)
- RLGC 缩放算术 `7a59e5e812f677acc84b75e77d77db787a545291`：[38624386322](https://github.com/minexin/RFSim/actions/runs/38624386322)
- 线性外部 S 分解 `c9bc6cfe66e2b1d211848d89b103e19b6e735abf`：[38632765095](https://github.com/minexin/RFSim/actions/runs/38632765095)
- 固定泵 PHD 网络 `87cd75d356f62693a59500af088277382c42ac5b`：[38641741754](https://github.com/minexin/RFSim/actions/runs/38641741754)

- frozen nodal noise 修复 `a0218741785dba724f30c779dc2596284fae9d66`：[38706950468](https://github.com/minexin/RFSim/actions/runs/38706950468)
- 表格 model/coherent adapter `71999c42cdafe5c01b07580a033844adcc4e0664`：[38710540300](https://github.com/minexin/RFSim/actions/runs/38710540300)
- RFAMP holdout `586fd46f43319ab2db384958452ffefddd055831`：[38720558955](https://github.com/minexin/RFSim/actions/runs/38720558955)
- DC 有限源与安装格式证据 `dddc0593961b3d734ee7702d96ee9d4ca370519a`：[38722607277](https://github.com/minexin/RFSim/actions/runs/38722607277)

- raw reader `e633109022aa6e447127361952ad1ccab24e4039`：[38756583678](https://github.com/minexin/RFSim/actions/runs/38756583678)

- raw catalog `8b6105ac7295afc72376c206c477edd73d52e8b7`：[38764883668](https://github.com/minexin/RFSim/actions/runs/38764883668)

- 前次能力证据覆盖层 `85b5a9c78098310f4f5afb2af7fd2ce2bec0210a`：[38767456505](https://github.com/minexin/RFSim/actions/runs/38767456505)
- locked PI PLL `599268d2c99aaf4b31b374f0a76172f4137044c2`：[38773377200](https://github.com/minexin/RFSim/actions/runs/38773377200)
- 共享复相位响应 `1e2dbfba0b00a63a8b2779098ae41ae65647e079`：[38779870896](https://github.com/minexin/RFSim/actions/runs/38779870896)
- 有限频带 jitter `b2dcccf819d2ee0451f208b2e94334c927b8f374`：[38790793537](https://github.com/minexin/RFSim/actions/runs/38790793537)
- 两个 installed SDK 工作流 `3f75c58f4471d24aaf8c2f908ada678da9379a91`：[38793055242](https://github.com/minexin/RFSim/actions/runs/38793055242)
- 只读 installed X-parameter 重放工具 `c2c3252aacb75642adc6ad294009d75b6d13db91`：[38795700296](https://github.com/minexin/RFSim/actions/runs/38795700296)
- 有界线性 MNA `5c24a778e15c2ef666e5747104b4218ece04dbf8`：[38804226969](https://github.com/minexin/RFSim/actions/runs/38804226969)
- MNA 复参考功率波 S `5ddc39e74cb5a8a9e75536abfe75e532e6fd1929`：[38810746626](https://github.com/minexin/RFSim/actions/runs/38810746626)

- Touchstone raw 两端口噪声 writer `f603bcf42e1db12fdd50704b0c73d9a189d557de`：[38813359905](https://github.com/minexin/RFSim/actions/runs/38813359905)
- 显式等温 MNA 噪声 SDK 示例 `5c5812e0f81ef4fa45893784f9f9e7430170e218`：[38815413675](https://github.com/minexin/RFSim/actions/runs/38815413675)


- raw 字节跨平台修复 `71e56a157d2eb2ecd05831e6629a4b97442e19cd`：[38819089099](https://github.com/minexin/RFSim/actions/runs/38819089099)
- 独立弱噪声块 `d84c60558abbf4ff2737476771970d0fb5bbf28b`：[38823709238](https://github.com/minexin/RFSim/actions/runs/38823709238)
- 单/双音归档重放 `b5ed5efa5b95a79eba8ea156738158c31326fd93`：[38825457841](https://github.com/minexin/RFSim/actions/runs/38825457841)
- 共享来源归档重放 `118c1a77850001c260705860bc2d7d37247bdf6a`：[38827153851](https://github.com/minexin/RFSim/actions/runs/38827153851)
- 两级 source-cubic 诊断重放 `02c71637c751e7e3f4e6315c036803ef26a87f65`：[38832230566](https://github.com/minexin/RFSim/actions/runs/38832230566)

- 行为级系统示例的严格 SDK 清单修复 `6ef3b0c0c840d2434b4b59e959b6c93eb4beb211`：[38835703380](https://github.com/minexin/RFSim/actions/runs/38835703380)

frozen noise 原提交 `2033fd33c47a49f0b3ce33b322e97dfe5cc77cc9` 的
[38705856824](https://github.com/minexin/RFSim/actions/runs/38705856824) 为 14 成功、4 失败。
新严格快照直接取自实际 REST jobs 的 head_sha/run_id/status，保留旧阶段原格式快照。
当前机读索引从实际列表核算为 45 个不同终态运行、810 个逐作业记录：
786 成功、23 失败、1 取消，不能写成全部通过。前次 37 条索引及其全部快照原样保留；
本轮追加的 8 个已完成运行均保留完整原始 REST run/jobs JSON、SHA-256 和逐 job 映射。
最终工程源码 CI 38835703380 已在此列表中；后续文档覆盖层自身的 CI 仍须单独核验。
checker 核验原始字节、run/head、完整 18 作业矩阵和每项 job 的身份/终态；文字计数不是独立证据。
只有原 27 份 normalized-only 历史运行享有既有例外，其余新增运行必须有完整 raw 绑定。

原失败也保留：pilot `2752af6a5010a7c5ee5fbeddda42c360cf54ff97`
[38579642585](https://github.com/minexin/RFSim/actions/runs/38579642585) 为 16 成功、2 失败；
writer `a88dcc041917e30a620a6f06a98caa56042b5b04`
[38600226155](https://github.com/minexin/RFSim/actions/runs/38600226155) 为 14 成功、3 失败、1 取消。
本轮追加文档覆盖层 `eebef2f38a5a758446374c9a7a963997f9dc03ff` 的
[38818254433](https://github.com/minexin/RFSim/actions/runs/38818254433)：16 成功、2 失败。
原行为示例 `350bc9fcb4b2663972c639ad47429711a35c2eb8` 的
[38834090412](https://github.com/minexin/RFSim/actions/runs/38834090412) 为 6 成功、12 失败。
以上五个历史 run 的实际 REST conclusion 均为 failure；writer 的 1 个 cancelled 是作业结论，
不计作失败作业。后续修复不会改写这些历史结论。

逐作业快照由机读映射引用。18 作业是 Windows/Ubuntu/macOS × Debug/Release ×
核心/纯 SDK/归档重放，并非 18 个厂商兼容测试。所有发布门禁保持开放。

## 保留失败与有限结论

- AM/PM：显式校准后的 549 个无告警登记点、37 个告警诊断点及额外端点分开；
  原始解析映射 110/586 的失败仍在。给定实测输出功率的相位吻合不等于网络功率预测通过。
- DataPoints：输出离散候选 8/8 是固定表的有限新配置预测；输入候选 2/8。
  原连续样条及仅名义零旁路反例保留，不导出连续律、精确边界或导数。
- 显式高阶归档 1413 来源项通过不代表自动 RFAMP 系数；0 dB 级联、RFPwrIn、
  衰减器噪声原失败不改判。衰减器约 9.8915667e-7 残差仍超过原 1e-7 阈值。
  频谱削减关闭后恢复的来源不再列为未知缺项。
- 通用 ModelAdapter 和原生 PHD 切片不等于专用器件库或厂商参数翻译器。
  安装 PHD 格式已有诊断，RFAMP A/B/C/D 已采集；系数语义及更广泛数值验收仍未建立。

原始报告、容差、告警、校准/独立评分边界和历史构建计数不重写。
历史 Python/JSON 叙述按[退役说明](python-interface-retirement.md)阅读；产品 Python 接口已退役，
开发用证据脚本不是恢复 Python 产品。

## 尚需外部证据与发布选择

- RFAMP 参数律：A/B/C/D 及 380 来源有限 holdout 已完成，尚不能识别专有系数律或推广到其他参数、频率与全级联。保持预注册、原始实际复输入及原容差，不重新要求已完成的采集。
- PHD：安装格式与帮助已检查；legacy 归一化、完整系数转换、参考面与复阻抗映射、变泵数据和导数验证仍缺。格式证据不能替代独立厂商数值预测。
- 其他厂商噪声、ZPORT/CS 归一化、DC/RC 验收、PLL/PNCP 及专用器件族逐项未证实；原生 grounded DC/G/C、线性 MNA/S 与有界 PLL/相位能力已存在，但不闭合厂商验收。[目录与覆盖边界](rf-design-catalog.md)只是库存。原始 RFPwrIn、级联和衰减器噪声失败保留。
- 正式许可、公开分发、版本/标签和 API 稳定性需要用户的产品选择；本轮未发布或选定许可。

这些是后续依赖清单，不是新的操作要求，也不表示全项目完成或可计算总百分比。
未来覆盖层本身的精确提交 CI 仍须单独核验，不将源基线 CI 写成未来成功，不为追录自身 CI 迭代源基线。

## 校验与维护

[JSON Schema](../validation/compatibility-evidence.schema.json)使用 Draft 2020-12。
开发检查可运行 `python scripts/reference/check_compatibility_evidence.py --self-test`，无需产品
Python 包或第三方模块。它验证所用 schema 子集、路径、唯一 ID、目录库存、CI 提交/运行绑定、
终态矩阵与成功/失败/取消计数；所有快照均强制逐 job SHA/run/status 绑定。
早期快照逐 job 字段根据独立重新取得的原始 CI 数据补全。
自测注入错误引用、重复 ID/运行、虚假 CI 成功及 job 身份/状态/计数篡改，检查拒绝行为。
既有原始 REST 快照路径/hash 绑定继续使用：还校验原始运行/完整 jobs 与归一化记录的一致性，
拒绝原始字节篡改、重新计算 hash 后的错误身份及缺失/不完整绑定。只有明确列出的原 27 个
历史运行可沿用 normalized-only 格式，新增运行不能删除 raw 字段降级；历史快照不重写。
checker/schema 是开发证据工具，默认 C++ SDK 构建不依赖它；不恢复 Python 产品接口。

该工具只校验证据元数据，不验证自由文本数值/厂商结论真实性，不执行 C++ 数值测试，
也不将归档失败提升为通过。新增实现应有明确契约与独立数值测试，再引用实际记录；
新增厂商通过须有独立场景/原始采集/原容差依据，不能仅改文档中的“通过”。
