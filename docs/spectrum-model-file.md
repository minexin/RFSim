# 匹配单向频谱链路 JSON v1

`rfmodel.spectrum-chain` 保存公共频率网格、输入功率波及按顺序执行的器件。
它与线性网络文件分开，避免把含频率转换的处理误当作单频 S 参数网络。
所有器件传输均调用 C++ 核心；Python 只做解析、校验和顺序编排。

```powershell
python -m rfmodel examples/two-tone-mixer.json `
  --library build-msvc/Release/rfmodel_c.dll `
  --output build-reference/two-tone-mixer-result.json
```

需安装当前 Python 包或将仓库 python 目录加入 PYTHONPATH，输出目录须已存在。
执行规则与线性 CLI 相同：所有计算和 JSON 序列化成功后才写文件，失败返回 1。

## 字段与单位

顶层必填 format（固定 `rfmodel.spectrum-chain`）、version（整数 1）、spacing_hz、
input、stages；可选 reference_ohms，默认 50。频率间隔及实参考阻抗均为正有限数。
input 是最多 2048 项的数组，每项含 bin 和 amplitude：bin 为 0 到 2147483647
的整数，amplitude 为实数或 `[实部, 虚部]`，单位 sqrt(W)。bin=0 时幅度必须为实数。
同一 bin 不得重复；空输入允许。实际频率为 bin*spacing_hz，负频率由核心共轭补全。

stages 按数组顺序执行，数量 1–512，每级 id 为唯一非空字符串。支持：

| type | 必填参数 | 可选参数 |
|---|---|---|
| cubic_amplifier | power_gain_db、input_ip3_dbm | 无 |
| p1db_fundamental | power_gain_db、output_p1db_dbm | 无 |
| ideal_mixer | lo_bin（正整数） | conversion_gain_db=0、lo_phase_radians=0 |
| linear_network | network（线性网络模板） | 无 |

全部级共用频率间隔及参考阻抗，不自动插值、滤波或改变参考阻抗。未知字段、未知器件、
重复键、重复 ID、非有限数以及错误参数均拒绝处理。没有脚本表达式执行。

## 输出与示例

结果 format 为 `rfmodel.spectrum-results`，包含 version、spacing_hz、reference_ohms、
规范化后的 input 及 stages。每一级保存 id、type、spectrum；每条谱线包含 bin、
frequency_hz、amplitude（实虚数组）、power_w。按 bin 升序输出，零项可能省略。
不把不同来源但落在同一 bin 的功率单独相加：相干复数波叠加由核心完成。

示例每个输入单音为 1e-5 W，频率 10 和 13 MHz；20 dB、IIP3=10 dBm 的三阶放大器
产生 7 MHz IM3，功率 1e-9 W。8 MHz LO 将它折叠至 1 MHz；两级完整输出频谱均保留。
这既可作为脚本批处理输入，也可用于后续 SystemVue 比对的 RFModel 侧重放。

## 范围与验证

仅支持级间匹配的单向链路：线性级内部可求解反射，但没有级间失配反馈、非线性网络连接、谐波平衡、变频噪声、LO 端口
加载和多阶 LO 杂散。三阶模型不能替代深压缩 PA 饱和模型；理想实混频器保留上下边带，
不会自动选取 IF。限制继承 [频谱接口](spectrum-api.md)。完整非线性网表仍待实现。

回归覆盖逐级 IM3 和差频功率、重复输入频点、非法 DC、重复器件 ID、未知参数、
版本和频率网格校验，以及命令行结果生成。SystemVue 仿真对照尚未执行。

2026-09-29 验证：Debug/Release 动态库上的 Python 专项各二十项通过；生成并安装
wheel 到项目内独立目录后，使用安装后的 Release 动态库再次通过二十项测试。
本轮未修改 C++ 数值核心，没有重复全套 C++ 回归；跨平台结果尚未核验。

## 线性网络级

linear_network 的 network 对象必填 devices、external_ports，可选 connections、terminations。
端点及连接规则沿用线性 JSON，external_ports 必须恰有两个端口，依次为输入、输出。
devices 只接受 id 加静态 s 或参数 model（二选一）；支持现有 transmission_line、
rlgc_line、linear_amplifier 和 touchstone 模型。频率由进入本级的当前谱线确定，包含上游非线性
和混频产生的新频点；参考阻抗统一继承链路。模板不接受 frequencies_hz、s_samples、
reference_ohms、信号激励或噪声字段，避免把原始输入频率表误用于新生成的谱线。

每个频点通过原生网络求解提取等效二端口，再调用原生频谱传输；匹配外部条件下
输出为 S21*a。内部连接和反射终端参与求解，级间反射不参与。非实数 DC 输出会报错。
空频谱仍在 0 Hz 校验网络模板，但输出为空。内部终端没有独立源或热发射。

