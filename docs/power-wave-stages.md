# RF 线性阶段的混合参考阻抗

相干网络、相干系统图、频谱链和放大器来源后级的线性子网络支持逐器件、逐端口的复功率波参考。阶段之间仍以顶层 `reference_ohms` 的公共正实参考交换波；不能将局部参考下提取的 S 直接作为阶段 S。

## 物理与坐标契约

内部连接沿用 [LinearNetwork 的电压连续、电流守恒接线](power-wave-references.md)。先消去内部端口，提取选择的外部 S，再把所有外部端口转换到阶段的公共实参考。该变换也改变外部“匹配”所对应的物理源/负载条件。例如局部参考为 31+j12 Ω，阶段参考为 50 Ω 时，阶段输入仍是 50 Ω 下的入射波。

系统图保持既有前馈阶段边界；每个线性子网络内部可有反射往返。阶段之间没有新增反射反馈。固定频点的网络缩减不会产生频率转换。噪声、非线性偏置及变频网络的独立复参考仍使用各自接口和边界，不能由本阶段的信号缩减推断完成。

## C++ / C / Python 快照

新头文件 `rfmodel/power_wave_network.hpp` 提供：

```cpp
auto reduced = rfmodel::reduce_power_wave_network(network, {0, 3}, 75.);
auto scattering = reduced.external_s({0, 1});
```

原网络可以使用任意合法的局部功率波参考，其构造时默认参考不决定结果参考。选择端口必须未连接、未终止，全部其他端口必须已连接或终止；任一非零独立边界源都会报错，避免在 S 缩减中丢失源偏置。返回普通 `LinearNetwork`，端口按选择顺序重新编号为 0..N−1，全部参考为指定正实数。它独立拥有矩阵，不引用原网络。不同频点可分别构造原网络并缩减，再交给既有 `transmit_linear_spectrum`、`transmit_coherent_network` 或 `transmit_linear_terms`。

不要求物理 Z 矩阵可逆：理想直通也可缩减。若最终坐标变换或网络求解奇异，沿用既有数值错误。只有所选外部参考已经全部等于目标参考时才跳过坐标变换。原生网络类布局与旧传播函数均未改变。

C 增加 `rfmodel_network_reduce_power_waves(network, ports, port_count, reference_ohms, out)`。`port_count` 为 1..1024；`out` 须指向 NULL，且不得与输入端口数组或原句柄重叠。失败不写 `*out`，成功产生独立句柄，用 `rfmodel_network_destroy` 释放。已有句柄、结构布局和 ABI 版本 1 保留；新 Python wheel 需要含此新导出符号的配套动态库。

```python
with library.network() as original:
    # 局部 S 是这些参考下的矩阵，而非自动识别为 50 ohm 数据。
    original.add_power_waves([[0, 1], [1, 0]], [25, 100])
    stage = original.reduce_power_waves([0, 1], reference_ohms=50)
# 原句柄关闭后，快照继续有效。
with stage:
    output = stage.transmit_spectrum(1e9, {1: 0.1}, [0, 1])
    # 单个上述局部块相当于阻抗变换，S21=0.8，输出为 0.08。
```

Python 返回现有 `Network` 类型，保留锁、上下文管理和显式关闭；构造 Python 快照对象失败时释放原生句柄。端口迭代器读取上限为 1025 项，超限即拒绝。布尔参考、非法端口、非正/非有限参考均拒绝。旧传播入口直接接收混合参考网络仍会报错，调用者须明确选择缩减。

这是信号 S 快照。`LinearNetwork`/C 句柄原本不保存内生噪声协方差，快照也不附带噪声；需要噪声时使用线性分析的 S/噪声联合重归一化接口。

## JSON 的线性子网络

以下 version 1 分析中的线性子网络新增可选器件字段 `reference_impedances_ohms`：

- `rfmodel.coherent-network` 的 `network`。
- `rfmodel.coherent-system` 的 `linear_network` 阶段，包括递归多项式及混合来源表达式执行路径。
- `rfmodel.spectrum-chain` 的 `linear_network` 阶段。
- `rfmodel.amplifier-components` 的线性 `post_stages`。

每个器件一项数组，长度等于本地端口数；每项为实数或 `[实部, 虚部]`，实部须严格为正，数值须有限。所有实际求值频点复用这些坐标。参数器件 `model`（含 Touchstone）先在顶层实参考求值，再由原生接口转换到局部参考；显式 `s` 已在所声明局部参考下，不二次转换。未声明的器件继续使用公共实参考。原格式没有声明局部参考时，执行和输出契约保持不变。

```json
{
  "id": "r",
  "model": {"type": "resistor", "connection": "series", "resistance_ohms": 25},
  "reference_impedances_ohms": [[31, 12], [73, -21]]
}
```

器件 `s_samples`、`reference_samples_ohms` 和任意输出参考覆盖仍不属于这些阶段格式：阶段频点由上游谐波/混频动态生成，没有独立的固定频率数组可供位置匹配。原生构建回调及单独的线性分析继续支持逐频率参考。终端反射依然在该端口局部坐标定义，不能把同一个反射系数当作任意参考下不变的物理负载。

共享阶段装配器完成外部参考转换后，才把矩阵交给既有波/来源传播内核。JSON 结果的顶层 `reference_ohms` 始终描述阶段流；来源编号、相干组、路径、重复频点及零贡献项保留既有契约。空谱仍校验网络和参考，不静默接受非法描述。

## 可重跑例子与证据

[混合参考 RC 系统图](../examples/coherent-mixed-reference-rc.json)先用电压多项式产生二次谐波，再通过串联 25 Ω 电阻及并联 1 pF 电容。两器件的四个端口采用不同复参考，包括外部端口。独立物理 ABCD 公式给出公共实参考 R 下：

```text
Y = j*2*pi*f*C
S21 = 2 / (2 + 25/R + (25+R)*Y)
```

```powershell
python -m rfmodel examples/coherent-mixed-reference-rc.json `
  --library build-reference/power-wave-stage-install-Release/bin/rfmodel_c.dll `
  --output build-reference/coherent-mixed-reference-rc-result.json
python scripts/verify-power-wave-stage-example.py `
  examples/coherent-mixed-reference-rc.json `
  build-reference/coherent-mixed-reference-rc-result.json
```

原生回归直接由 V/I 方程构造局部串联阻抗器件，在多组目标实参考、DC/RF 频点和双端激励下检查传输、电压连续、电流守恒与净吸收功率。还覆盖奇异 Z 的直通、非互易端口重排、取消后来源和零失真项、内部终端与非零源拒绝。

C 专项检查失败原子性、输出别名、已有句柄不被覆盖及独立生命周期。Python 16 项回归涵盖 RC 独立解析、三端口锁相/独立源合路、Touchstone、混频新增频点、多项式/高阶来源传播、内存异常释放、非法/空输入和 CLI 输出保留。完整本地、安装 wheel、旧 C 二进制及远程验证以[本阶段机读记录](../validation/power-wave-stage-validation-20261012.json)和对应提交 CI 为准。

这些是物理及工程回归。SystemVue ZPORT/CS、厂商路径预算、变频图跨语言复参考、混合参考非线性工作点和通用器件 DC 工作点仍未验收，A05 整项保持开放。

后续原生 C++ 已支持[变频网络逐通道复参考](conversion-power-wave.md)，包括 A/B、偏置和 C/P 联合转换及内部物理接线；变频 C/Python/JSON 扩展输入与混合参考非线性工作点仍未开放。
