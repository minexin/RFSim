# 构建与当前验证状态

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

MSVC x64 Debug 和 Release 均构建成功，CTest 各 13/13 通过。测试使用始终有效的检查函数，Release 不会因 NDEBUG 而跳过断言。

覆盖：静态模型维度、矩阵行列越界、匹配衰减器波幅/功率、非互易方向性、双端口同时激励及非二端口拒绝。

## 尚未验证

- Linux/macOS 构建尚未实际运行。
- 新增 network.hpp 已验证单频同参考阻抗的网络反馈求解，范围见 network.md；频率扫描已通过端到端回归（interpolation.md）；复杂参考阻抗尚未完成。
- Touchstone 已验证基础 RI/MA/DB 转换和单位换算，完整限制见 touchstone.md；插值已纳入扫描回归；标量噪声和 Friis 级联已纳入测试（noise.md），不代表完整网络噪声能力。
- SystemVue 2023 对照验证尚未执行，v0.1 和长期目标均未完成。