examples/mixer-linear-network.json 为 10 MHz 单音经 8 MHz LO 混频，产生 2/18 MHz，
随后通过幅度 0.5 衰减器和 31.25 ns 延迟线。两个输出功率均为 0.25 W，
相位分别为 −π/8 与 −9π/8，验证在变频后的实际频率求值，而非复用输入频率响应。

本功能仍是匹配单向频谱分析，不代表完整 RF System Analysis 或 SystemVue 器件兼容。

2026-09-29 线性级接口验证：MSVC Debug/Release 全套各 43/43，安装后独立 C/C++
消费者各 2/2；Python 32 项测试在安装后的 Release 动态库上通过。
新示例通过 CLI 生成结果，格式检查通过 78 个 C/C++ 文件。未执行新的 SystemVue
实测；本次跨平台 CI 结果尚未核验。

Touchstone 级的 path 按整个频谱 JSON 文件的目录解析，直接调用 analyze_spectrum
时可传 base_directory。数据模型在实际输入谱线上插值并转换到链路参考电阻；
上游产生超出数据频带的谱线时默认报错，不能把这些谱线丢弃或默认为零。
空频谱仍按前述 0 Hz 校验规则处理，带限数据文件可能因此需要显式 clamp。
文件中的噪声段不参与确定性频谱传输。

## 单音 P1dB 基波压缩级

p1db_fundamental 调用原生 P1dBFundamentalCompression，仅允许至多一条非零 RF 谱线，
拒绝多音及非零 DC。显式零幅度谱线可忽略；空输入仍验证模型参数是否合法。
输出频率和相位保持不变，幅度按输出 P1dB 标定的主信号公式计算；超过输入 P1dB
域时报错，不钳制功率，也不生成谐波、互调或噪声。

如果上游混频器或多项式产生多个非零频点，本级会拒绝处理，不能逐音调用并把结果
当作多音压缩。级间仍为匹配单向连接，reference_ohms 沿用公共功率波约定。

examples/single-tone-compression.json 的 1 GHz 输入为 +90°、−9 dBm 单音，
经过小信号增益 20 dB、OP1dB=10 dBm 的放大器后输出为 0.1j sqrt(W)、10 mW。
后续 −90°、幅度 0.5 的线性网络使输出成为 0.05 sqrt(W)、2.5 mW。
CLI 在多音或越界失败时保留已有结果文件；不会写入部分计算结果。

2026-09-29：Debug/Release 动态库上的 Python 42 项测试均通过，安装后的 Release
动态库复测也通过，包含 CLI 成功及失败保护。本轮没有修改 C++ 核心或新增 SystemVue 实测。

## 通用电压多项式

`polynomial_amplifier` 级使用 `voltage_coefficients` 数组，长度 1..10，依次为
零阶至最高阶（最多九阶）的实数有限系数。计算关系为
`v_out(t) = sum(c[n] * v_in(t)^n)`，系数单位为 V_out / V_in^n；输入输出仍使用
公共 reference_ohms 下的 RMS 功率波，不能把系数误当作功率多项式系数。

```json
{"id":"square","type":"polynomial_amplifier","voltage_coefficients":[0,0,1]}
```

新例子 `examples/square-law-spectrum.json` 输入 10 MHz、0.1 sqrt(W)，参考阻抗
50 ohm，平方律输出 DC 为 0.0707106781 sqrt(W)、5 mW，20 MHz 为 0.05 sqrt(W)、
2.5 mW。DC、偶次/奇次谐波、和差频均保留；不会自动删除偏置或带外频点。
如需后接只接受 RF 的压缩接口，必须显式处理非零 DC。通用多项式没有隐含饱和
或 P1dB 钳位，也不代表已经还原 SystemVue RFAMP 的内部高阶拟合。

C 入口为 `rfmodel_polynomial_amplifier_transmit`，Python 为
`library.polynomial_amplifier(spacing_hz, amplitudes, voltage_coefficients=[...], reference_ohms=50)`。
二者复用已有 C++ MatchedPolynomialAmplifier；保留其稀疏卷积工作量、频率索引
和输出容量限制，超限失败而非截断。C 系数指针必须非空、数量 1..10，输出沿用
频谱缓冲区约定；容量不足时不覆盖已有输出元素。

2026-09-30：新增平方律单音相位、双音差频、空输入常量偏置、非法系数、JSON
结果和 C 缓冲区测试。Debug 全套中发现的测试路径变量错误已修正，Python 定向
复验通过，其余 47 项原运行通过；Release 全套 48/48。安装消费者 Debug/Release
各 2/2，安装 Release 动态库 Python 46 项通过。无新增 SystemVue 实测。
