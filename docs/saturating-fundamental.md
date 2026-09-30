# 饱和基波模型

`SaturatingFundamentalCompression` 在小信号至 P1dB 区域复用三阶基波压缩关系，
在 P1dB 以上用增量 tanh 连接到输出饱和功率。它保留输入相位，不产生谐波、
互调、AM/PM 或噪声，也不求解非线性反馈网络。

## 方程和有效域

G 为线性功率增益，A=sqrt(G)，r=10^(-1/20)，P1 和 Ps 为输出 P1dB、OPSAT
的瓦特值。输入 P1dB 为 Pi1=P1/(G*r²)，x1=sqrt(Pi1)，y1=sqrt(P1)，L=sqrt(Ps)。
驱动幅度 x=sqrt(Ptotal)，其中 Ptotal 默认为选定基波的 |a|²。

```text
x <= x1: y = A*x*[1-(1-r)*(x/x1)^2]
x >  x1: y = y1 + (L-y1)*tanh[A*(3*r-2)*(x-x1)/(L-y1)]
```

输出基波为 `a*y/x`；零驱动走低功率分支，避免除零。两分支在 P1dB 处的
幅度与一阶导数连续，驱动对应的输出幅度单调趋近 L。tanh 作用于超过 P1dB
后的增量，区别于此前未吻合的“整体幅度平移 tanh”假设。系数只由 G、P1dB、
OPSAT 推导，没有拟合 SystemVue 采集误差。

参数必须有限，OPSAT 严格大于 OP1dB；增益、锚点功率和幅度余量必须可表示。
总输入 RF 功率必须有限、非负，包含选定基波功率（沿用 16 epsilon 相对舍入容差）。
无效参数、无法表示的输出报错。显式总功率接口只定义额外 RF 驱动如何缩减
所选基波，调用方仍须求解其他端口和频率。厂商交叉验证仅涉及单音，不能据此
宣称多音饱和或反馈网络已验证。旧 P1dBFundamentalCompression 保留超 P1dB 拒绝行为。

## 接口

```cpp
#include <rfmodel/saturating_fundamental.hpp>
rfmodel::SaturatingFundamentalCompression amp(20., 20., 23.);
auto single = amp.transmit_fundamental(rfmodel::Complex{0., .1});
auto driven = amp.transmit_fundamental(rfmodel::Complex{0., .05}, .01);
```

C 入口为 `rfmodel_saturating_fundamental(G_dB, OP1dB_dBm, OPSAT_dBm, incident,
total_incident_power_w, output)`。输出指针必须非空，错误时保持原值。Python：

```python
wave = library.saturating_fundamental(
    .1j, power_gain_db=20, output_p1db_dbm=20, output_saturation_dbm=23)
partial = library.saturating_fundamental(
    .05j, power_gain_db=20, output_p1db_dbm=20, output_saturation_dbm=23,
    total_incident_power_w=.01)
```

JSON 新类型 `saturating_fundamental` 使用 power_gain_db、output_p1db_dbm、
output_saturation_dbm 三个参数。当前只允许一个非零 RF 音调，拒绝 DC 和多音；
空输入仍校验参数。例子 examples/single-tone-saturation.json 输入 +10 dBm，
输出 0.1992293703616 W，保持 +90° 相位。Python 必须搭配含新 C 符号的动态库；
ABI 号仍为 1，旧二进制不能提供新接口。安装包包含新 C++ 头文件。

## 2026-09-30 交叉验证

本机帮助 `sim/Gain_Compression_and_Intermod_Generation.html` 仅公开“低功率三阶、
饱和区双曲正切”的描述，没有公开此精确公式。当前实现由连续性与渐近值推导并
经过新采集交叉验证，不声称掌握厂商内部算法。

固定 G=20 dB、OP1dB=20 dBm、OIP2=40 dBm、OIP3=30 dBm、RISO=100 dB、
50 ohm、1 GHz。OPSAT=23 dBm 的 −30、+0.9、+3、+6、+10 dBm 使用此前数据；
公式确定后新增 +2 dBm，以及 OPSAT=26 dBm/输入 +8 dBm 两个交叉验证点。
七点的原生基波输出相对差异均小于 3e-8。+2 点约 2.35e-8，OPSAT26/+8 点约
1.62e-8。参数、数据新鲜度和拓扑均复核。

超过 P1dB 的五点带厂商精度警告，因此报告统一标记 eligible_for_compatibility=false，
仅记录是否在 1e-7 诊断阈值内，不代表严格产品兼容通过。先前整体 tanh 假设的
差异报告保留，见 [饱和诊断](systemvue-saturation-diagnostic.md)。

精简数据为 validation/systemvue-2023-incremental-tanh-captures.json，原生报告为
validation/systemvue-2023-native-saturation-diagnostic.json，包含原始采集及 DLL 散列。

```powershell
python scripts/reference/compare-saturating-fundamental.py `
  build-msvc/Release/rfmodel_c.dll `
  validation/systemvue-2023-incremental-tanh-captures.json `
  build-reference/native-saturation-replay.json
```

参考实例已恢复 sample、输入 −30 dBm、OPSAT=23 dBm，恢复记录位于
build-reference/compression-incremental-tanh-restored-001，H2/H3 与原基准完全一致。

本机 Release CTest 51/51；Debug 原 50/50 加新增原生参考诊断 1/1 均通过。
独立安装消费者 Debug/Release 各 2/2，通过新 C++/C 符号求值。解析回归覆盖锚点、
连续导数、深饱和极限、相位、显式总驱动、失败不写输出、JSON 边界及旧 API 域不变。
参考回归检查改变实测输出会暴露差异，未知消息和默认严格验收仍拒绝警告。
