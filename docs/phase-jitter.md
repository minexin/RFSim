# 有限频偏相位方差与 RMS 相位/时间抖动

本接口是独立、header-only C++17 计算：对用户给定的一侧相位 PSD、显式频偏区间和插值策略积分。它不声称测得真实器件的全部抖动，也不构成 SystemVue PNCP 或厂商 jitter 验收。C ABI、既有请求布局、PLL 传递函数和波噪声接口不变。

## 物理量与显式输入

头文件 `rfmodel/noise/phase_jitter.hpp`：

```cpp
#include <rfmodel/noise/phase_jitter.hpp>

rfmodel::PhaseJitterRequest request{
    {{1e3, 1e-12}, {1e6, 1e-12}}, // 正频偏 Hz，一侧 S_phi rad^2/Hz
    1e8,                         // 明确的物理载频 Hz
    1e3, 1e6,                    // 完整积分区间 Hz
    rfmodel::PhaseJitterIntegrationPolicy::linear_rad2_per_hz};
auto result = rfmodel::measure_phase_jitter(request);
// variance = 9.99e-7 rad^2
// rms phase = 0.0009994998749374609101 rad
// rms time = 1.5907534571602809633e-12 s
```

`PhaseNoiseDensitySample` 字段为 `offset_hz`、`phase_psd_rad2_per_hz`。这是实相位过程的一侧 PSD，不是 `NoiseDensitySample` 的 W/Hz，也不从 AM/PM 混合波噪声对角元、carrier bin 或 PLL 反馈分频比推断相位谱/物理载频。调用者负责提供正确单位和纯相位统计量。

定义：

```text
phase_variance_rad2 = integral(S_phi(f), lower_offset_hz .. upper_offset_hz)
rms_phase_rad = sqrt(phase_variance_rad2)
rms_time_s = rms_phase_rad / (2*pi*carrier_hz)
```

`PhaseJitterRequest` 按值拥有 samples；构造必须明确提供 carrier、上下界和策略，没有默认积分带宽。结果 `PhaseJitterMeasurement` 按值拥有上述三量、carrier、实际上下界、策略和 `interpolation_intervals`。区间数仅计算与请求带宽有非空交集的源区间，包括零 PSD 区间。结果不保留请求引用；请求复制、原数据销毁和后续输入改变均不会改变已返回结果。失败不发布部分结果，赋值调用失败时已有结果保持原值。

## 插值、覆盖与采样误差

只支持显式 `linear_rad2_per_hz`：在实际 Hz 轴上线性插值线性 rad²/Hz，并以梯形法积分这一插值函数。剪裁在给定上下界；不得外推。log-spaced grid 可以输入，但不是 log-log 插值，也不在 dB 域直接积分。

- 所有样本（包括带外样本）都必须合法；至少 2 点，最多 1,000,000 点。
- 频偏为正、有限且严格递增；PSD 有限且非负。允许线性零和次正规输入值。
- carrier 为正且有限；上下界满足有限的 `0 < lower < upper`。
- 整个请求带宽必须由样本覆盖；不自动排序、合并重复点、延拓到 DC/无穷或设置 Nyquist/2fc 默认值。
- 未知策略、NaN/Inf、重复/倒序频偏、负 PSD、非法 carrier/band/count 抛 `std::invalid_argument`；覆盖不足抛 `std::out_of_range`。
- 这是相位过程积分，不强加 upper < carrier。后续波边带映射需自行满足它的正边带边界条件。

例如 `S_phi=2+3f`，积分 [2,7] 的面积为 77.5。对于 [1,10,100] 上取样的 1/f 与 1/f²，线性插值面积分别是 9.9 与 4.9995，而不是连续幂律的 ln(100) 与 0.99。积分精度和采样误差是两回事；稀疏点可能漏掉窄带峰谷。应自行加密并检查收敛。本接口不自动加点、自适应积分或生成新的 PLL/网络求解点。

## SSB 的可选显式转换

`ssb_dbc_per_hz_to_phase_psd_rad2_per_hz(L)` 单独实现常规小角度纯 PM 近似：`S_phi = 2*10^(L/10)`。例如 -120 dBc/Hz 对应 2e-12 rad²/Hz。它不把任意总波噪声解释成相位噪声；若要先转换一张 SSB 表，转换后再按线性 PSD 插值。零 PSD 请直接用线性值 0，而不是 -Inf dBc/Hz。

输入必须有限；转换后的正值必须为有限正规 binary64，范围不满足时抛 `std::overflow_error`。准入以计算后舍入的 binary64 值为准，不宣称任意精度阈值判断。实现用分解的二进制指数避免大自然对数指数在正规边界附近的消减损失。

## 数值范围与限制

生产计算只要求标准 double，不依赖较宽 long double。正数二进制缩放用于插值权重、端点均值、面积及时间换算；总面积采用缩放 Neumaier 补偿累加。不先形成 `2*pi*carrier`，避免中间上溢导致本可表示的最终时间被拒。可恢复的次正规内部面积/PSD 不设人为截断；远小于主项精度的贡献仍服从浮点舍入。

非零返回 variance、phase RMS、time RMS 必须为有限正规 binary64；最终非零下溢（含次正规结果）或上溢抛 `std::overflow_error`，不悄悄归零、钳到阈值或返回 Inf。即使平方根或时间可能可表示，最终 variance 非正规仍按本合同拒绝。全零谱在完整验证之后精确返回三个零，并保留区间计数和元数据。

大方差可以数值合法，但不证明小角度 PM 近似仍物理有效。本结果仅是指定有限频带的 RMS phase/time 指标，不是 full-band、period、cycle-to-cycle、peak-to-peak 或 differential jitter。共享源差分指标需要完整复互谱及明确载频归一化，本阶段不实现。

## 验证与来源

解析测试包括白相位谱、线性剪裁、分区可加性、线性等价加点、载频/PSD 缩放、log-grid 区别和幂律加密收敛；还覆盖未知策略、全表验证、计数预算、nextafter 边界、极端可恢复中间值、总面积溢出、最终下溢、零谱和 owned 请求/结果。安装后独立双 translation unit consumer 同时使用模块路径和 flat compatibility header，检查 ODR 与运行结果。focused 检查不是全门禁，最终集成验证需绑定最终提交另记。

物理换算参考：
- [Analog Devices MT-008, pp.3–5](https://www.analog.com/media/en/training-seminars/tutorials/mt-008.pdf)：指定区间的线性相噪积分、SSB 因子二及除以 2π载频的时间换算。其 ADC 应用带宽建议不是本接口默认值。
- [Analog Devices: Clock (CLK) Jitter and Phase Noise Conversion](https://www.analog.com/en/resources/technical-articles/clock-clk-jitter-and-phase-noise-conversion.html)：phase/TIE、period 与 cycle-to-cycle 指标的区别。

插值选择、正规数结果范围和测试 fixture 是 RFModel 的明确合同，非厂商行为复制；不改写既有厂商证据或验收阈值。
