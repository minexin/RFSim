# 旧式峰值导数 AM-PM 与工作点组合

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

`LegacyPointDerivativeAmPm` 提供输出功率驱动的相位曲线；`apply_output_am_pm` 将该曲线组合到已有 C++ 非线性工作点响应和直接/共轭导数。新增模型不改变已存在的放大器接口。C ABI、Python、JSON 接入已完成；新符号和可选描述不改变已有 C 结构布局或旧入口。

2026-10-13 新增独立命名的 [`SystemVue2023PointDerivativeAmPm`](systemvue-am-pm-adapter.md)，将先前冻结的经验参数映射落实为可显式选择的 C++ 模型，也支持 `apply_output_am_pm`。它不改动本页原模型公式；两者的厂商对比结论分别保留。

## 官方约束与推导范围

来源是本机 SystemVue 2023 官方帮助 `sim/AM_to_PM_-_Legacy.html`，页面及帮助包哈希见 [来源记录](../validation/systemvue-2023-am-pm-provenance.json)。官方描述低功率相位增量为零，高功率相位恒定，并在指定输出功率处达到指定的有符号 degrees/dB 峰值导数。帮助先给出幅度域三次曲线，再描述 dB 域峰值的移位/缩放；下面的闭式表达式是依这些约束推导的结果，不是厂商运行结果。

令 k 为峰值 degrees/dB，Pref=10^((PwrAMtoPM-30)/10) W，u=sqrt(Pout/Pref)。三次曲线的非零部分写为 a*u²+b*u³，要求 dφ/dPout_dBm 在 u=1 达到 k 且该导数的极值出现在 u=1，得到：

```
phi_deg = k/ln(10) * (30*u² - (40/3)*u³), 0 <= u < 1.5
phi_deg = 22.5*k/ln(10),                  u >= 1.5

d(phi_deg)/d(Pout_dBm) = k*u²*(3-2*u),    0 <= u < 1.5
                       = 0,              u >= 1.5
```

高功率延伸在 Pout=2.25*Pref 开始，相位和一阶导数连续；曲线在此不是二阶光滑。k 可为正、负或零，零表示恒等映射。官方 -5 degrees/dB、10 dBm 例子的渐近值约 -48.85°，本公式为 -48.8581292149°。该公式由文档约束推导。2026-10-12 的 A01 既有捕获已与真实 C API 对比：37点中30点超出1e-5°容差，最大差约0.006163°；详见[实测差异记录](systemvue-am-pm-existing-reference.md)，尚未达到厂商兼容。

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
#include <rfmodel/conversion/am_pm_linearization.hpp>
#include <rfmodel/conversion/amplifier_linearization.hpp>
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

2026-10-13 新增[六组参数映射迁移实验](systemvue-am-pm-parameter-study.md)：原始参数映射通过 73/438 扫描点，全部通过点来自零相位对照；两种先前冻结的修正映射均通过 438/438 点。本轮没有改变本页公式或核心实现，尚未唯一辨识厂商内部常数。

`am_pm_test` 独立积分 dB 域导数并核对闭式曲线，覆盖正负/零峰值、零功率、低/高驱动、峰值锚点与恒相位边界。高阶双音响应组合用独立积分后的真实响应做实/虚中心差分，检查 A/B 及通道选择。反馈案例在非零 AM-PM 斜率区，以独立复数不动点迭代检查根，再用实 2×2 闭环矩阵检验 C/P 及非零幅相相关。安装 C++ consumer 直接包含新增头文件。

当前不是完整 AM-PM 兼容验收。原始 PointDerivative 解析映射仍未通过厂商对比；新增显式校准模型的独立验证见[适配器记录](systemvue-am-pm-adapter.md)。旧式 DataPoints 的零 dBm 反例尚未解释。Compression Curve 的 AM-AM/AM-PM 表格输入、多音/来源选择、频变/记忆效应与自动高阶标定仍待实现或核验。SystemVue 会话问题见 [恢复记录](systemvue-session-recovery.md)。

## 本地工程验证（2026-10-12）

MSVC Debug/Release 构建通过，CTest 各 119/119；安装后独立 C/C++ consumer 各 2/2。150 个 C/C++ 文件格式检查与 git diff --check 通过。日志为 `build-reference/am-pm-{build,ctest,install,consumer-build,consumer-test}-{Debug,Release}.log`。本轮只新增头文件与原生回归，未改旧结构布局，使用现有构建目录的增量构建；这些记录不是 clean-first 重建结果。本轮没有推送或运行新的远程 CI。

本地重跑可执行 `cmake --build build-msvc --config Debug --target am_pm_test` 与 `ctest --test-dir build-msvc -C Debug -R ^am_pm_test$ --output-on-failure`；完整回归去掉 `-R`，Release 替换配置名。

## C ABI、Python 与 JSON

