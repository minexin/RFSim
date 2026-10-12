# 旧式峰值导数 AM-PM 与工作点组合

`LegacyPointDerivativeAmPm` 提供输出功率驱动的相位曲线；`apply_output_am_pm` 将该曲线组合到已有 C++ 非线性工作点响应和直接/共轭导数。新增模型不改变已存在的放大器接口。C ABI、Python、JSON 接入尚未实现。

## 官方约束与推导范围

来源是本机 SystemVue 2023 官方帮助 `sim/AM_to_PM_-_Legacy.html`，页面及帮助包哈希见 [来源记录](../validation/systemvue-2023-am-pm-provenance.json)。官方描述低功率相位增量为零，高功率相位恒定，并在指定输出功率处达到指定的有符号 degrees/dB 峰值导数。帮助先给出幅度域三次曲线，再描述 dB 域峰值的移位/缩放；下面的闭式表达式是依这些约束推导的结果，不是厂商运行结果。

令 k 为峰值 degrees/dB，Pref=10^((PwrAMtoPM-30)/10) W，u=sqrt(Pout/Pref)。三次曲线的非零部分写为 a*u²+b*u³，要求 dφ/dPout_dBm 在 u=1 达到 k 且该导数的极值出现在 u=1，得到：

```
phi_deg = k/ln(10) * (30*u² - (40/3)*u³), 0 <= u < 1.5
phi_deg = 22.5*k/ln(10),                  u >= 1.5

d(phi_deg)/d(Pout_dBm) = k*u²*(3-2*u),    0 <= u < 1.5
                       = 0,              u >= 1.5
```

高功率延伸在 Pout=2.25*Pref 开始，相位和一阶导数连续；曲线在此不是二阶光滑。k 可为正、负或零，零表示恒等映射。官方 -5 degrees/dB、10 dBm 例子的渐近值约 -48.85°，本公式为 -48.8581292149°。这里只匹配文档所述数值，尚无新的 SystemVue 仿真对照。

## C++ API 与导数

`am_pm.hpp` 中的 `response(power_w)` 返回相位弧度和 `radial_phase_slope = 2*P*dφ/dP`。`transmit(z)` 返回 exp(iφ(|z|²))*z，保持复功率波的幅度。非有限参数、负功率、不可表示的参考功率、功率溢出均拒绝。零功率不取对数；极大功率/极小参考值通过比较幅度进入恒相位支路，不先计算可能溢出的比值。

`am_pm_linearization.hpp` 的 `apply_output_am_pm(base, law, output_channels)` 接受既有 `ConversionLinearization` 和明确的本地输出通道索引。每个选中通道使用自己的出射功率，未选通道原样保留。重复、越界和 DC 选择均拒绝；空集合不改变模型。该选择仅描述调用者指定的确定性数学组合，不推断厂商多音/谐波相位策略，也不保留同频来源身份。

令 z=F_i(a)、q=2*|z|²*dφ/dP、r=exp(iφ)，局部映射的 Wirtinger 导数为：

```
Az = r*(1+i*q/2)
Bz = r*i*q/2*(z/abs(z))²       # z=0 时为 0
Anew_i = Az*A_i + Bz*conj(B_i)
Bnew_i = Az*B_i + Bz*conj(A_i)
```

这一组合保留 AM 到 PM 的扰动耦合。仅给旧 A/B 乘一个固定相位，会遗漏功率扰动引起的相位变化。通用工作点求解器据组合后的真实 F、A、B 生成仿射偏置，再传播 C/P 噪声。网络固定出射偏置在器件回调之后加入，不受本适配旋转；器件内禀噪声仍需调用者明确指定其注入参考面。

```cpp
#include <rfmodel/am_pm_linearization.hpp>
#include <rfmodel/amplifier_linearization.hpp>
using namespace rfmodel;
const SaturatingFundamentalCompression amplitude(20., 20., 23.);
const LegacyPointDerivativeAmPm phase(-5., 10.);
const std::vector<ConversionChannel> channels{{0,1},{1,1}};
ConversionNonlinearDevice device{0, [=](const std::vector<Complex>& a) {
    auto base = linearize_saturating_amplifier(1e6, channels, a, amplitude);
    return apply_output_am_pm(base, phase, {1});
}};
// device 传入 solve_conversion_operating_point，统一求解反馈和收敛噪声。
```

## 回归和未完成项

`am_pm_test` 独立积分 dB 域导数并核对闭式曲线，覆盖正负/零峰值、零功率、低/高驱动、峰值锚点与恒相位边界。高阶双音响应组合用独立积分后的真实响应做实/虚中心差分，检查 A/B 及通道选择。反馈案例在非零 AM-PM 斜率区，以独立复数不动点迭代检查根，再用实 2×2 闭环矩阵检验 C/P 及非零幅相相关。安装 C++ consumer 直接包含新增头文件。

当前不是完整 AM-PM 兼容验收。厂商实测、旧式 DataPoints 模式、Compression Curve 的 AM-AM/AM-PM 表格输入、多音/来源选择、跨语言接口、频变/记忆效应与自动高阶标定仍待实现或核验。SystemVue 会话问题见 [恢复记录](systemvue-session-recovery.md)。

## 本地工程验证（2026-10-12）

MSVC Debug/Release 构建通过，CTest 各 119/119；安装后独立 C/C++ consumer 各 2/2。150 个 C/C++ 文件格式检查与 git diff --check 通过。日志为 `build-reference/am-pm-{build,ctest,install,consumer-build,consumer-test}-{Debug,Release}.log`。本轮只新增头文件与原生回归，未改旧结构布局，使用现有构建目录的增量构建；这些记录不是 clean-first 重建结果。本轮没有推送或运行新的远程 CI。

本地重跑可执行 `cmake --build build-msvc --config Debug --target am_pm_test` 与 `ctest --test-dir build-msvc -C Debug -R ^am_pm_test$ --output-on-failure`；完整回归去掉 `-R`，Release 替换配置名。
