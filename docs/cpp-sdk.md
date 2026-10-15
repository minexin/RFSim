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
#include <rfmodel/conversion/conversion_network.hpp>
#include <rfmodel/conversion/conversion_power_wave.hpp>
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
- `optimization.hpp`：`minimize_bounded`；`monte_carlo.hpp`：`run_monte_carlo`。完整无 Python 例子是 [LC 匹配](../examples/analysis/optimize_lc_match.cpp)和[容差分析](../examples/analysis/monte_carlo_resistor.cpp)。

频率 Hz、阻抗 Ω、功率 W、噪声 C/P 为 W/Hz；RF 采用 RMS 功率波，幅度为 sqrt(W)，DC 保留实自由度。`SMatrix.values` 按行优先存储。dBm 是显示或明示参数单位，不能与 W 混用。

优化/Monte Carlo 的求值回调由调用者同步执行网络构造、参数赋值和结果投影，应返回规定维数的有限实数，并自行管理捕获对象的生命周期与外部状态。没有隐含 Python 字段选择器或自动并行；确定性重放还要求回调本身确定。取消在评估之间检查，不中断正在执行的回调。不要只看是否抛异常判断成功，还须检查返回的停止原因、有效样本/有限点状态及失败记录。详细异常、失败与取消约定见[优化](optimization.md)及[Monte Carlo](monte-carlo.md)。

转换 API 和相干流管线安装消费者各在两个独立 translation unit 中编译链接，检查 header-only 的 ODR/linkage；后者在两处调用限幅/高阶模型及传播模式，Release 检查不受 NDEBUG 影响；安装目录整体移动后再通过新前缀消费，避免依赖原安装位置。

混合参考自动工作点另有双翻译单元 `operating_power_wave_consumer`，从安装头文件检查全部参数、默认调用、物理连线及接受点导数噪声，详见[调用契约与命令](conversion-operating-point-power-wave.md)。

## 完整有类型仿真任务

新入口 `CoherentSystem`、`ConversionGrid`、`analyze_conversion_simulation`、`run_optimization_task` 和 `run_monte_carlo_task` 已迁移原 Python 图、频率映射及任务装配职责。旧 JSON 文件分发不再交付。详细类型、所有权、回调、单位和预算见 [仿真编排 API](cpp-simulation-api.md)。

[simulation_tasks_consumer](../tests/consumer/simulation_tasks.cpp) 在两个翻译单元中从安装后的 SDK 调用全部新增入口。验证结果见 [接口退役记录](python-interface-retirement.md)，历史跨平台记录不替代本次验收。

公共头文件已按模块划分。新增调用使用 `<rfmodel/模块/文件.hpp>`；旧扁平路径由构建/安装阶段生成的兼容头继续支持，见[代码目录说明](repository-layout.md)。

## 内部工程 SDK 归档

当前归档是内部工程候选，**不是公开发布的 SDK**；分发许可证仍待决定，
本阶段不添加许可证授权，不创建 release/tag，也不代表完整 SystemVue 兼容验收。

在干净的 Git 工作区中使用 CMake 3.20+、Git 和本机 C++17 编译器打包（不支持交叉编译环境生产归档；提取后的纯头文件可供交叉编译消费者使用）：

```sh
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF \
  -DRFMODEL_BUILD_EXAMPLES=OFF -DCMAKE_INSTALL_LIBDIR=lib \
  -DRFMODEL_ENABLE_SDK_PACKAGING=ON
cmake -DCONFIG=Release -P build-sdk/PackageSDK.cmake
```

该选项默认为 OFF，不增加默认构建目标。打包脚本从已核对的源码在私有暂存目录
重新配置纯 SDK 的安装树，避免旧构建目录的导出文件或安装脚本污染归档。
产物位于 `build-sdk/sdk-artifacts/`，名称为
`RFModel-0.1.0-sdk-headers-g<实际提交前12位>.zip`，附 `.zip.sha256`。
归档只有一个根目录，按白名单包含公共头文件、兼容包装头、CMake 包、
自包含双翻译单元示例、独立 README、校验脚本及 `manifest.json`。
不包含 C ABI 实现、CLI、Python、SystemVue 采集/工程、参考归档或构建路径。
`api/c_api.h` 仍属于公开头文件清单；它不表示该归档提供 C ABI 二进制。

manifest 记录实际完整 Git HEAD、干净状态、提交时间、生成时间、生产工具版本，
以及排序后的负载路径、字节数和 SHA-256。manifest 不包含自身哈希；ZIP 旁的
SHA-256 覆盖整个归档。哈希用于完整性检查，不是发布者签名。
不同平台或配置的 ZIP 不承诺逐字节一致；CI 工程产物按平台/配置区分。

