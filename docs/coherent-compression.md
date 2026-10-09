# 相干载波的共同基波压缩

此接口把现有 [饱和基波曲线](saturating-fundamental.md) 接入相干系统。
适用于匹配、单向的基波压缩节点：先归并到同一输入端口的来源分量，再以
全部载波的总功率驱动同一个增益曲线，输出各来源的复幅度。

## 计算约定

1. 按频率、谱类型、带宽、相干组进行原生归并。同键幅度相加，其他键功率相加；
   同相增强、反相抵消发生在压缩驱动计算之前。
2. 输入驱动 P 是归并后各组 |a|² 的和，单位 W。不同频率和独立来源共同驱动。
3. 对每个载波调用既有 SaturatingFundamentalCompression.transmit_fundamental(a, P)。
   曲线在 P1dB 以下使用三次基波响应，在其上使用增量 tanh，所有组共用同一 P。
4. 返回实际输入驱动及归并输出；保持频率、带宽、相干编号、复相位和零幅度组。

因此两组独立载波各占输入 P1dB 的一半时，总输出等于 OP1dB，
每组占输出功率的一半。深饱和时全部组的总功率趋于同一个 OPSAT，
而不是每组分别获得 OPSAT。被相干抵消的分量不作为残留功率压缩其他载波。

仅接受 source 谱类型（也包括保持此类型的理想混频输出）。
已生成的 harmonic/intermod 会被拒绝，避免把它们静默当成新基波。
该模型不生成谐波或互调，不含 AM/PM、噪声、反向端口和非线性反馈。
多载波共同压缩采用上述模型假设，不能代替完整 RF Design 放大器。

## C++、C 与 Python

C++ 头文件 `rfmodel/coherent_compression.hpp`：

```cpp
const rfmodel::SaturatingFundamentalCompression amp(20., 20., 23.);
const auto result = rfmodel::compress_coherent_fundamentals(spacing_hz, carriers, amp);
// result.input_power_w, result.output.components / power_by_bin_w / total_power_w
```

C 导出 `rfmodel_compress_coherent_fundamentals`。
使用现有 coherent_component/bin_power 缓冲区，另返回 input_power_w。
最多 4096 个输入分量；所有数组与标量不得重叠，任何失败均保持输出不变。
空输入允许 NULL 数组，但仍必须提供计数和两个功率指针；参数仍须有效。

```python
result = library.compress_coherent_fundamentals(
    spacing_hz, carriers,
    power_gain_db=20., output_p1db_dbm=20., output_saturation_dbm=23.)
print(result.input_power_w, result.output.total_power_w)
```

返回 CoherentCompressionResult(input_power_w, output)，output 是 CoherentReduction。
Python 包需要包含新增导出符号的同阶段动态库；ABI 版本仍为 1，
这不意味着本阶段 Python 包能加载缺少该符号的旧 DLL。

## 系统图节点

`rfmodel.coherent-system` version=1 新增：

```json
{
  "id": "amp",
  "type": "fundamental_compression",
  "input": "rf",
  "output": "compressed",
  "power_gain_db": 20,
  "output_p1db_dbm": 20,
  "output_saturation_dbm": 23
}
```

字段全部必填；output 必须是新流。单输入流可包含多个来源或频率，
多路物理端口应先通过线性合路网络求得此流，不能直接累加流功率替代网络。
结果 stages 中该节点新增 input_power_w，streams 保存其完整输出。
空流仍验证增益、OP1dB 与 OPSAT；跨阶段反射和负载迭代仍不支持。

示例 `examples/coherent-compressed-receiver.json` 在正交镜像抑制接收链之前
加入压缩节点。1 GHz、1 W 输入被限制到约 0.199526 W，然后分路和变频：
共时钟 LO 时 200 MHz 输出约 0.199526 W，1800 MHz 在浮点范围内相消；
独立 LO 时每个边带约 0.099763 W。

```powershell
python -m rfmodel examples/coherent-compressed-receiver.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/compressed-receiver-result.json
```

## 验证范围

原生测试覆盖 P1dB 锚点、多频共同驱动、共享饱和预算、反相抵消、带宽分组、
输入换序、空输入、非法类型和功率溢出。C 测试检查缓冲区不足及错误参数时的
原子失败；独立 C/C++ 安装消费者及 Python wheel 测试覆盖新接口。
系统图回归覆盖压缩后分路/混频/合路、独立 LO、阻塞载波和非法空流参数。

这些是解析、接口和组合行为的验证。本次没有新增 SystemVue 压缩实测。
现有单音曲线参考仍保留；方向总谱的线性相干实测不能证明非线性共同驱动兼容。
RFPwrIn 五项差异仍未解释；该接口不把 RFPwrIn 当作驱动的等价定义。
后续仍需真实多源驱动放大器对照，以及谐波、互调来源跨级传播。

本阶段本地 MSVC Debug/Release 各 67/67，安装后 C/C++ 消费者各 2/2；
独立 wheel 的 Python API 61 项和系统图 13 项通过，CLI 示例已执行。
100 个 C/C++ 文件通过统一格式检查；数值和散列见
[解析验证记录](../validation/coherent-compression-analytic.json)。远端 CI 按提交运行核验。
