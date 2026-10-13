# SystemVue 2023 PointDerivative AM/PM 显式适配器

`SystemVue2023PointDerivativeAmPm` 是 C++17 SDK 中可显式选择的经验校准模型，证据来自 SystemVue **2023.0.0.11903 RFAMP 的 PointDerivative 模式**。它支持相位响应、复功率波旋转及工作点直接/共轭导数组合。`LegacyPointDerivativeAmPm` 继续表示文档约束推导的原始解析曲线，默认调用和既有 C ABI 的含义保持不变。

## 参数与调用

构造参数依次是有符号峰值 `degrees/dB` 和输出参考功率 `dBm`。适配器在 C++ 内部使用以下冻结映射，再调用原解析曲线：

```text
effective_peak = peak_degrees_per_db * 1.0000010001140027
effective_reference_dbm = output_reference_dbm - 0.0012252678340014533
```

参考偏移对应 `20*log10(4/3)-2.5`，但“厂商使用 2.5 dB 取整”仍只是解释假设；约 1 ppm 的比例来自先前数据拟合，并非已证明的厂商常数。两项数值在本轮独立验证前冻结，没有用新增数据重新拟合。两种既有冻结候选均曾通过容差，因此仍不能唯一辨识厂商内部公式。

`response(P)` 输入**实际输出功率 W**，返回 `phase_rad` 和 `radial_phase_slope = 2*P*dφ/dP`。不要用输入功率加名义增益代替压缩后的输出功率。`transmit(z)` 以 `|z|²` 为功率旋转复功率波，保持幅度。有限性、非负功率、参考功率可表示性及零功率处理沿用原模型；转换后的参数也必须可表示。

```cpp
#include <rfmodel/nonlinear/am_pm.hpp>
#include <rfmodel/conversion/am_pm_linearization.hpp>
#include <rfmodel/conversion/amplifier_linearization.hpp>
using namespace rfmodel;
const SystemVue2023PointDerivativeAmPm phase(-3.5, 5.5);
const auto response = phase.response(0.001); // 1 mW actual output
const auto rotated = phase.transmit(Complex(0.03, 0.01));

const SaturatingFundamentalCompression amplitude(13., 8.5, 11.5);
const std::vector<ConversionChannel> channels{{0, 1}, {1, 1}};
ConversionNonlinearDevice device{0, [=](const std::vector<Complex>& incoming) {
    auto base = linearize_saturating_amplifier(915e6, channels, incoming, amplitude);
    return apply_output_am_pm(base, phase, {1});
}};
// Pass device to solve_conversion_operating_point with the network and drive.
```

`apply_output_am_pm` 对明确选择的每个 RF 输出使用该通道自己的功率，并组合真实相位导数；只给 A/B 乘常相位会漏掉幅相耦合。重复、越界或 DC 输出选择拒绝。与[原模型组合契约](am-pm.md)一致，网络固定偏置在器件回调后加入，内禀噪声注入参考面由调用者指定。本轮未新增 C ABI 或已退役的 Python 产品入口。

## 独立压缩与频率验证

[预登记计划](../validation/am-pm-point-derivative-adapter-plan-20261013.json)在分析前选定上述候选、`1e-5°` 圆周相位容差和最多 5 次分析。四组新增配置如下；所有 OIP3 最终按[事先修订](../validation/am-pm-point-derivative-adapter-amendment-20261013.json)设为 OP1dB + 20 dB，OIP2 为 140 dBm。

|配置|峰值 °/dB|参考 dBm|增益 dB|OP1dB / OPSAT dBm|普通源相位|判定范围|
|---|---:|---:|---:|---:|---:|---|
|915 MHz|−3.5|5.5|13|8.5 / 11.5|37°|无告警数值验证|
|2.45 GHz|4.25|−12.5|7|−9.5 / −6.5|−45°|无告警数值验证|
|10 GHz|−8|15|31|15 / 18|−120°|压缩告警，诊断数据|
|30 MHz|0|−4.5|−3|−1.5 / 1.5|123°|零相位及损耗对照|

每组计划 37 点，名义输出从参考 −10 dB 至 +8 dB，步长 0.5 dB；实际相位按实测 DCP 输出功率评分。Quick Sweep 的输入相位实测为 0；普通源评分分别减去各自实测输入相位。普通 DCP 与原始频谱 V/Z/P 是两个独立功率轴。

原生 probe 直接调用新 C++ 类型，Python 不再对参数做校正。与先前六组 438 点合并后：

|评分集合|无告警数值通过|告警诊断数值通过|全部最大误差 °|
|---|---:|---:|---:|
|登记扫描|549 / 549|37 / 37|2.31520e-7|
|额外端点|3 / 3|1 / 1|6.24785e-8|
|普通 DCP|9 / 9|1 / 1|1.36206e-7|
|普通原始频谱|9 / 9|1 / 1|4.84366e-8|

原始解析映射在合并登记扫描中仍只有 110/586 点通过，全部来自零相位对照。新模型数值通过不改变旧模型结论，也不把告警诊断提升为完整验收。

## 采集偏差与恢复

首个 915 MHz 配置将 OIP3 设为 130 dBm，触发其偏离建议 OP1dB 相对区间的 INFO；该次停止、归档并排除。修订 OIP3 后使用唯一预留额度，最终实际调用为 **1 次配置诊断 + 4 次参数组，共 5 次**，没有重跑 10 GHz。

