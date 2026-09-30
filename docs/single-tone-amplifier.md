# 单音限幅放大器

`SingleToneLimitedAmplifier` 将独立饱和基波与软限幅后的 H2/H3 组合为匹配单向器件。
它接受一个非零 RF 音调，输出基波、二次和三次谐波，阻断 DC。非零 DC、多音
均明确拒绝，不能把多音逐个调用后相加当作已实现互调或交叉压缩。

## 限幅公式和来源

从 sample 的 H2/H3 反推等效输入功率比例，经诊断反解得到增量 tanh 的起始/
渐近功率接近 −4/+2 dBm。由此提出下式，随后用新参数组交叉验证：

```text
knee_dBm  = OP1dB_dBm - G_dB - 4
limit_dBm = OPSAT_dBm - G_dB - 1
k = sqrt(knee_W), L = sqrt(limit_W), x = abs(input_wave)
x <= k: limited_amplitude = x
x >  k: limited_amplitude = k + (L-k)*tanh[(x-k)/(L-k)]
```

两个偏移量是根据 sample 数据识别的假设，不是厂商公开公式，不能表述为完全
无实测标定。没有针对各点分别拟合修正量：同一对偏移量用于输入功率、OPSAT
和新参数组扫描。适用范围仍需扩大实测确认。

通用 `SoftInputLimiter(knee_power_w, limiting_power_w)` 接受显式瓦特锚点，保持
相位，拐点一阶导数连续。锚点必须正且有限，上限大于起点，幅度余量可表示。
放大器以限幅波驱动已有 IIP2/IIP3 多项式，保留 H2/H3，删除 DC，再用原始输入
计算 [饱和基波](saturating-fundamental.md) 并替换基波。

H2/H3 沿用正二阶、负三阶系数约定；截点幅值不唯一决定真实器件失真相位。
OP1dB/OPSAT 只标定基波，当前组合不额外保证任意截点参数下总输出谱功率小于
OPSAT。深饱和总谱、AM/PM、高阶产品、反向传播、失配和噪声均未完成。

## API 和模型文件

```cpp
#include <rfmodel/single_tone_amplifier.hpp>
rfmodel::SingleToneLimitedAmplifier amp(20., 20., 23., 20., 10., 50.);
auto output = amp.transmit({1e6, {{1000, rfmodel::Complex{0., .03162277660168379}}}});
```

构造参数依次为 G dB、OP1dB dBm、OPSAT dBm、IIP2 dBm、IIP3 dBm、共同实参考
阻抗 ohm。功率波为 RMS sqrt(W)。零值输入项忽略；空输入返回空谱，但模型
参数和频率网格仍须有效。频率越界、非有限功率明确报错。

C 入口 `rfmodel_single_tone_amplifier_transmit` 沿用频谱数组约定，至多输出三个
升序 bin；容量不足或校验失败时，输出数组和 output_count 均保持原值。
Python `library.single_tone_amplifier(spacing_hz, amplitudes, *, power_gain_db,
output_p1db_dbm, output_saturation_dbm, input_ip2_dbm, input_ip3_dbm, reference_ohms=50)`
返回字典，须搭配含新符号的同版动态库，ABI 号仍为 1。

JSON 级类型为 `single_tone_amplifier`，五个参数名同 Python，参考阻抗取链路公共值。
例子 examples/limited-single-tone.json 输入 1 GHz、0 dBm、+90°，产生三个 RF bin。
可接线性网络/滤波器；保留多个非零产品时不能直接进入另一个单音级。

## 2026-09-30 对照证据

九组均无 manager 消息，严格检查新鲜度、拓扑、16 项参数及谱身份/谱形：

| 参数组 | G/OP1dB/OPSAT，dB 或 dBm | 输入 dBm | 来源 |
| --- | --- | --- | --- |
| sample | 20/20/23 | −30、−10、−3、0、+0.9 | 原五点扫描，用于识别假设 |
| sample | 20/20/22，20/20/26 | 各 0 | 原 OPSAT 扫描 |
| limiter | 10/15/18 | +3、+5 | 假设提出后独立采集 |

两组 OIP2=40、OIP3=30 dBm、RISO=100 dB、NF=3 dB、50 ohm、1 GHz。新参数组
同时改变增益、P1dB、OPSAT，+3/+5 点的等效功率比例预测相对差异约
2.35e-8/1.87e-8。九组基波/H2/H3 共 27 项在原阈值 1e-7 下全部通过，最大相对
差异约 7.50e-8。报告保留未限幅多项式的原差异；不改写历史结论，也不声明
整类 RFAMP 或失真相位已经兼容。

精简数据 validation/systemvue-2023-limited-single-tone-captures.json；比较报告
validation/systemvue-2023-limited-single-tone.json，包含原始采集及 DLL 散列。

```powershell
python scripts/reference/compare-limited-single-tone.py build-msvc/Release/rfmodel_c.dll `
  validation/systemvue-2023-limited-single-tone-captures.json `
  build-reference/limited-single-tone-replay.json
```

运行器新增 `--compression-profile limiter`，该组拒绝 sample 专用 OPSAT 覆盖和
警告诊断模式。+5 点首次发现 SystemVue 已退出，第二次连接早于新实例 COM
就绪，两次失败记录保留在 compression-limiter-plus5-001/002。确认现有实例
COM 就绪且工作区为空后才打开专用副本，成功采集为 003。随后恢复 sample/
−30 dBm，无消息、H2/H3 与原基准完全一致，见 compression-limiter-restored-001。

本机 Release 全套 53/53；Debug 初次 52/53，修正新增测试对缓冲区不足时 count
不变的预期后失败项通过。覆盖限幅拐点/极限、相位约定、阻抗归一化、DC/多音
拒绝、频率溢出、C 输出原子性、Python/JSON 一致性、未知消息拒绝，以及改动
观测值会暴露差异。跨平台结果以对应提交 CI 为准。

独立安装后的 C/C++ 消费者 Debug/Release 各 2/2 通过，验证新头文件及 C 符号可用。
