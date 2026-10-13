# SystemVue 2023 AM/PM 参数映射迁移实验（2026-10-13）

六组预先登记的 PointDerivative 参数共取得 438 个固定步长扫描点，并各取得一次普通源及原始频谱观测。现有 `LegacyPointDerivativeAmPm` 的原始参数映射仍未通过厂商对比；两种带冻结比例系数的候选映射在本轮范围内通过。**本轮没有拟合新参数，也没有修改产品公式或宣布完整 AM/PM 兼容。**

## 登记与结果

[原始计划](../validation/am-pm-point-derivative-plan-20261013.json) 在新参数组分析前提交，SHA256 为 `8ab865ee8e8300e9201e3f111d0bfb674da98f51a6988de60920b4bbdddd5650`。相位绝对容差固定为 `1e-5°`，采用圆周差。比较入口是独立开发用 C++ 原生 probe；Python 仅负责归档、参数映射和评分，没有产品实现回退。

|冻结假设|438 点扫描通过数|扫描最大误差（°）|普通 DCP 通过数|普通原始频谱功率轴通过数|
|---|---:|---:|---:|---:|
|原始解析参数|73|0.00862770344|1/6|1/6|
|仅参考功率偏移|131|0.0000684069020|1/6|1/6|
|偏移 + 冻结比例|438|0.000000231520|6/6|6/6|
|先前回溯拟合的两个冻结参数|438|0.000000275655|6/6|6/6|

原始解析参数通过的 73 点全部来自零相位对照组，五组非零参数全部失败。失败点完整保留，没有裁掉零 dBm 或恒相位区。普通源相位为 30°，Quick Sweep 自有源实测相位为 0°；评分分别减去各自输入节点的实测相位。

“仅偏移”使用 `20*log10(4/3)-2.5 = -0.0012252678340014533 dB`；“偏移 + 冻结比例”另将峰值乘以 `1.0000010001140027`。最后一项使用同一比例及旧数据回溯所得 `-0.001225288713598571 dB` 偏移。上述参数在本轮采集前已经冻结。前者的 2.5 dB 取整只是待验证解释；比例系数来自旧数据拟合，不能称为厂商已证实常数。

|参数组|峰值（°/dB）|参考功率（dBm）|普通观测目标输出（dBm）|
|---|---:|---:|---:|
|negative-reference-10|-5|10|10|
|positive-reference-10|7|10|14|
|negative-reference-minus10|-5|-10|-6|
|positive-reference-30|7|30|30|
|zero-phase|0|10|10|
|fractional-parameters|-2.75|3.25|7.25|

每组固定 100 MHz、20 dB 增益；OP1dB/OPSAT/OIP2/OIP3 为 100/103/140/130 dBm，以隔离 AM/PM 参数。扫描覆盖参考输出 −10 至 +8 dB，步长 0.25 dB，共 73 点，包含恒相位延伸区。主评分轴是实际 DCP 输出 W；普通 DCP 与由 V/Z/P 核对的原始频谱功率分别评分，未混用两种功率定义。

## 会话恢复与采集修订

最初的运行期参数设置产生脚本语言提示及错误读回值，在 RunAnalysis 前停止。两次直接恢复调用又返回 `DISP_E_TYPEMISMATCH`。已将任务副本另存为新取证文件并从未修改输入恢复；没有将“切换脚本语言”作为已证实修复。用户随后打开可见空实例，任务拥有的隐藏实例经正常退出后，采集连接到用户实例。

通用 XML 序列化副本虽通过 XML 语义比较，加载后仍产生既有 DiffPhase 方程解析错误；改为只替换原始字节中目标字段的方案后，该错误未再出现。独立参数副本最终只改动 16 个目标字段，其余 XML 语义保持一致。每次分析前核对工作区目录、文件哈希和 17 个实际 SI 参数值。

