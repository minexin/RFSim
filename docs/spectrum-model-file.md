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
