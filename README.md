# RFModel / RFSim

面向仿真软件集成的 C++17 射频仿真库。最终调用入口为 C++ 函数和类型 API，核心为 header-only 的 `RFModel::rfmodel`。当前处于原型开发期，**尚未达到完整 SystemVue 2023 兼容目标**。

## 集成入口

- [代码目录与模块归属](docs/repository-layout.md)
- [C++ SDK 安装与调用](docs/cpp-sdk.md)：可独立安装、移动并消费，不依赖 Python 或 C ABI。
- [仿真编排 API](docs/cpp-simulation-api.md)：线性/频谱分析、相干系统图、扩频与变频工作点、相位噪声及测量、参数化优化和 Monte Carlo。
- [完整 C++ 安装消费者](tests/consumer/simulation_tasks.cpp)：直接组合相干图、频率规划、转换分析和参数任务。
- [Python 接口退役及功能迁移记录](docs/python-interface-retirement.md)：产品包、wheel、Python/JSON CLI 已移除；独立开发采集与参考工具保留。

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

默认构建 C++ 测试、示例、C ABI 与原生 S 数据 CSV CLI，不查找 Python。仅开发用归档参考重放通过 `RFMODEL_BUILD_REFERENCE_TESTS=ON` 显式启用 Python 脚本。Windows 可运行 `scripts/build-msvc.ps1`。

外部工程使用 `find_package(RFModel 0.1.0 EXACT CONFIG REQUIRED)` 并链接 `RFModel::rfmodel`。最小 SDK 的开关见[安装说明](docs/installation.md)。代码遵循四空格、多行定义、显式花括号；提交前执行 `scripts/format-cpp.ps1 -Check`。

## 主要能力与验证边界

- [Touchstone](docs/touchstone-two.md)、多端口网络、频率扫描、混合复参考阻抗、S/Z/Y 转换及噪声协方差。
- RLC、理想器件、[频率相关电缆](docs/frequency-dependent-cable.md)、滤波器、[链路噪声](docs/linear-path-noise.md)及功率后处理。
- [相干流管线](docs/coherent-pipeline.md)、来源历史、多项式及[限幅/高阶放大器](docs/coherent-pipeline-amplifiers.md)、失真传播与共同压缩。
- [变频功率波](docs/conversion-power-wave.md)、[复参考工作点](docs/conversion-operating-point-power-wave.md)、结构扩频、完整 C/P、加载噪声和相位噪声。
- [优化](docs/optimization.md)、[Monte Carlo](docs/monte-carlo.md)与新的有类型任务装配。
- [显式 AM/PM 参数适配器](docs/systemvue-am-pm-adapter.md)：C++ 相位、导数及工作点组合；549 个无告警登记点通过，另保留 37 个告警诊断点，完整兼容仍未验收。
- [DataPoints 零 dBm 判别](docs/systemvue-am-pm-datapoints-zero-study.md)：七组、387 个登记扫描点支持零位置异常；DCP 同功率异相位反例保留，完整表格算法尚未闭合。
- [DataPoints 普通原始功率轴](docs/systemvue-am-pm-datapoints-raw-axis-study.md)：十个新点中原始功率样条通过九点；−0.000001 dBm 零相移反例否定“仅名义零旁路”，四个冻结候选均未全部通过。
- [DataPoints 顺序复现](docs/systemvue-am-pm-datapoints-order-study.md)：两个相反顺序下五个普通目标均复现；±0.00001 dBm 两个新增目标通过原始功率样条，但近零旁路规则与完整兼容仍未闭合。
- [DataPoints 增益与源功率分配](docs/systemvue-am-pm-datapoints-allocation-study.md)：改变增益后，输出离散模式通过八个新配置，输入离散模式仅通过两个；有限结果未确定内部坐标、旁路边界或产品导数契约。

工程回归和独立物理参考不等于厂商验收。已有 SystemVue 采集中的失败、缺项与 diagnostic-only 结论继续保留；本次接口迁移不改变其判定。旧文档中的 Python/JSON 示例属于历史接口，当前集成请使用上述 C++ 指南。

[长期路线](docs/roadmap.md) · [兼容矩阵](docs/systemvue-2023-matrix.md) · [RF Design 参数目录](docs/rf-design-catalog.md) · [历史构建记录](docs/build-validation.md) · [原生归档参考重放](docs/native-reference-replay.md)