`rfmodel_evaluate_am_pm(peak_degrees_per_db, output_reference_dbm, output_power_w, output)` 返回相位弧度和径向相位斜率。功率单位为 W，参考功率为 dBm，峰值为 degrees/dB；非法参数不写输出。

`rfmodel_apply_output_am_pm` 接受 `rfmodel_am_pm_linearization_request`：通道数 1..512、正频率间隔、正实参考阻抗、物理端口/频点数组、已有出射波和逐行排列的 A/B。输出沿用 `rfmodel_conversion_linearization_output`。局部 C 入口不接收入射工作点，不返回仿射偏置；调用者按 `F(a)-A*a-B*conj(a)` 计算。

`rfmodel_conversion_network_solve_nonlinear_am_pm` 在旧通用非线性入口的 models/model_count 后新增 phases/phase_count。每项 `rfmodel_conversion_am_pm` 指向已声明的非线性 device，包含峰值、参考功率和本地输出索引。器件索引唯一，phase_count 不超过非线性模型数；支持双线性混频器、基波压缩、多项式和共同限幅高阶放大器。线性矩阵不能单独作为此入口的相位器件。

输出索引必须唯一、存在且为 RF 通道，最多 512 项；计数为零允许 NULL，表示恒等选择。即使空选择，其他参数仍须合法。索引按通道数组位置编号，不是物理端口号或频点值。固定偏置在相位组合之后加入；输入/器件噪声的含义不自动改变。局部和网络入口保护嵌套描述、索引及输入数组免受输出别名覆盖，容量不足、参数错误或不收敛时保留全部输出及诊断。调用者负责有效地址和生命周期。

Python 的 `Library.am_pm_response(output_power_w, *, peak_degrees_per_db, output_reference_dbm)` 返回 `AmPmResponse`。局部组合返回含一般仿射偏置的 `AmplifierLinearization`：

```python
channels = [(0, 1), (1, 1)]
incident = [0.01 + 0.003j, 0j]
base = library.linearize_saturating_amplifier(
    1e6, channels, incident, power_gain_db=20,
    output_p1db_dbm=20, output_saturation_dbm=23)
point = library.apply_output_am_pm(
    1e6, channels, incident, base,
    peak_degrees_per_db=-5, output_reference_dbm=10, output_channels=[1])
```

局部组合的 spacing_hz、channels、reference_ohms 必须与 base 对应；这些元数据不存储在 Python 返回结构中，封装不能核实其身份。`solve_conversion_operating_point(..., am_pm=[{"device": 0, "peak_degrees_per_db": -5, "output_reference_dbm": 10, "output_channels": [1]}])` 在每次非线性迭代使用真实相位响应及导数，省略 am_pm 保持原调用路径。

在 `rfmodel.conversion-network` 版本 1 JSON 中，am_pm 位于器件对象上，与 model 同级：

```json
"am_pm": {
  "peak_degrees_per_db": -5,
  "output_reference_dbm": 10,
  "output_channels": [1]
}
```

它要求显式 operating_point 和上述四类非线性模型之一，未知字段、布尔数值、非整数或重复索引均拒绝。收敛后重新组装带相位的 A/B 和偏置用于噪声系数及通道分析。完整例子为 [nonlinear-am-pm-feedback.json](../examples/nonlinear-am-pm-feedback.json)，包含物理负反馈、复数驱动及反馈支路噪声。

```powershell
$env:PYTHONPATH = "python"
python -m rfmodel examples/nonlinear-am-pm-feedback.json --library build-msvc/Release/rfmodel_c.dll --output am-pm-result.json
```

这是 ABI 1 的符号扩展，须部署含三个新符号的原生库；纯 Python wheel 不捆绑 DLL。

## 跨语言阶段验证（2026-10-12）

Debug/Release clean-first 构建和完整 CTest 各 120/120，安装后的 C/C++ consumer 各 2/2。Python 3.12 从新 wheel 导入并连接安装后的 Release DLL，17 组共 322 项通过，其中 AM-PM 新增 14 项。独立积分、实虚差分、不动点根与实 2×2 噪声矩阵核对相位响应、反馈和 C/P，另覆盖四种模型、多个相位器件、固定偏置顺序、热启动、噪声系数重建和 CLI 失败保留旧文件。C 专项覆盖短容量、嵌套数组别名和失败原子性，安装 C consumer 调用相位新符号。

日志为 `build-reference/am-pm-api-{build,ctest,install,consumer-build,consumer-test}-{Debug,Release}.log` 和 `build-reference/am-pm-api-wheel-<suite>.log`；摘要及哈希见 [本地验证记录](../validation/am-pm-api-validation-20261012.json)。本阶段未推送、未产生新的远程 CI 或厂商仿真数据。

完整 CTest 使用当前工作区，其中六份既有 SystemVue 参考脚本草稿保持未提交；其路径及哈希单列于验证 JSON。这是该工作区的真实执行证据，尚不是新提交的纯净检出验证。AM-PM 原生、跨语言与安装回归均已执行。
