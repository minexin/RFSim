# C++ 函数 API 与独立 SDK

最终集成以 C++17 函数/类型 API 为主。数值核心是 header-only 的 `RFModel::rfmodel`，不需要 Python、C ABI 动态库或 JSON 解析器。CMake 3.20+ 负责安装和导出包含目录、语言要求。

## 最小 SDK：只安装头文件与 CMake 包

```sh
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DRFMODEL_BUILD_C_API=OFF \
  -DRFMODEL_BUILD_CLI=OFF -DRFMODEL_BUILD_EXAMPLES=OFF
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

仅安装 C++ SDK 时执行独立 C++ 消费者，不创建 C 消费者。数值错误按现有 C++ API 抛出异常；调用方应在自己的边界处理 `std::exception`。通道按器件加入顺序及器件内通道顺序排列；参考阻抗、单位和 C/P 契约见[变频功率波参考](conversion-power-wave.md)。

## 无 Python 的完整原生回归

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DRFMODEL_BUILD_C_API=OFF \
  -DRFMODEL_BUILD_CLI=OFF -DRFMODEL_BUILD_EXAMPLES=ON
cmake --build build-native --config Debug
ctest --test-dir build-native -C Debug --output-on-failure
```

- `BUILD_TESTING=OFF` 不再留下未受控的原生测试可执行文件。
- `RFMODEL_BUILD_EXAMPLES` 独立控制六个 C++ 示例，默认 ON；测试关闭时仍可单独构建示例。
- 默认不查找 Python。只有显式 `RFMODEL_BUILD_REFERENCE_TESTS=ON` 的开发重放依赖 Python。
- 旧 Python 开关 OFF 兼容现有脚本；显式要求 ON 将报退役错误，避免误报已验证绑定。

## 常用入口与调用责任

- `coherent_pipeline.hpp`：`CoherentPipeline` 的不可变有类型相干流、混频/线性/多项式/共同驱动压缩，以及[限幅/高阶放大器与失真传播](coherent-pipeline-amplifiers.md)；[调用、历史与固定边界](coherent-pipeline.md)。
- `frequency_dependent_cable.hpp`：显式参数的[频率相关同轴电缆](frequency-dependent-cable.md)，提供传播损耗与完整复 S 函数、器件包装和扫描示例；仅为 RFModel 定义的有限频率近似。
- `network.hpp`：`LinearNetwork`，连接及等效 S；`frequency_conversion.hpp` / `conversion_network.hpp`：`FrequencyConversionModel`、`FrequencyConversionNetwork` 和完整 C/P 结果。
- `conversion_power_wave.hpp`：模型、仿射偏置、内生噪声和源/负载边界的功率波参考变换。仅声明新参考不会自动保留旧物理边界。
- `conversion_operating_point.hpp`：`solve_conversion_operating_point` 保留公共实参考契约；[solve_conversion_operating_point_power_waves](conversion-operating-point-power-wave.md) 支持逐通道复参考。两者均返回收敛点的波、原坐标 Jacobian、偏置和噪声。
- `optimization.hpp`：`minimize_bounded`；`monte_carlo.hpp`：`run_monte_carlo`。完整无 Python 例子是 [LC 匹配](../examples/optimize_lc_match.cpp)和[容差分析](../examples/monte_carlo_resistor.cpp)。

频率 Hz、阻抗 Ω、功率 W、噪声 C/P 为 W/Hz；RF 采用 RMS 功率波，幅度为 sqrt(W)，DC 保留实自由度。`SMatrix.values` 按行优先存储。dBm 是显示或明示参数单位，不能与 W 混用。

优化/Monte Carlo 的求值回调由调用者同步执行网络构造、参数赋值和结果投影，应返回规定维数的有限实数，并自行管理捕获对象的生命周期与外部状态。没有隐含 Python 字段选择器或自动并行；确定性重放还要求回调本身确定。取消在评估之间检查，不中断正在执行的回调。不要只看是否抛异常判断成功，还须检查返回的停止原因、有效样本/有限点状态及失败记录。详细异常、失败与取消约定见[优化](optimization.md)及[Monte Carlo](monte-carlo.md)。

转换 API 和相干流管线安装消费者各在两个独立 translation unit 中编译链接，检查 header-only 的 ODR/linkage；后者在两处调用限幅/高阶模型及传播模式，Release 检查不受 NDEBUG 影响；安装目录整体移动后再通过新前缀消费，避免依赖原安装位置。

混合参考自动工作点另有双翻译单元 `operating_power_wave_consumer`，从安装头文件检查全部参数、默认调用、物理连线及接受点导数噪声，详见[调用契约与命令](conversion-operating-point-power-wave.md)。

## 完整有类型仿真任务

新入口 `CoherentSystem`、`ConversionGrid`、`analyze_conversion_simulation`、`run_optimization_task` 和 `run_monte_carlo_task` 已迁移原 Python 图、频率映射及任务装配职责。旧 JSON 文件分发不再交付。详细类型、所有权、回调、单位和预算见 [仿真编排 API](cpp-simulation-api.md)。

[simulation_tasks_consumer](../tests/consumer/simulation_tasks.cpp) 在两个翻译单元中从安装后的 SDK 调用全部新增入口。验证结果见 [接口退役记录](python-interface-retirement.md)，历史跨平台记录不替代本次验收。
