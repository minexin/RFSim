# 构建与当前验证状态

最新跨平台记录：提交 `d312947299cfca10e948c76d0230f711685a3432` 的 [CI 35802872861](https://github.com/minexin/RFSim/actions/runs/35802872861) 在 Windows/MSVC 19.51、Ubuntu/GCC 13.3、macOS/AppleClang 21 的 Debug/Release 六个配置均通过核心测试 24/24、安装后消费者测试 1/1。机器可读摘要与原始测试结论行见 `validation/cross-platform-d312947.json`。消费者测试仅调用基本线性分析，不代表所有新增噪声 API 的安装后行为已覆盖。下方较早的 14 项记录保留为历史证据。

本机已确认安装 Visual Studio 2022 Build Tools，MSVC 19.44.35228（工具目录 14.44.35207），Windows SDK 10.0.26100.0，以及 Visual Studio 自带的 CMake/CTest。它们没有加入当前 PowerShell 的 PATH；此前“没有可用工具链”的判断不准确。

Windows 可复现命令（需要能访问 SDK 和工具链的执行环境）：

```powershell
./scripts/build-msvc.ps1 -Configuration Debug
./scripts/build-msvc.ps1 -Configuration Release
```

其他已配置 C++17 工具链的平台可使用：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## 已执行验证

MSVC x64 Debug 和 Release 均构建成功，CTest 各 28/28 通过。测试使用始终有效的检查函数，Release 不会因 NDEBUG 而跳过断言。

覆盖：静态模型维度、矩阵行列越界、匹配衰减器波幅/功率、非互易方向性、双端口同时激励及非二端口拒绝。

## 尚未验证

- 三平台运行已验证，见下方远端 CI 记录；范围限于所列提交与现有测试。
- 新增 network.hpp 已验证单频同参考阻抗的网络反馈求解，范围见 network.md；频率扫描已通过端到端回归（interpolation.md）；复杂参考阻抗尚未完成。
- Touchstone 已验证基础 RI/MA/DB 转换和单位换算，完整限制见 touchstone.md；插值已纳入扫描回归；标量噪声和 Friis 级联已纳入测试（noise.md），不代表完整网络噪声能力。
- SystemVue 2023 对照验证尚未执行，v0.1 和长期目标均未完成。

## 跨平台 CI 实证

[运行 35746795541](https://github.com/minexin/RFSim/actions/runs/35746795541)，提交 `7fba1bf137025282668157a26625780ec1865e46`，在 2026-09-23 核对完整日志：

| 平台 | 编译器 | 配置 | 核心与 CLI | 安装后消费者 |
|---|---|---|---|---|
| Ubuntu | GCC 13.3 | Debug / Release | 各 14/14 | 各 1/1 |
| macOS | AppleClang 21 | Debug / Release | 各 14/14 | 各 1/1 |
| Windows | MSVC 19.51 | Debug / Release | 各 14/14 | 各 1/1 |

机器可读证据摘要：`validation/cross-platform-7fba1bf.json`。CI 证明已有能力在这些编译器上通过回归，不代表完成 SystemVue 功能兼容。官方 hosted runner 的 latest 标签会变化，具体编译器以上述运行日志为准。

噪声参数导入阶段：本机 MSVC Debug/Release 核心各 25/25，扩展后的独立安装包消费者各 1/1。消费者新增相关噪声扫描、NFmin/GammaOpt/Rn 提取、参数重建矩阵及 NF 校验；该新增覆盖尚不包含上方历史 CI 记录。

代码可读性整理后：46 个 C++ 文件通过 clang-format 19.1.5 格式检查；本机 MSVC Debug/Release 核心各 26/26，独立安装包消费者各 1/1 通过。格式规则和复现命令见 code-style.md。
