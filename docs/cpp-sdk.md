# C++ 函数 API 与独立 SDK

最终集成以 C++17 函数/类型 API 为主。数值核心是 header-only 的 `RFModel::rfmodel`，不需要 Python、C ABI 动态库或 JSON 解析器。CMake 3.20+ 负责安装和导出包含目录、语言要求。

## 最小 SDK：只安装头文件与 CMake 包

```sh
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DRFMODEL_BUILD_C_API=OFF \
  -DRFMODEL_BUILD_CLI=OFF -DRFMODEL_BUILD_EXAMPLES=OFF \
  -DRFMODEL_BUILD_PYTHON_TESTS=OFF
cmake --build build-sdk --config Release
cmake --install build-sdk --config Release --prefix /absolute/path/rfmodel-sdk
```

该配置仅启用 C++ 工具链，不查找 Python，也不生成任何测试、示例或共享库目标。header-only 安装无需先生成原生库。Windows 下可用同样的选项，安装路径改为本机绝对路径。

独立工程的 CMakeLists.txt：

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyRFApplication LANGUAGES CXX)
find_package(RFModel 0.1.0 EXACT CONFIG REQUIRED)
add_executable(my_rf_application main.cpp)
target_link_libraries(my_rf_application PRIVATE RFModel::rfmodel)
```

配置时设置 `-DCMAKE_PREFIX_PATH=/absolute/path/rfmodel-sdk`。不应添加 RFModel 源码目录作为备用 include 路径；这样才能验证使用的确实是安装包。

## 最小函数调用

```cpp
#include <rfmodel/conversion_network.hpp>
#include <rfmodel/conversion_power_wave.hpp>
#include <iostream>

int main() {
    using namespace rfmodel;
    // 固定泵的小信号关系 b = 0.2*a + 0.1*conj(a)，原坐标为50欧姆。
    const FrequencyConversionModel original(
        1e9, {{0, 1}}, {1, {.2}}, {1, {.1}});
    const std::vector<Complex> references{{75., 10.}};
    const auto changed = renormalize_power_waves(original, references);
    const auto boundary = renormalize_conversion_boundary(
        original, references, {0}, {Complex{.01, .02}}, {0.}, original.zero_noise());
    FrequencyConversionNetwork network(1e9);
    network.add_power_waves({changed.model, boundary.source, boundary.reflection,
                            boundary.source_noise, original.zero_noise()});
    const auto result = network.analyze(true);
    std::cout << result.incident[0] << ' ' << result.outgoing[0] << '\n';
}
```

完整可编译、自带物理断言的调用示例为 [conversion_api.cpp](../tests/consumer/conversion_api.cpp)：固定泵混频器 → 热电阻，多通道不同复参考、物理源/负载变换、完整加载 C/P、V/I 和显式噪声系数实验。它只链接安装后的 `RFModel::rfmodel`。

```sh
cmake -S tests/consumer -B build-sdk-consumer \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/absolute/path/rfmodel-sdk
cmake --build build-sdk-consumer --config Release
ctest --test-dir build-sdk-consumer -C Release --output-on-failure
```

仅安装 C++ SDK 时执行两个独立 C++ 消费者，不创建 C 消费者。数值错误按现有 C++ API 抛出异常；调用方应在自己的边界处理 `std::exception`。通道按器件加入顺序及器件内通道顺序排列；参考阻抗、单位和 C/P 契约见[变频功率波参考](conversion-power-wave.md)。

## 无 Python 的完整原生回归

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DRFMODEL_BUILD_C_API=OFF \
  -DRFMODEL_BUILD_CLI=OFF -DRFMODEL_BUILD_EXAMPLES=ON \
  -DRFMODEL_BUILD_PYTHON_TESTS=OFF
cmake --build build-native --config Debug
ctest --test-dir build-native -C Debug --output-on-failure
```

