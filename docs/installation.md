# 安装与外部 CMake 接入

RFModel 当前为 header-only C++17 库。包版本 0.1.0 用于工程依赖识别，不代表 v0.1 功能验收完成；v1.0 前不承诺兼容升级。

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

2026-09-22：MSVC Release 核心回归 10/10，安装包消费者 1/1 通过。Linux/macOS 尚未实际验证。安装内容包括所有公共头文件和 Config/Version/Targets CMake 文件。