四组均返回 38 点：完整的 37 个登记点及高于请求停止值 0.5 dB 的额外端点。数量检查发现偏差后，上层命令未将失败传播给后续独立调用，2.45 GHz 和 10 GHz 已执行；这不满足原计划逐组暂停的流程要求，不能描述为全程无偏差采集。随后停止并提交[偏差保留记录](../validation/am-pm-point-derivative-adapter-capture-deviations-20261013.json)，再执行最后的 30 MHz 组。全部 148 个登记点及 4 个额外端点均保留，没有改写计划点数或删点。

10 GHz 普通工作点触发“超过输入 1 dB 压缩点”的低精度告警，整组只作诊断。捕获来自停止后的只读恢复。`analysis_done` 的归一化依据是冻结辅助脚本先通过 `IsAnalysisDone` 检查、再因 `Manager.GetErrors` 抛出该告警的控制流推断；不是恢复文件中直接记录的完成标志。运行区间以调用意图和监督进程完成时间为界，这一较弱证据在归档中明确保留。

每份输入只替换原工作区目标字段字节；分析前核对哈希和 17 个 SI 参数。结果保存到新的独立文件，随后关闭。最终重新打开原始基准工作区而不运行分析，源、放大器和扫描路径的 109 个参数值及表达式与原基线一致。用户拥有的 SystemVue 实例保持打开。

[采集清单及实际脚本字节快照](../validation/am-pm-adapter-captures-20261013.json)与[规范化观测](../validation/am-pm-adapter-observations-20261013.json)保存来源哈希、诊断、完成依据及全部点。原厂工作区与完整 COM 捕获只保存在本地 `build-reference/ampm-adapter-study-20261013`；跨平台 CI 只重放提交的观测，不声称验证不可用的原始文件。

## 重放与工程验证

显式启用 `RFMODEL_BUILD_REFERENCE_TESTS=ON` 后构建 probe，再执行：

```sh
python -I -S scripts/reference/diagnose-systemvue-am-pm-adapter.py \
  validation/am-pm-parameter-study-observations-20261013.json \
  validation/am-pm-adapter-observations-20261013.json \
  new-adapter-result.json --native-probe PATH_TO_NATIVE_PROBE
```

有本地原始捕获时增加 `--raw-root .`，重新核对十份原始来源及归一化结果。输出拒绝覆盖；报告保存 1,220 次原生交换及构建身份。退出码表示全部无告警评分轴是否数值通过，`eligible_for_full_model_compatibility` 始终为 false。告警点也会评分并保留。

`am_pm_test` 对两种相位律检查独立 dB 导数积分、数值微分、功率守恒、恒相位边界、双音工作点导数及含噪声反馈闭环。`native_systemvue_am_pm_adapter` 检查全部归档点、告警与额外点保留、哈希链及错误频率/电压拒绝路径。安装消费者调用新模型；开发用安装/移动 SDK 重放执行全部归档套件，并禁止导入已退役的 `rfmodel` Python 产品包。

提交 `296952b` 已通过 Windows/MSVC 19.44 的 Debug/Release **全量 clean-first 构建**；完整 CTest 各 **129/129**，安装后的独立 C++/C 消费者各 **10/10**。全量 CTest 包含安装并移动 SDK 后的 33 项协议/归档/导入隔离检查。211 个 C/C++ 文件格式检查通过。

[Debug 正式重放](../validation/am-pm-adapter-Debug-20261013.json)和[Release 正式重放](../validation/am-pm-adapter-Release-20261013.json)各执行 1,220 次原生交换，均重新验证本地原始来源，全部数值记录完全一致。构建命令、日志哈希、证据字节复核及失败构建尝试见[本地验证记录](../validation/am-pm-adapter-validation-20261013.json)。初次构建曾因重复 `Path/PATH` 环境项失败，规范化子进程环境后又遇到沙箱内 MSBuild FileTracker 拒绝访问；使用获准的标准 MSVC 构建后通过，没有用这两次失败结果冒充完成验证。

用户确认授权后，本阶段五个提交已推送至 GitHub `main`，受测提交为 `741d695`。[本次跨平台 CI](https://github.com/minexin/RFSim/actions/runs/38407094754) 的 **18/18 项任务全部通过**，覆盖 Windows、Linux、macOS 的 Debug/Release。每个配置的核心/CLI 为 **119/119**、安装后的 C++/C 消费者为 **10/10**；独立纯 C++ API 为 **106/106**、移动后的纯头文件 SDK 消费者为 **8/8**；归档重放 CTest 为 **10/10**。

六个重放任务均报告校准模型 586/586、原解析模型 110/586，最大校准误差均为 `2.3151960348855027e-7°`。其中 37 个 10 GHz 告警点仍为诊断数据。任务状态、18 份日志摘要和上传产物元数据见[本次 CI 验证记录](../validation/am-pm-adapter-ci-20261013.json)。CI 使用已提交观测，不运行 SystemVue，也不声称复核本地专有工作区和完整 COM 捕获。此前本地验证文件保留当时未推送的历史状态；本记录更新发布结果，不更改受测实现。

## 未完成的兼容范围

这些数据验证给定输出功率下的单路 RFAMP 相位映射，没有验证 RFModel 对完整压缩网络输出功率的预测。模型无频率参数；离散配置通过不能证明连续频带、频率相关相位或记忆效应。30 MHz 为零相位对照，10 GHz 带告警，不能据此宣称全频段非零相位验收。

DataPoints 零 dBm 反例、Compression Curve 表格输入、多音和来源相位策略、高阶谐波的厂商相位选择、其他 SystemVue 版本及完整参数兼容仍待验证。经验常数来源尚未被唯一解释。长期 SystemVue 2023 兼容目标保持未完成。
