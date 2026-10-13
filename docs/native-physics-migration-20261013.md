# 有限物理回归迁入直接 C++：Chebyshev 与收敛快照噪声

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

日期：2026-10-13。范围仅为原生测试与本记录；数值内核、公共 API、CMake、Python/C ABI 和厂商数据均未修改。本记录补充 [Python 退役计划](python-retirement-plan.md) 中指出的两类有限缺口，不表示全部 Python 编排已迁移或可立即删除。

## 迁移对应关系

| 保留的 Python 参考 | 直接 C++ 对应 | 独立预期与加强项 |
|---|---|---|
| `test_chebyshev_filter.py::test_four_responses_against_analog_poles` | `chebyshev_filter_test.cpp::test_four_mappings_and_topology` | 教科书模拟极点乘积；显式 LP/HP/BP/BS 频率映射；阶数 2/3/8/16/64、四种纹波、两个拓扑。除原等边缘衰减外，再检查边缘衰减等于两倍纹波 |
| `test_independent_edge_attenuation_and_parity` | `test_independent_edges_and_dc_parity` | 边缘功率 `10^(-A/10)`，独立于纹波；阶数 2/3/8/9；两带边、DC 和几何中心；偶阶不匹配、奇阶通过 |
| `test_dual_topology_preserves_transmission` | 上述两个函数 | 所有四类响应的传输不变、两个反射变号、阶数奇偶反射关系；加入 DC、带中心和极高频限值 |
| `test_narrow_band_and_extreme_attenuation` | `test_extreme_band_edges_and_dc` | 1e−200、1e200 与其相邻浮点带边；BP/BS、阶数 2/3/64、两拓扑；两带边的带符号极点响应；最小次正规纹波至 6000 dB 的有限性 |
| `test_odd_polynomial_near_dc_without_cancellation` | `test_extreme_band_edges_and_dc` | `T3(x)=4x^3−3x`：6000 dB 时近 DC 传输功率 0.1；频率比 1e−608 下溢而反射约 3e−308 仍保留；再检查真正次正规的非零反射 |
| `test_am_pm_operating_point.py::test_nf_rebuild_uses_phase_derivative` | `conversion_operating_point_test.cpp::am_pm_snapshot_reference_noise` | 独立标量压缩公式、dB 域相位斜率数值积分、径向/切向导数；保持原 0.01 驱动，再加复相位及 0.06 饱和区驱动 |
| `test_highorder_operating_point.py::test_noise_analysis_reconstructs_converged_model` | `conversion_operating_point_test.cpp::highorder_snapshot_reference_noise` | 独立共同限幅公式及 `cos^3(x)=(3cos(x)+cos(3x))/4`；保持原 0.03 驱动，再加复相位及 0.06 驱动；同时检查基波与三次谐波 |

原有低通递推、偶阶 ABCD、三阶显式 LC 网络测试全部保留。新增四类响应在适用点检查互易性、两行功率守恒、幺正交叉项及零无源热噪声。

## 收敛快照组合的实际检查

六个工作点 fixture 从与最终驱动不同的 0.08 幅度及不同相位初值启动，要求至少一次迭代并满足收敛残差。后续网络只使用求解器返回的 `linearized_devices` 和 `output_offset`，不再次调用非线性线性化函数来构造快照。

独立参考给出每个输出沿输入径向/切向扰动的复导数 `r`、`t`。290 K 的 proper 入射热噪声可写成两个相互独立、方差均为 `kT/2` 的实随机量，因此：

- `C_ij/kT = (r_i conj(r_j) + t_i conj(t_j))/2`
- `P_ij/kT = (r_i r_j + t_i t_j)/2`
- 输出参考增益 `G = (|r|² + |t|²)/2`

同样的外积分别用于出射、入射和入射—出射互统计，共六个完整矩阵。检查覆盖结构零、复相位、基波—三次谐波相关、净入射噪声、最终雅可比、最终仿射偏置、重放波及非零固定输出偏置。参考温度实验必须给出独立增益、`kT*G`、NF 因子 1、NF 0 dB 和零等效附加温度。

这验证直接 C++ 调用者的收敛快照组合，不能替代仍保留的 JSON 自动重建、参数优先级或序列化测试。

## 容差与可表示量

- 模拟极点复传输误差 `<3e−12`，保持原 Python/原生极点参考的严格量级。
- 独立边缘功率误差 `<5e−12`；DC/中心及相邻极端带边 `<5e−13`。
- 幺正行功率误差 `<1e−12`、交叉项 `<1e−14`；热噪声每项 `<1e−32 W/Hz`。互易、拓扑变号与奇偶关系直接检查精确等式。
- 6000 dB 偶阶 DC 传输为可表示的约 `1e−300` 幅度，其平方已下溢，故检查幅度的相对误差 `<2e−12`。同样检查约 `3e−308` 的反射相对误差，不能以单位量级绝对容差放过丢零。
- 真正次正规反射约为 `3*denorm_min`，允许一单位次正规 ULP 的最终舍入，并强制非零。
- 工作点波、雅可比和 C/P 使用 `5e−10` 乘各自物理幅度/导数乘积尺度；噪声先除以 `kT`，没有 1 W/Hz 或单位幅度绝对容差下限。结构零要求精确零。此容差容纳独立相位积分误差，严于原 Python 组合参考的 `2e−8`。
- NF 因子/分贝误差 `<1e−12`，等效附加温度绝对值 `<1e−9 K`。

所有检查使用始终启用的异常断言，不依赖 Release 中会被 `NDEBUG` 关闭的标准 `assert`。

## 本地验证与边界

Linux、GCC 14.2.0、CMake 3.31.10：Debug 和 Release 各完成两个受影响目标的专项编译/测试，以及 91/91 全量原生 CTest。配置关闭 C ABI、CLI 与 Python 测试，保留原生示例；两个缓存均无 Python 发现项或 C 编译器项，CTest 清单无 Python/C ABI 命令。

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release \
  -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF \
  -DRFMODEL_BUILD_PYTHON_TESTS=OFF
cmake --build build-native --parallel 2
ctest --test-dir build-native --output-on-failure
```

将 `Release` 换成 `Debug` 并使用另一构建目录可重现另一配置。独立的 Release 故障注入副本验证这些断言能够拒绝映射、边缘缩放、拓扑、提前下溢、AM/PM 斜率、高阶限幅斜率和返回偏置等错误；生产源码未保留任何注入修改。详见 [本轮验证记录](../validation/native-physics-migration-validation-20261013.json)。

本轮没有 Windows/macOS 或远端 CI 结果，没有新 SystemVue 仿真，没有扩展厂商兼容结论。既有 AM/PM diagnostic 差异、高阶 holdout 与校准区分、未实现厂商参数等保持原状态。Python 参考、JSON/CLI 契约、C ABI 和历史数据均继续保留。