- `BUILD_TESTING=OFF` 不再留下未受控的原生测试可执行文件。
- `RFMODEL_BUILD_EXAMPLES` 独立控制四个现有 C++ 示例，默认 ON；测试关闭时仍可单独构建示例。
- `RFMODEL_BUILD_PYTHON_TESTS=OFF` 明确禁止查找 Python/注册脚本测试，即使保留 C ABI。旧 `RFMODEL_REQUIRE_PYTHON_TESTS=OFF` 只表示“不强制存在”，并不等于关闭。
- `RFMODEL_REQUIRE_PYTHON_TESTS=ON` 与关闭测试/C ABI/Python 测试互斥；配置直接报错，不静默跳过要求。

## 常用入口与调用责任

- `network.hpp`：`LinearNetwork`，连接及等效 S；`frequency_conversion.hpp` / `conversion_network.hpp`：`FrequencyConversionModel`、`FrequencyConversionNetwork` 和完整 C/P 结果。
- `conversion_power_wave.hpp`：模型、仿射偏置、内生噪声和源/负载边界的功率波参考变换。仅声明新参考不会自动保留旧物理边界。
- `conversion_operating_point.hpp`：`solve_conversion_operating_point`，收敛后的线性化器件及偏置可用于后续噪声分析；混合复参考自动工作点尚未支持。
- `optimization.hpp`：`minimize_bounded`；`monte_carlo.hpp`：`run_monte_carlo`。完整无 Python 例子是 [LC 匹配](../examples/optimize_lc_match.cpp)和[容差分析](../examples/monte_carlo_resistor.cpp)。

频率 Hz、阻抗 Ω、功率 W、噪声 C/P 为 W/Hz；RF 采用 RMS 功率波，幅度为 sqrt(W)，DC 保留实自由度。`SMatrix.values` 按行优先存储。dBm 是显示或明示参数单位，不能与 W 混用。

优化/Monte Carlo 的求值回调由调用者同步执行网络构造、参数赋值和结果投影，应返回规定维数的有限实数，并自行管理捕获对象的生命周期与外部状态。没有隐含 Python 字段选择器或自动并行；确定性重放还要求回调本身确定。取消在评估之间检查，不中断正在执行的回调。不要只看是否抛异常判断成功，还须检查返回的停止原因、有效样本/有限点状态及失败记录。详细异常、失败与取消约定见[优化](optimization.md)及[Monte Carlo](monte-carlo.md)。

安装消费者还把同一转换 API 放在两个独立 translation unit 中编译链接，检查 header-only 的 ODR/linkage；安装目录整体移动后再通过新前缀消费，避免依赖原安装位置。

## 数值功能与 Python 装配边界

优化和 Monte Carlo 的搜索、抽样、相关分布、预算、统计及回调算法已有原生 C++ API。Python 中的声明式任务字段、JSON 参数选择器、单位/目标装配及 CLI 入口是另外一层，删除它们不等于删除核心算法，也不能声称 C++ 已有等价文件入口。

相干系统图还存在 Python 端的多级来源身份注册、共同驱动与历史组合逻辑。原生内核已有，但完整编排尚未逐项迁移。Python 包的分阶段退出条件见[依赖审计与迁移计划](python-retirement-plan.md)。本阶段不删除 Python 辅助脚本、SystemVue 对照脚本或参考数据，也不为保留 CLI 阻塞 C++ SDK 的使用。

本轮 Linux Debug/Release 验证、安装重定位、双翻译单元及逐头编译证据见[机读记录](../validation/cpp-sdk-priority-validation-20261013.json)。独立三平台两配置 CI 已编写但尚未远程执行，不能据此声称 Windows/macOS 通过。

构建边界：本轮验证默认本机工具链。SDK 配置回归的嵌套工程暂不转发自定义生成器平台、工具集或交叉编译 toolchain；这些配置需要另行验证，不属于当前三平台默认 CI 配置的通过声明。
