# 四个转换功率波边界 fixture 迁入直接 C++

日期：2026-10-13。发布基线：`cabbbcedabc9d2cf9f38e6da86f5cab06d16bce0`。本轮只扩展两个既有原生测试和本记录，不修改数值内核、公共 API、CMake、Python/C ABI 或厂商数据，不执行提交、推送或发布。

本轮补齐 [Python 退役计划](python-retirement-plan.md) 列出的四个指定物理 fixture。它们的检查入口直接调用 C++17 函数，不经过 ctypes、JSON、Python 包或 CLI；原 Python 测试继续保留。本记录是该计划对应“待迁移”条目的增量证据，不表示全部 Python 高层行为已经迁移。

## 对应关系与独立预期

| 保留的 Python 参考 | 原生函数 | 独立预期与加强项 |
|---|---|---|
| `test_conversion_power_wave_api.py::test_extreme_references_follow_independent_norton_thevenin_formulas` | `conversion_power_wave_test.cpp::test_mismatched_extreme_references` | 75 Ω 电阻、45+3j Ω Thevenin 源；1 Ω 旧参考；五个尺度 1e−24、1e−12、1、1e12、1e24 Ω，每个分别使用实参考和乘以 1+0.3j 的复参考。检查器件 S、直接/共轭噪声传递、Norton 仿射偏置、内生 C/P、边界源/反射及源 C/P |
| `test_conversion_power_wave_api.py::test_boundary_matches_thevenin_full_cp_and_retains_internal_zeros` | `conversion_power_wave_test.cpp::test_partial_boundary_thevenin` | 三通道只选 0、2 两个外部通道；两个独立实潜变量 Thevenin 电压形成非零跨通道 C 和 P。逐项检查外部完整矩阵，内部源/反射及 C/P 的行和列必须精确为零；反向变换故意反转选择列表顺序；拒绝遗漏的外部边界及内部噪声泄漏，输入快照不变 |
| `test_conversion_power_wave_file.py::test_dc_lift_noise_and_frequency_major_reference_order` | `conversion_power_wave_test.cpp::test_unequal_dc_linear_lift` | 25 Ω 串联电阻、290 K；DC 使用 31/73 Ω 两个不同实参考，RF 使用 19+8j/110+31j Ω。独立 Johnson 电压密度 4kTR 与 KCL 推导完整热噪声，检查升维前后 C/P、DC 的 P=C、RF 的 P=0、跨频结构零、频率优先排序、确定性波和净噪声。非法复 DC 参考拒绝且不改变输入样本 |
| `test_conversion_power_wave_file.py::test_touchstone_embedded_noise_preserves_physical_loads` | `touchstone_noise_test.cpp::test_complex_reference_conversion_chain` | 与 `examples/noisy-amplifier.s2p` 等价的 75 Ω 内嵌噪声文件，复参考 31+12j/73−21j Ω、2 GHz 线性采样/升维、物理 50 Ω 冷负载。独立噪声因子多项式和两端口物理反馈方程给出所有出射/入射/互相关 C/P，另检查波、净噪声与还原后的 50 Ω 矩阵 |

### 非匹配极端参考

对 `I=V/R+J`，出射波为 `(R−conj(Z))/(R+Z)*a − R*sqrt(Re Z)/(R+Z)*J`，因此固定物理 Norton 噪声/偏置的参考变化比例可直接求出。对 `V+Zs*I=Vs`，边界为 `a=(Zs−Z)/(Zs+conj(Z))*b + sqrt(Re Z)/(Zs+conj(Z))*Vs`。预期值不使用 RFModel 的参考变换或求解器。

这些范围检验有限、可表示的小传递系数与小协方差；并不覆盖 IEEE 次正规噪声，也不声称所有正实部参考都已穷举。

### 不同 DC 参考的热噪声

令 `D=R+Z1+Z2`。串联 Johnson 电压的两个出射系数相反，所以 `Cij=sign(i,j)*4kTR*sqrt(Re Zi*Re Zj)/|D|²`，对角符号为正、非对角为负。该预期不调用 `passive_thermal_noise`，也不通过“两个 RFModel API 互相比较”构造真值。DC 是实随机量，因而完整 DC 块 `P=C`；RF 块为 proper 噪声，且不同频率相互独立。

### Touchstone 到实际负载的完整链

原始文件给定 `S21=2`、`Fmin=2`、`GammaOpt=0` 和 `Rn/Z0=1/2`。独立匹配 `F(Gamma)=2+2|Gamma|²/(1−|Gamma|²)` 的系数可得 `C75/kT=diag(1,4)`。

75 Ω 坐标下，50 Ω 负载的反射为 −1/5，故内生噪声满足 `b75=(c1,c2−2c1/5)`；转到实际 50 Ω 出射坐标后，`b50=sqrt(24/25)*b75`。由此得到完整 `C50/kT=[[0.96,−0.384],[−0.384,3.9936]]`。复参考中的协方差、入射统计和互统计均从该物理解和波定义推导。线性 proper 噪声的 P 为零；加载求解器从实正交分量重建时允许下述尺度内的舍入残差。

## 容差、失败检查和范围

- 非零极端参考噪声传递、偏置及 C/P 的相对误差严格小于 1e−12；零共轭传递使用对应直接传递幅度的 3e−12 尺度。
- 其余新数值断言使用 3e−12 乘物理尺度：反射/S 用无量纲尺度；边界源用自身幅度；相关边界 C/P 用对角方差几何平均；热噪声用 kT。没有 1 W/Hz 的绝对容差下限。
- 部分外部边界的内部源、反射及 C/P 行列必须精确为零。DC/RF 的独立跨频噪声块也必须精确为零。其余需要数值计算得到的物理零使用明确的物理尺度。
- 所有断言在 Release/NDEBUG 下继续启用。五个独立的 Release 故障注入分别破坏 Norton 传递、内部 C 行列零值（1e−40 泄漏）、DC 非对角 P、Touchstone 原始参考阻抗、加载互协方差，均被拒绝。注入仅存在于单独的临时 include 树中。
- 没有扩大 Touchstone 解析范围；仍为既有 legacy 全矩阵与二端口噪声记录。本轮不覆盖自动频率规划、自动非线性工作点的复参考编排或通用 JSON 输入。

实际构建选项、各配置结果、源码哈希、误差指标和故障诊断见 [验证记录](../validation/native-conversion-boundary-fixtures-validation-20261013.json)。本轮没有新的 Windows/macOS、远端 CI 或 SystemVue 仿真证据；物理回归通过不改变既有厂商通过、失败或 diagnostic 分类。