第一组配置诊断发生了真实分析调用：原示例的 Compression Curve 按 Fidelity 自适应选点，仅取得 3 点，并报告扫描输入频率为空。官方帮助 `sim/Compression_Curve.html` 与 `sim/Amplitude_Sweep.html` 及官方 `Quick Sweep of Dual Amp w Filter.wsv` 的 Path4 确认固定步长需要 `QCSwpType=3`（Amplitude Sweep）。因此显式设置扫描频率 100 MHz 和该扫描类型，再使用预留恢复额度采集。诊断数据保留但不计入 438 点。

三份流程修订均在对应后续分析前提交：

- [独立副本及另存结果修订](../validation/am-pm-point-derivative-amendment-20261013.json)
- [原始字节保留修订](../validation/am-pm-point-derivative-byte-preservation-20261013.json)
- [固定步长扫描修订与次数核算](../validation/am-pm-point-derivative-sweep-correction-20261013.json)

总计 8 次分析：1 次基线、1 次排除的配置诊断、6 次正式参数组。没有增加上限。每组分析结果保存到新的独立工作区文件，随后关闭；原厂示例、基准副本和制备输入均未覆盖。最终重新打开原始基准副本，不运行分析；源、放大器及扫描路径共 109 个参数值和表达式与先前基线一致。原始副本与官方工作区 SHA256 仍为 `ead90a212c786eff0cfa782dab47dbfe14c421b3f43b32c573bb990cb8348877`。

## 证据与重放

[采集清单](../validation/am-pm-parameter-study-manifest-20261013.json) 保存实际调用台账、输入改动、原始捕获、结果工作区及恢复哈希。[采集源码快照](../validation/am-pm-parameter-study-capture-sources-20261013.json) 保存实际本地脚本，仅供审计，包含固定 Windows 路径，不是通用产品入口。[规范化观测](../validation/am-pm-parameter-study-observations-20261013.json) 保留全部点、SI 读回、时间戳和原始载波电压/阻抗。

原始 WSV 和完整 COM 捕获保留在本地 `build-reference/ampm-parameter-study-20261013`，未作为可重新分发的厂商文件提交。归档脚本核对数据新鲜度、节点、频率、源相位、扫描网格、V/Z/P 一致性和文件哈希；远程 CI 可使用提交的规范化观测重放，但不能声称复核不可用的本地原始文件。

```sh
python scripts/reference/diagnose-am-pm-parameter-study.py \
  validation/am-pm-point-derivative-plan-20261013.json \
  validation/am-pm-parameter-study-observations-20261013.json \
  new-result.json --native-probe PATH_TO_NATIVE_PROBE
```

存在本地原始捕获时加 `--raw-root .`，会重新从原始字节归一化并逐项比较。评分命令保留原始公式的失败退出码 `1`；候选映射通过不会使原始公式被报告为通过。已有输出拒绝覆盖。启用 `RFMODEL_BUILD_REFERENCE_TESTS=ON` 后，CTest 的 `native_diagnose_am_pm_parameter_study` 对上述结论及证据拒绝路径进行回归。

提交 `a962def` 的本地 Debug/Release 原生 probe 目标构建成功，两套 AM/PM CTest 各 2/2 通过（新增参数实验套件含 6 项单元测试）。[Debug 正式重放](../validation/am-pm-parameter-study-Debug-20261013.json)与[Release 正式重放](../validation/am-pm-parameter-study-Release-20261013.json)的三个功率轴结果完全一致，均重新核验本地原始捕获。211 个 C/C++ 文件格式检查及差异空白检查通过。这里没有声称本地全量构建、全量 CTest 或安装消费者重测；范围及日志哈希见[验证记录](../validation/am-pm-parameter-study-validation-20261013.json)。

## 未完成的兼容边界

本轮只覆盖 100 MHz、近似线性 AM-AM 条件、单路 RFAMP、六组 PointDerivative 参数。它没有验证强压缩下组合、多音/来源相位策略、频变或记忆效应，也未解释 DataPoints 模式的旧零 dBm 反例。两种冻结映射都在容差内，当前数据不能唯一辨识厂商归一化实现。后续应独立验证比例来源及不同 AM-AM/频率条件，再决定是否提供明确命名的厂商参数适配；不能静默替换现有文档推导公式。
