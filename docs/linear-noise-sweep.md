# 线性噪声扫描

`analyze_linear(grid, external_ports, build, intrinsic_noise)` 的第四参数可选，为 `NoiseCorrelation(double frequency_hz, const LinearNetwork&)` 回调。每个频点先构建一次网络，再将同一网络以 const 引用传给噪声回调；回调提供该网络全部端口的本征出射噪声。

返回的 `LinearAnalysisResult::noise_correlation` 在请求噪声时包含 M 个 N×N 矩阵，M 是频点数、N 是外端口数。矩阵的行列顺序与 scattering 和 external_ports 一致，单位 W/Hz。未提供回调时此数组为空，表示未计算，而非零噪声。回调或求解失败会抛出包含频率的异常，不返回不完整结果。

`independent_noise({device1_noise, device2_noise, ...})` 按器件插入网络的顺序组装块对角矩阵。它检查每块的尺寸与半正定性，拒绝空列表和超过 1024 的总端口数。只适用于器件之间不相关的噪声；跨器件相关源仍须显式填写全局矩阵。无噪声器件也须提供相应阶数的零矩阵以保持端口编号对齐。

```cpp
auto result = rfmodel::analyze_linear(grid, ports, build_network,
    [&](double f, const rfmodel::LinearNetwork&) {
        return rfmodel::independent_noise({
            rfmodel::passive_thermal_noise(device1.s_parameters(f), 290.),
            rfmodel::passive_thermal_noise(device2.s_parameters(f), 580.)
        });
    });
```

回归覆盖不同物理温度的两级衰减器、频变插损、反序外端口、解析输出噪声、一次构建/频点、无噪声请求的空字段，以及中途回调错误的频率上下文。这里的 W/Hz 定义尚未与 SystemVue 2023 CS 数据归一化核对，不能宣称 CS 数值兼容；NF/NFmin/Rn/GammaOpt 可通过 noise-figure.md 所述二端口后处理接口计算。
