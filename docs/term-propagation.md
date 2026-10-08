# 互调项在线性后级中的传播

多音放大器输出的本级来源项现可经过线性网络继续传播，覆盖 C++、C、Python
及 JSON/CLI。每项按实际输出频率乘网络的外部 S21，保留 order、bin 和带符号
contributors；不先合并同频率项，保留精确陷波后的零项。

该能力连接已有互调模型与线性滤波/电缆/衰减网络。外部输入和输出匹配，网络
内部反射和连接反馈由现有线性求解器处理。多个后级之间按匹配单向边界串接，
不包含后级反射返回放大器后的非线性反馈，不引入噪声。具有耦合或失配的线性
元件需要放进同一个后级网络内共同求解。

## 原生接口

```cpp
#include <rfmodel/term_propagation.hpp>
auto output = rfmodel::transmit_linear_terms(
    spacing_hz, traced.terms, {0, 1}, 50., build_network_at_frequency);
```

回调接收 Hz，返回该频率下的 LinearNetwork。每个不同频率调用一次，用单位
入射波取得传输系数，再分别传播各项；相消的来源不会因此从数据中消失。
空输入仍在 0 Hz 构建/验证网络，含表格数据的模型需要覆盖此验证频率或显式
选择支持的带外策略。函数返回新数组，不修改输入，保持输入顺序。

输入最多 4096 项、2048 个不同输出频率。order 为 1..3，bin 必须为正数且
频率有限，幅度功率可表示；contributors 升序、非零、只用前 order 个，其余
补零，求和必须等于 bin。重复 (order,bin,contributors) 身份被拒绝。调用者
应先明确合并同一身份的路径或在更高层保留路径 ID，不能把重复输入当独立源。
输出功率溢出整体报错。分离的项不被隐式做相干求和或非相干功率相加。

C `rfmodel_network_transmit_terms` 接收固定 S 的 network、两个 external_ports、
spacing_hz 和 rfmodel_amplifier_term 数组，容量以项计。输出数组和 count 在任一
失败时保持不变；空输入可使用 NULL 数组，但仍检查拓扑。输入输出缓冲区按
公共 C 契约互不重叠。Python `network.transmit_terms(spacing_hz, terms, ports)`
返回 AmplifierMixingTerm 元组，保留来源信息，网络关闭后拒绝调用。

## JSON 后级与路径

`rfmodel.amplifier-components` version=1 增加可选 post_stages 数组。非空时要求
include_terms=true，至多 128 级；每级严格包含唯一非空 id、type=linear_network
和 network。network 使用既有线性网络的 devices、external_ports、可选
connections/terminations，必须恰有两个外部端口。器件只接受 id 和 s/model，
显式拒绝噪声字段，避免输出造成已处理噪声的误解。

后级支持固定 S、传输线、RLGC、线性放大器和 Touchstone 等既有模型；JSON
层对每个实际频率构建并求解，再交由原生项传播接口处理。相对 Touchstone
路径按模型文件目录解析，CLI 的输出覆盖保护也覆盖后级引用文件。

输出根节点三个分阶谱和 terms 仍表示放大器输出；post_stages 则保存各后级的
terms 快照和累计 path（例如 ["cable","pad"]）。根节点的两种驱动功率仍属于
放大器输入，不被解释为后级功率。路径说明该快照经过的线性后级，尚不支持
分支/再合路的全局来源 ID 或第二个非线性器件生成新组合。

```powershell
$env:PYTHONPATH = 'python'
python -m rfmodel examples/multitone-linear-post-stages.json `
  --library build-msvc/Release/rfmodel_c.dll --output build-reference/linear-terms.json
```

示例每音 −3 dBm，先经过 6 dB 损耗、0.1 ns 延迟电缆，再经过幅度传输 0.5 的
衰减器。各来源保留，最终各项幅度乘 0.5×10^(−6/20)，相位按本项输出频率延迟。

## 验证边界

解析回归覆盖频率相关延迟、内部反射反馈、来源相消、陷波零项、同频率求解
复用、错误身份/参考阻抗/端口和功率溢出。C 验证失败输出原子性；Python/CLI
验证多级快照、相对 Touchstone 路径、输入覆盖保护和 schema 拒绝。既有 48 项
SystemVue 功率参考继续回归，但尚未新增“放大器加线性后级”的 SystemVue
实测，不能把解析一致性表述为整条链路已通过厂商对照。

2026-10-08 本机 Debug/Release 各 58/58 CTest 通过，独立安装 C/C++ 消费者
两种配置各 2/2 通过。示例 CLI 保留 16 项，最终复传输系数相对解析值的最大
差异约 4.01e-16；格式检查覆盖 90 个 C/C++ 文件。跨平台验证绑定本提交 CI。
