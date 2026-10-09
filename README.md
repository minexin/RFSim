# RFModel / RFSim

面向 SystemVue 2023 Linear Analysis、RF System Analysis 和 RF Design 器件能力的 C++17 射频仿真工程。当前处于原型开发期，**尚未达到完整兼容目标**。

## 已有实现

- Touchstone legacy S 数据、RI/MA/DB 转换、频率插值及表格器件接口。
- 同一正实数参考阻抗下的多端口连接、反射反馈、等效 S 提取和频率扫描。
- 匹配衰减/延迟模型、功率与阻抗后处理、标量 Friis 级联噪声。
- S 数据 CSV 命令行导出与可安装 CMake 包。

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Windows 本机可运行 `scripts/build-msvc.ps1` 自动定位已有 VS 工具链。外部工程通过安装包中的 `RFModel::rfmodel` 接入，见 [安装说明](docs/installation.md)。

代码统一使用多行定义、4 空格缩进和显式控制流程花括号。提交前运行 `scripts/format-cpp.ps1 -Check`；批量整理可运行 `scripts/format-cpp.ps1`，详见 [代码风格](docs/code-style.md)。

## 验证与剩余工作

[三平台 CI](https://github.com/minexin/RFSim/actions/runs/35746795541) 已验证 Windows/MSVC、Ubuntu/GCC、macOS/AppleClang 的 Debug/Release，六组均通过 14 个核心/CLI 测试与 1 个安装消费者测试。证据绑定提交 `7fba1bf`，见 [验证报告](docs/build-validation.md)。

已运行 SystemVue 2023 衰减器的损耗、温度和源功率对照，噪声密度与极小衰减边界仍有差异，见 [实测报告](docs/systemvue-attenuator-power.md)。相关噪声协方差、双向线性链路和基础非线性/混频能力已进入核心；这些实现不代表完整 RF Design 库兼容。仍需完成独立/复参考阻抗、完整器件目录与更广泛的 SystemVue 数值验证。

新增 [链路噪声接口](docs/linear-path-noise.md) 支持源/负载失配、器件内相关噪声和源噪声/器件噪声分解。

[多音放大器接口](docs/multitone-amplifier.md) 已提供总 RF 功率限幅以及直接、二阶、
三阶分谱结果，贯通 C++/C/Python/JSON；等功率双音的指定功率点已对照，完整
重叠谱合并、级联、相位和噪声语义仍待验证。

[长期路线](docs/roadmap.md) · [兼容矩阵](docs/systemvue-2023-matrix.md) · [线性分析输出契约](docs/linear-analysis-compatibility.md)

[同频多源非线性放大器](docs/coherent-amplifier.md) 已接入 C++/C/Python 与系统图；
21 组 SystemVue 记录的 206 个来源谱项通过比较，递归多级非线性仍待完成。

[失真来源级联](docs/coherent-amplifier-cascade.md) 新增 cascaded_amplifier：传播前级谐波/互调，计入共同驱动，并与本级同源失真相干合并。八组 SystemVue 对照中六组通过，两组 0 dB 增益差异保留；次级失真再混频仍未完成。
