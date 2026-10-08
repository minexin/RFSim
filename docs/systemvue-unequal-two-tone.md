# SystemVue 不等功率双音交叉验证

2026-10-08 使用本机 SystemVue 2023，沿用独立 RFAMP 专用副本与原采集接口。
1.0/1.1 GHz CW 的输入功率分别受控，源相位均为 0。器件参数与
[等功率双音](systemvue-two-tone.md) 相同：G=20 dB、NF=3 dB、
OP1dB=20 dBm、OPSAT=23 dBm、OIP2=40 dBm、OIP3=30 dBm、
RISO=100 dB，端口 50 ohm。数值核心、限幅参数和 1e-7 阈值均未改动。

## 实测结果

| Source1 / Source2，dBm | 比较项数 | 最大功率相对误差 | 结论 |
| --- | ---: | ---: | --- |
| −30 / −40 | 16 | 7.5001004e-8 | 通过 |
| −3 / −12 | 16 | 7.2457295e-8 | 通过 |
| −12 / −3 | 16 | 7.2457295e-8 | 通过 |
| 0 / −20 | 16 | 5.5770261e-8 | 通过 |

每组含 2 个直接项、4 个二阶项、10 个三阶项，其中 4 个三阶项与载频重叠。
共 64 个独立来源功率点通过。9 dB 输入差对应两侧 IM3 功率比
7.943282347 或其倒数 0.125892541，交换两音功率后正确反转。
0/−20 dBm 的总输入约 0.0432 dBm，低于该样本输入 P1dB=1 dBm；
它覆盖了单音功率高于原等功率每音上限的情况。

比较使用已有总 RF 功率限幅模型。两路原始输入分别构造幅度，不取平均，
不从观测拟合系数。主频谱与路径数据集均验证本次运行时间戳，manager 消息为空。
完整来源表达式匹配仍为强制条件，不能只按输出频率匹配。

## 采集与重放

Python runner 增加可选 --compression-second-power-dbm；仅接受双音 sample
模式和有限的 −200..30 dBm 数值。省略时第二音与第一音相同。
PowerShell 入口实施同样约束，使用既有 Set 表达式传给 SystemVue。

    python scripts/reference/run-systemvue-reference.py compression build-reference/RFModel_AmplifierCompression.wsv build-reference/new-unequal-run --source-power-dbm -3 --compression-two-tone --compression-second-power-dbm -12

    python scripts/reference/compare-amplifier-terms.py build-msvc/Release/rfmodel_c.dll validation/systemvue-2023-unequal-two-tone-captures.json build-reference/unequal-replay.json

仅在只读探针确认专用实例没有工程时才加 --open-copy。
采集输出目录必须全新；超时后检查同一进程，不自动重试。

精简证据为 validation/systemvue-2023-unequal-two-tone-captures.json，
保留参数、谱项、时间戳、原始目录及 SHA256。数值报告为
validation/systemvue-2023-unequal-amplifier-terms.json，另记录精简证据及 DLL 散列。
原始目录为 build-reference/compression-unequal-{minus30-minus40,minus3-minus12,
minus12-minus3,zero-minus20}-001，不上传厂商工作区文件。

新元数据 source_powers_dbm 为按 Source1/Source2 排序的两个数。
比较器仍可读旧等功率 source_power_dbm_per_tone；同时提供两个字段会拒绝，
有序功率对重复也会拒绝。输出报告统一使用 source_powers_dbm。
修改元数据而不修改真实源参数、交换次序或伪装成等功率都无法通过校验。
旧 compare-two-tone.py 的未限幅诊断仍只接受等功率，原高功率失败结论保留。

## 恢复、回归及边界

最终已恢复 −30 dBm 单音，采集目录为
build-reference/compression-unequal-restored-002。其新鲜度、源参数和 H2/H3
数值均通过 compare-single-harmonics.py；002 是本轮最后一次成功分析。
SystemVue 实例保留专用工程，未保存或替换任何用户工作区。

本轮本机 Debug/Release 全套 CTest 各 58/58 通过；第四组加入后再运行两种配置
的双音/来源回归。后台 runner 9 项、来源比较器 7 项测试通过。
现有 C++/C ABI 未修改，跨平台结果以本提交的六项 CI 作业为准。

这些结果支持固定参数下按总 RF 功率限幅及按输入幅度生成谱项的行为。
仍未证明不同相位、不同频距、独立参数组、多级非线性、完整噪声/变频联合分析，
也未验证载频重叠项的相干合并或全 RFAMP 等价。下一步需要扩展这些独立条件。
