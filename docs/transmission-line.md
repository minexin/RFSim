# 有阻抗失配的均匀传输线

按单位长度 R、L、G、C 建模的传输线见 [分布参数模型](rlgc-transmission-line.md)。

`rfmodel/models/transmission_line.hpp` 中的 `TransmissionLineModel` 实现
`RFDeviceModel` 和 `SParameterProvider`，可以直接接入现有线性网络、频率扫描和
被动热噪声计算。两个端口使用相同的正实数参考阻抗，线的特性阻抗独立指定。

```cpp
rfmodel::TransmissionLineModel line(
    "100 ohm line", 100., 0.25e-9, 0., 50.);
const auto s = line.s_parameters(1e9);
// S11 = 0.6, S21 = -j * 0.8
```

构造参数依次为名称、特性阻抗（欧姆）、单程延迟（秒）、单程传播损耗（dB，默认 0）、
端口参考阻抗（欧姆，默认 50）。传播损耗不是失配二端口测得的插入损耗。
延迟和损耗非负；频率为非负有限值，单位 Hz。端口编号为 0、1。

令 `r=(Zc-Z0)/(Zc+Z0)`，`p=10^(-loss/20)*exp(-j*2*pi*f*delay)`，则：

- `S11=S22=r*(1-p*p)/(1-r*r*p*p)`；
- `S21=S12=(1-r*r)*p/(1-r*r*p*p)`。

实现以缩放阻抗计算 r，避免阻抗求和溢出，并用有界传播因子避免长有损线的
双曲函数溢出。接近全反射到双精度难以分辨的阻抗比仍会显式拒绝
（`1-abs(r) <= 64*epsilon`）；这不是所有已接受输入的精度保证。

为避免合法高失配线在共振处放大 `sin(pi)` 残差，先在周期域约化至最近四分之一
周期，再计算小残余相位。精确整数、半整数、四分之一周期保留精确象限值；
相邻可表示周期不会被阈值吸附。耦合项直接由缩放阻抗比
`q=min(Zc,Z0)/max(Zc,Z0)` 计算为 `4q/(1+q)^2`，不从接近 ±1 的 r 相减。
保留原始损耗，用 `expm1` 和 `2*sin²(theta)` 计算传播亏损，再按耦合归一化；
极小损耗与相位先缩放再转化，避免次正规输入先下溢、丢失本可被失配放大的响应。
实部与虚部分别使用消去相消项的等价式，不依赖 `long double` 比 `double` 更宽。

这些改进不改变常数均匀线物理模型，也不消除极靠近共振时输入 double 的条件数。
一般频率与延迟相乘仍先舍入为 double，极大乘积的相位精度限制仍在；
乘积溢出仍报错，即使传播衰减已使透射为零。极小周期使用二进制指数重排以保留
其归一化响应，但不承诺恢复一般乘积舍入前的精确相位。

这是常数实特性阻抗、常数损耗和恒定群延迟的模型，不包含频变 RLGC、复数特性阻抗、
趋肤损耗或色散，也未宣称与 SystemVue 某一传输线器件的所有参数等价。
热噪声可通过已有 `passive_thermal_noise(s, temperature_k)` 组合计算，器件自身不自动附加噪声。

解析回归覆盖四分之一波长变换、半波长直通、零频直通、无损矩阵幺正性、
匹配模型一致性、两段有损线级联等于双长线、极大损耗极限和非法输入。
新增固定的 100 位独立 ABCD oracle 回归覆盖双向高失配、精确与相邻象限、
次正规损耗/相位、普通参数、极大损耗，逐实/虚部分量核对，并检查互易、无源和
热噪声接口。开发工具 `scripts/reference/generate-transmission-line-oracle.py`
用 mpmath 从精确 binary64 输入生成 `tests/fixtures/transmission_line_oracle.hpp`；
产品、原生测试与安装消费者只读固定 C++ 数据，不依赖 Python 或 mpmath。
安装包消费测试直接包含此头文件并检查四分之一波长反射。
SystemVue 2023 数值对照仍待补充，不以解析测试替代兼容验收。

2026-09-28 本地验证：MSVC Debug/Release 全套 CTest 各 38/38，安装后消费项目
Debug/Release 各 1/1；68 个 C++ 文件通过格式检查。本次变更的 Linux/macOS
验证由推送后的 CI 执行，本地结果不代表其已通过。

后续核验：提交 `ae59183` 的 CI 运行 `36433135457` 已完成，Windows、Ubuntu、macOS
各 Debug/Release 共六项任务全部成功，证据见
`validation/cross-platform-ae59183.json`；这份记录仅对应该提交。
