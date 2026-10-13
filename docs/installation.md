# 安装与外部 CMake 接入

RFModel 的 C++17 核心为 header-only 库；默认还构建共享 C ABI 和命令行程序，Python 产品接口已退役，默认构建不查找 Python。包版本 0.1.0 用于工程依赖识别，不代表 v0.1 功能验收完成；v1.0 前不承诺兼容升级。

```sh
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build --config Release
cmake --install build --prefix /your/install/path --config Release
```

消费者配置 `CMAKE_PREFIX_PATH` 指向安装目录，使用：

```cmake
find_package(RFModel 0.1.0 EXACT CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE RFModel::rfmodel)
```

导出的目标传播头文件目录及 C++17 要求；消费者无需引用源码路径。`tests/consumer` 是独立消费者项目，可针对安装目录单独配置、构建和运行 CTest。

历史记录（2026-09-22）：当时 MSVC Release 核心回归 10/10，安装包消费者 1/1 通过，Linux/macOS 未验证。此记录不代表当前提交的验收状态，后续证据见 [构建记录](build-validation.md)。安装内容包括所有公共头文件和 Config/Version/Targets CMake 文件。


## 可选 C ABI

C 消费者链接 `RFModel::c_api`，Windows 部署所选配置的 DLL，Debug 名称带 d 后缀。C ABI 与 C++ 编排接口分别维护；本次 Python 退役保留已有 C 函数和原生 CSV CLI。新的仿真编排请使用 [C++ 类型 API](cpp-simulation-api.md)。

## C++ 优先的最小安装

仅需要 C++ 函数 API 时，可关闭 C ABI、CLI 和示例，只安装头文件与 CMake 包。精确命令和完整独立消费者见 [C++ SDK 指南](cpp-sdk.md)。`BUILD_TESTING=OFF` 现在覆盖全部测试目标；示例由 `RFMODEL_BUILD_EXAMPLES` 单独控制。
