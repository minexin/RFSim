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

仍需完成独立/复参考阻抗、相关噪声网络、非线性和变频求解、完整器件目录与 SystemVue 数值对照。本机 COM 激活探测返回 0x80080005，尚未运行参考仿真；不能将解析测试当作 SystemVue 差异测试。

[长期路线](docs/roadmap.md) · [兼容矩阵](docs/systemvue-2023-matrix.md) · [线性分析输出契约](docs/linear-analysis-compatibility.md)
