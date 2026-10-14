# 纯确定性变频波求解

`FrequencyConversionModel::solve_waves(source, reflection, connections = {}, output_offset = {})`
和 `FrequencyConversionNetwork::solve_waves(output_offset = {})` 返回独立的
`ConversionWaveResult { incident, outgoing, relative_residual }`。

```cpp
#include <rfmodel/conversion/conversion_network.hpp>
rfmodel::FrequencyConversionModel model(1e6, {{0, 1}}, {1, {.4}}, {1, {.1}});
const auto waves = model.solve_waves({1.}, {.2}, {}, {.03});
rfmodel::FrequencyConversionNetwork network(1e6);
network.add({model, {1.}, {.2}, model.zero_noise(), model.zero_noise()});
const auto same = network.solve_waves({.03});
```

## 明确的噪声边界

新入口不接收、传播或验证噪声 C/P，也不返回零噪声矩阵来暗示完成了噪声分析。
网络中的存储噪声不参与该求解；现有 `add`/`add_power_waves` 仍检查矩阵维度与有限性，
但新求解本身不检查 PSD、DC 噪声或接线端边界噪声约束。需要这些检查时使用
`validate_noise` 或完整 `analyze`。

原 `analyze(..., loaded_noise = false)` 语义完全保留：仍验证源及本征 C/P，仍计算
完整出射噪声；`false` 只是不计算加载后的入射/互相关/净噪声。所有既有函数、结果类型
和 C ABI 均不替换，新 API 仅增加 C++ 入口。Python 产品接口保持退役。

## 方程、数值范围与残差

新旧入口共享边界装配与最终波方程残差检查：

- `b = A*a + B*conj(a) + output_offset`
- 外部边界 `a = reflection*b + source`
- 物理接线使用每侧复参考阻抗的自反射和耦合系数，保持相同 V、相反内向 I
- DC 为实通道，复 DC 源、反射或偏置拒绝；内部接线端允许确定性出射偏置

正常数值范围下，直接形成实正交方程 `(I-Q*R)*b = Q*source + offset`，调用真正的
单 `std::vector<Complex>` RHS 消元，不构建方阵 RHS、逆矩阵或噪声传递矩阵。
主元阈值和逐 RHS 归一化 backward-error 与现有多 RHS 求解一致；波结果仍采用同一
物理残差及 `512*N*epsilon` 门限。稠密矩阵消元仍是三次量级，本阶段改善工作量和常数，
不是稀疏算法或渐近复杂度降阶。

先形成 `Q*source` 的运算顺序在极小/极大数值处可能丢失旧传递矩阵路径仍能表示的值。
实现检测非有限、次正规乘积/累计 RHS、非零乘积下溢为零，以及求解溢出或可能的输出
范围损失。此时保守回退到旧 `inverse*Q` 的确定性运算顺序，仍不读取或传播任何噪声。
因此并非所有输入都走快路径，也不承诺极端范围加速；旧路径本身无法表示或残差不合格的
输入仍拒绝。仅返回很小的残差不等于没有下溢，回归专门对极小值使用相对或逐位对照。

## Newton 工作点

仅中间 Newton 候选改用 `solve_waves`。初始拓扑完整噪声验证仍在任何确定性回调前；
最终接受态仍完整 `analyze`，包括 `loaded_noise=false` 时的出射 C/P。
接受态本征噪声 provider 仍在收敛后按设备索引顺序各调用一次，不进入候选或回溯，
失败和拒绝态不会消耗 provider。返回值仍是接受的非线性波，不用最后一次仿射求解替换。

## 工程验证与基准

`parameter_vector_solver_test` 和 `conversion_wave_solve_test` 覆盖主元交换、奇异、
极端尺度、零 RHS、首列对照、A/B、DC、物理复参考接线、内部偏置与独立解析反馈环。
既有工作点/噪声回归及安装后的双 TU 消费者继续检查 API 与 provider 契约。

可复现基准在 `benchmarks/conversion_wave_solve.cpp`，不注册为计时 CTest：

```sh
c++ -std=c++17 -O3 -DNDEBUG -Iinclude benchmarks/conversion_wave_solve.cpp -o wave-bench
./wave-bench
# 与旧头文件对照：使用相同源码和编译器，另外传 -DRFMODEL_BENCH_BASELINE
```

N=16/64/128/256，各五次，输出每次静态网络和非线性端到端时间、中位时间、残差、迭代与
回溯次数，以及每次确定性回调输入的校验和轨迹。静态对照为旧 `analyze(false)`，包括旧接口
不可关闭的出射噪声工作；非线性对照始终包含初始完整验证和最终完整噪声分析。
计时只作环境相关观察，不设硬门槛，不代表 SystemVue 或厂商兼容验收。

内部 `parameter_detail::solve(SMatrix, std::vector<Complex>, scale_floor)` 还对 RHS
消元、回代乘积和除法的下溢/次正规范围损失抛出 `std::overflow_error`；这里的异常表示
无法可靠保持数值范围，不仅指向上溢出。模型入口捕获它并走上述确定性回退。
原多 RHS 重载保持原有行为。
