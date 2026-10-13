# 原生混合参考工作点验证记录

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

2026-10-13，Linux/GCC 14.2.0、CMake 3.31.10、严格 C++17（关闭语言扩展）。基线提交为
`fc7f1e3c74d00983dc4c782338f1cc747fd881ed`；本记录针对其后的原生混合参考工作点补丁。

此阶段新增 `solve_conversion_operating_point_power_waves`，共用既有阻尼 Newton 和返回类型，
保留各器件完整参考向量，并在真实残差、Newton 候选和噪声中使用物理接线。
既有标量公共实参考入口、C ABI、Python 和 JSON 实现未变更。

## 独立物理基准

[物理回归](../tests/conversion/conversion_operating_point_power_wave_test.cpp)直接从 V/I 定义构造
20 Ω 串联电阻、30 Ω Thevenin 源和双频受控电流器件：

```text
I1 = 0.01 V1 + 0.01 |V1|² V1
I2 = 0.02 V2 + 0.004 V1²
Vs1 = 1.2 + 1.6j V, Vs2 = 0
V1 = 0.6 + 0.8j V
V2 = 0.028 - 0.096j V
```

这是显式有限通道的合成基准，不代表完整时域多项式或厂商器件。
另有实 DC 非线性分支：`V=0.5 V`、`Vs=0.8125 V`，同样经 20+30 Ω 串联阻抗。
共九通道，覆盖公共实参考、不等实参考和两组逐频点复参考。

独立预期值不调用生产重归一化、接线系数或网络求解器：

- 解析物理根转换为局部 a/b 后，初值须零迭代接受；默认及位移初值收敛至同一根。
- 本地单调电压半径方程给出真实回调，解析 A/B 与各实/虚输入列的中心差分核对。
- 完整加载噪声使用独立物理径向/切向增量方程及实随机驱动列的 Gram 矩阵。
  包含 Thevenin 电压噪声、Norton 电流噪声、串联电压噪声、DC 噪声和跨器件 RF/DC 源相关。
- 对每个元素核对出射、入射和入射在前的交叉 C/P，并核对净噪声、连线守恒及重建物理 V/I 统计。
- 物理内生噪声在已知根处转换为各坐标的固定出射 C/P；不会把不变的波噪声误称为不变的物理噪声。
- 覆盖零非线性系数、无回调线性/仿射极限及已接线通道的固定出射偏置。

最终测试计数为 2,243 个波/物理检查、144 个差分导数检查、12,783 个噪声/快照检查。
最大绝对误差分别为 `1.5582722721e-15`、`5.6527176035e-11`、`1.1632227364e-25`。
噪声容差按实际噪声量级设置，没有 1 W/Hz 的绝对误差下限。

## 契约与负向回归

[契约回归](../tests/conversion/conversion_operating_point_power_wave_contract_test.cpp)通过 64 个拒绝用例，
覆盖完整回调参考向量（含仅第二项的虚部不同）、通道顺序/端口/频点/步长、维度、非有限数、
实 DC、边界与 C/P、额外源噪声、重复/缺失接线、初值、选项、奇异 Jacobian、迭代上限与回溯失败。
检查非法输入在回调之前拒绝，以及失败不改变调用方设备、偏置和额外噪声。

同器件不同端口的接线含交错频点和非零 A/B 转换。相同复参考、不同实 DC 参考及约 `1e308 Ω`
参考分别检查物理连线；极大参考的独立预期值使用无量纲 V/I，不假定 `long double` 比 `double` 范围大。
溢出的试探步可回溯，非法回调契约不会被当作可恢复溢出吞掉。

宽松容差下已接受的非精确根须保留原入射波和真实 F(a)。显式解析核对该点 A/B、仿射偏置以及
全部加载 C/P、交叉项和净噪声，防止用额外 Newton 步的导数计算噪声。
隔离突变验证中，改回简单交换接线、只比较首个回调参考、返回额外仿射步的波均被测试拒绝。

## 构建与安装结果

| 验证 | 结果 |
|---|---|
| Debug 完整构建、CTest（含绑定和既有原生归档重放） | 176/176 |
| Release 完整构建、CTest（含绑定和既有原生归档重放） | 176/176 |
| Debug 安装后的独立 C/C++ 消费者 | 7/7 |
| Release 安装后的独立 C/C++ 消费者 | 7/7 |
| 新建纯 C++ SDK 配置、构建、安装并整体移动后消费 | 5/5 |
| 两个新增测试的 AddressSanitizer + UndefinedBehaviorSanitizer | 通过 |
| 全部 196 个仓库 C/C++ 文件的 clang-format 19.1.7 检查及 git diff --check | 通过 |

Debug/Release 使用既有构建目录执行完整构建及完整测试，并非 clean-first 记录。
SDK-only 使用新目录；C ABI、CLI、示例、Python 测试全部关闭，未发现 C 编译器或 Python 解释器。
移动安装目录后，消费者仅使用新前缀的安装头文件，编译参数为 `-std=c++17`；Release 检查在
`NDEBUG` 下仍执行。新增消费者在两个独立翻译单元调用完整参数和默认参数入口。

AddressSanitizer/UndefinedBehaviorSanitizer 使用 `-O1 -g -fsanitize=address,undefined`。
本执行环境下 LeakSanitizer 报告不支持 ptrace，因此最终运行设置 `ASAN_OPTIONS=detect_leaks=0`；
不宣称泄漏检查通过。

完整构建的关键选项为 `-DCMAKE_BUILD_TYPE=Debug` 或 `Release`，
`-DCMAKE_CXX_EXTENSIONS=OFF -DRFMODEL_REQUIRE_PYTHON_TESTS=ON -DRFMODEL_BUILD_REFERENCE_TESTS=ON`，随后执行完整
`cmake --build ... -j2` 和 `ctest --test-dir ... -j2 --output-on-failure`。
SDK-only 与独立消费的可复现步骤见 [C++ SDK](cpp-sdk.md)及[新入口契约](conversion-operating-point-power-wave.md)。

本记录不包含此补丁的 Windows/MSVC、macOS/AppleClang 或远程 CI 结果，也没有新的 SystemVue 实测。
完整厂商兼容、内置非线性辅助模型的任意参考适配和工作点相关噪声回调仍未由此阶段完成。