```sh
cmake -DARCHIVE="/absolute/path/RFModel-0.1.0-sdk-headers-g<commit>.zip" \
  -DDESTINATION="/absolute/path/new SDK extraction" \
  -DVERIFIER="/absolute/path/build-sdk/VerifySDK.cmake" \
  -P build-sdk/ExtractSDK.cmake
```

解压脚本先检查旁文件哈希、条目路径、类型和规范清单，再解压到尚不存在的目录。
压缩包限 32 MiB，单文件限 8 MiB，解压总量限 64 MiB，条目限 512 个；路径和清单长度亦有界。
这些完整性及路径检查不是通用恶意归档沙箱，仍需可信发布者和分发渠道。
然后用构建时的独立白名单验证完整清单及负载哈希。归档中的 `verify-sdk.cmake`
也可用于解压后的自检。提取后将 `RFModel_DIR` 显式指向
`<SDK>/lib/cmake/RFModel`，运行 `<SDK>/examples/sdk-consumer`。
双翻译单元示例通过函数返回拥有数据的 `NetworkWaves`：构造 6 dB 匹配衰减器和
50 Ω、1 ns 无损线，连接并扫描三个频点，检查输出复波、负载功率与增益；
先捕获负频率异常，再继续有效扫描。RMS 功率波单位为 sqrt(W)，Release 检查仍有效。
纯头文件 SDK 允许目标指针大小不同；完整 C ABI 包仍保留架构匹配检查。
版本仍采用 `0.1.0 EXACT`，不接受错误版本。

本地修改开发模式需显式添加 `-DALLOW_DIRTY_DEVELOPER=ON`；名称带
`-developer`，manifest 将 `source_commit` 设为 null，记录 observed HEAD 和 dirty，
不把未提交内容冒充精确提交候选。打包前源码变化需要重新配置，已有同名产物不覆盖。
测试使用独立临时 Git fixture，身份仅用于 fixture，不修改用户 Git 配置。

## 基础数值 API 的失败契约

以下收紧只影响畸形输入或非有限结果，函数类型和 C ABI 保持不变：

- `SMatrix::index` 及 const/non-const `operator()` 对端口越界、索引乘加回绕抛出
  `std::out_of_range`；`operator()` 也对存储不足抛出该异常。`index` 仅计算索引，
  不验证存储长度。没有新增端口数上限。
- `evaluate_two_port` 要求 `ports == 2`、恰好四个有限复数 S 元素，且两个入射波的
  实部、虚部均有限；违反这些条件抛出 `std::invalid_argument`。有限输入计算出非有限
  出射波时抛出 `std::overflow_error`。主动增益、非互易 S、复激励和零激励仍合法，
  不新增无源性或互易性限制，也不改变原计算公式。
- `interpolate_s` 仅接受 `OutOfBand::Reject` 和 `OutOfBand::Clamp`；非法枚举值在
  带内、端点、单样本及带外查询中均抛出 `std::invalid_argument`。
- `StaticSParameterModel` 构造时对维度乘法溢出、维度/存储不匹配以及任一非有限 S
  分量抛出 `std::invalid_argument`。它仍忽略查询频率（包括负值和非有限值），
  保留零端口、名称、端口 id 和参考阻抗元数据的历史语义，不引入 1024 端口上限。

这些检查不修改传入矩阵；失败后可以继续使用有效输入调用 API。

### PLL 相关相噪与有限带宽 jitter 的 C++ 集成示例

[`examples/pll-phase-jitter-sdk`](../examples/pll-phase-jitter-sdk/README.md)
提供 installed-only、双翻译单元 C++17 示例：两路同载波 locked PI PLL，显式共享
reference 的复 H 和独立 VCO，完整 cross C/P，以及直接从 rad²/Hz budget 计算的
单输出有限 band jitter。含 Release 有效的独立数值 oracle；不新增差分 jitter API。

### Frozen DC adapter integration

[`examples/frozen-adapter-sdk`](../examples/frozen-adapter-sdk/README.md) is a
second installed-only dual-TU host example. It owns frozen numeric state and
explicit optional noise, preserves RC frequency dependence, and checks existing
linear/noise/conversion/spectrum/coherent APIs with independent matrix oracles.
The `frozen_adapter_sdk_installed` and `pll_phase_jitter_sdk_installed` top-level
CTests validate both native integration examples in the same SDK stage; they
supplement, rather than replace, core numerical regression gates.
