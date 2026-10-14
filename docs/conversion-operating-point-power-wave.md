# 原生混合参考非线性工作点

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

C++17 新入口 `solve_conversion_operating_point_power_waves` 在各器件原有逐通道功率波坐标中求解固定非线性方程、物理接线和源/负载边界。它与[既有工作点接口](conversion-operating-point.md)使用相同类型、参数顺序和默认值：

```cpp
#include <rfmodel/conversion/conversion_operating_point.hpp>

rfmodel::ConversionOperatingResult
rfmodel::solve_conversion_operating_point_power_waves(
    const std::vector<rfmodel::ConversionDevice>& devices,
    const std::vector<rfmodel::ConversionPortConnection>& connections,
    const std::vector<rfmodel::ConversionNonlinearDevice>& nonlinear,
    const std::vector<rfmodel::Complex>& initial = {},
    const rfmodel::ConversionOperatingOptions& options = {},
    bool loaded_noise = false,
    const rfmodel::ConversionNoise* additional_source_noise = nullptr,
    const std::vector<rfmodel::Complex>& fixed_offset = {});
```

此声明用于说明签名；调用方只需包含头文件，不应重复声明默认参数。

## 原有坐标与回调契约

- 每个 `FrequencyConversionModel::reference_impedances_ohms()` 向量完整保留。所有参考必须有限且实部严格为正；DC 参考必须为正实数。同一端口的不同频点可采用不同参考。
- 各器件必须使用同一 `spacing_hz`；通道顺序仍为器件顺序、器件内的 `(port, bin)` 顺序。`initial`、`fixed_offset` 及全局额外源 C/P 均采用这个顺序和各通道局部坐标。
- `ConversionNonlinearDevice::evaluate(a)` 收到本地入射波，返回真实 `F(a)` 和该点的直接/共轭导数 `A/B`。每次回调返回的 Jacobian 必须具有原器件完全相同的完整参考向量、频率步长和有序 `(port, bin)` 列表；只保留首项参考或重排通道均不符合契约。
- 求解器不会隐式重归一化回调的输入、输出、导数或噪声。回调必须描述所声明坐标中的固定器件方程。现有仅接受标量实参考的局部模型适配器不会因此自动成为混合参考适配器。
- `source`、`reflection` 和源噪声已在该器件的局部坐标中；`intrinsic_noise` 与 `fixed_offset` 同样使用局部出射坐标。额外源噪声可保留跨器件相关性。噪声在本次求解中固定，回调不隐式生成工作点相关噪声。

## 边界、迭代与返回值

外部通道满足 `a_i = source_i + reflection_i*b_i`，器件满足 `b = F(a) + fixed_offset`。相连通道 i、j 的物理接线为：

```text
D = conj(Zi + Zj)
self_i = (conj(Zj) - Zi) / D
self_j = (conj(Zi) - Zj) / D
coupling = 2 sqrt(Re Zi Re Zj) / D
a_i = self_i*b_i + coupling*b_j
a_j = self_j*b_j + coupling*b_i
```

这是电压连续与电流守恒的功率波形式；公共实参考时退化为简单波交换。真实残差、Newton 仿射候选、回溯和收敛噪声都采用同一物理边界。已接线通道不能同时设置外部源、反射或源噪声；固定内部出射偏置和器件内生噪声仍可存在。更多坐标与 V/I 定义见[变频功率波参考](conversion-power-wave.md)。

阻尼 Newton、默认初值、容差、迭代上限和失败异常与既有接口一致。`waves.incident` 是最后接受的非线性点，`waves.outgoing` 是该点的真实 `F(a)+fixed_offset`；不会用最后一次仿射求解替换它们。`scaled_residual <= 1` 表示按给定容差收敛，`waves.relative_residual` 是真实边界残差。

`linearized_devices` 保存同一点、原坐标下的 Jacobian；`output_offset` 为 `F(a)-A*a-B*conj(a)+fixed_offset`。完整出射 C/P 及可选加载噪声使用该点的 Jacobian 和物理接线。新入口不改变一阶噪声、实 DC 或多解/动态稳定性的既有限制。

## 兼容边界与安装消费

`solve_conversion_operating_point` 继续要求所有器件采用同一公共正实参考，保持原有拒绝行为。C ABI、Python 和 JSON/CLI 自动工作点入口与显式向量限制均未改变；新入口只扩展原生 C++。它不增加 SystemVue 厂商兼容验收项。

[安装消费者](../tests/consumer/operating_power_wave.cpp)用复反射边界、非线性回调、混合复参考物理接线、固定偏置及额外源噪声调用全部参数，独立核对已知根、V/I 连续性和收敛 Jacobian 的 C/P。[第二翻译单元](../tests/consumer/operating_power_wave_probe.cpp)使用三个必需参数，覆盖默认参数及同端口实 DC/复 RF 参考；检查不依赖 `assert`，Release 下仍执行。

```sh
cmake -S tests/consumer -B build-op-consumer \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/absolute/path/rfmodel-sdk
cmake --build build-op-consumer --target operating_power_wave_consumer --config Release
ctest --test-dir build-op-consumer -C Release \
  -R '^installed_operating_power_wave_consumer$' --output-on-failure
```

消费者只链接安装后的 `RFModel::rfmodel`，无需 C ABI、Python 或 JSON。

完整独立物理基准、失败契约及实际构建范围见[本阶段验证记录](conversion-operating-point-power-wave-validation.md)。

## Accepted-state intrinsic noise

The [explicit intrinsic-noise provider entry](conversion-operating-noise.md) retains each channel’s full complex reference and freezes complete accepted-state C/P for measurements and noise experiments.
