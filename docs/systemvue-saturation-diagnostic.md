# SystemVue 饱和区诊断与未成立的候选模型

后续进展：本文“整体 tanh”假设继续作为未吻合记录保留。新推导的增量 tanh
基波模型在新增交叉验证点上吻合，已接入 C++/C/Python/JSON，见
[饱和基波模型](saturating-fundamental.md)。厂商警告和诊断证据边界仍保留。

2026-09-30，固定 sample 参数组：增益 20 dB、OP1dB=20 dBm、OPSAT=23 dBm、
OIP2=40 dBm、OIP3=30 dBm、RISO=100 dB、端口 50 ohm、频率 1 GHz。
新增输入 +3、+6、+10 dBm 的独立 RFAMP 采集，逐项验证 16 个参数、拓扑、
时间戳和 CF/CGAIN/DCP 结果形状。

## 诊断和验收的边界

本机官方帮助 `sim/Gain_Compression_and_Intermod_Generation.html` 说明主信号在
P1dB 以下用三阶多项式、以上用双曲正切，但未公开完整连接参数与高阶算法。
SystemVue 在这三个输入点均明确警告频谱与测量准确性下降，因此不能把这些结果
作为严格兼容通过证据。默认采集和比较继续拒绝任何 manager 消息。

新增 `--compression-diagnostic` 仅用于 sample 压缩案例。PowerShell 在此模式
保留完整 manager 消息和原始数据，由 Python 仅接受 RFAmp 超过输入 P1dB 或
饱和点的两种已观测到的准确性警告。其他器件、其他 INFO/WARNING/ERROR、
拼接的额外消息、陈旧数据均拒绝。成功状态为 captured_diagnostic，且
eligible_for_compatibility=false；没有兼容通过判据。

+6 dBm 首次尝试返回“超过输入饱和点”，与 +3 dBm 的“超过输入 P1dB”不同，
原诊断匹配拒绝该结果。确认完整消息后扩大到这第二种已知警告，再用新目录采集。
首次 failed 记录保留在 build-reference/compression-plus6-diagnostic-001，
有效诊断使用后缀 002。未覆盖或删除原始失败记录。

## 无实测拟合的候选公式及结果

令 A=sqrt(G)、r=10^(-1/20)、y1=sqrt(OP1dB_W)、L=sqrt(OPSAT_W)，输入幅度
x=sqrt(Pin_W)，x1=y1/(A*r)。在 P1dB 以下使用已有三阶基波关系：

```text
y(x) = A*x*[1 - (1-r)*(x/x1)^2]
```

尝试以幅度及一阶导数连续为附加假设连接饱和支路：

```text
s = A*(3*r-2)
k = s / [L*(1-y1^2/L^2)]
b = atanh(y1/L)
y(x) = L*tanh[k*(x-x1)+b], x>x1
Pout = y(x)^2
```

所有系数只由标称参数和连续性假设确定，没有拟合采集数据；这些连续性条件是
RFModel 的待检验假设，不是厂商公开承诺。

| 输入 dBm | SystemVue 输出 W | 候选输出 W | 相对差异 |
| --- | --- | --- | --- |
| +3 | 0.1397742930 | 0.1345933934 | −3.7066% |
| +6 | 0.1870114833 | 0.1754138794 | −6.2015% |
| +10 | 0.1992293693 | 0.1967879916 | −1.2254% |

该公式虽然满足小信号增益、P1dB、导数连续和饱和极限，但不能重现当前参考曲线。
不将它作为 SystemVue 模型写入数值核心，不改变原先低功率 P1dB API 的有效域。
由于参考自身有警告，本报告仅说明观测差异，不能据此断言真实器件物理误差。
后续需要继续确定厂商饱和支路及其驱动功率定义，并把基波压缩与谐波输入限制分开验证。

## 证据和复现

- 精简原始数据：validation/systemvue-2023-single-saturation-diagnostic-captures.json。
- 无拟合诊断：validation/systemvue-2023-single-saturation-diagnostic.json。
- 脚本：scripts/reference/diagnose-single-saturation.py。
- 上一阶段 OPSAT 扫描六配置 CI 全部通过：[运行 36661128528](https://github.com/minexin/RFSim/actions/runs/36661128528)，归档 validation/ci-04bcd05.json。

```powershell
python scripts/reference/run-systemvue-reference.py compression `
  build-reference/RFModel_AmplifierCompression.wsv build-reference/saturation-new `
  --source-power-dbm 3 --compression-profile sample --compression-diagnostic
python scripts/reference/diagnose-single-saturation.py `
  validation/systemvue-2023-single-saturation-diagnostic-captures.json `
  build-reference/saturation-replay.json
```

新模式不改变默认严格验收，也不自动重试或终止超时的 COM 进程。恢复采集为
build-reference/compression-saturation-diagnostic-restored-001：sample、OPSAT=23 dBm、
输入 −30 dBm，无 manager 消息，H2/H3 与原基准完全一致。

本机诊断测试 4 项验证公式约束、已观测差异、默认拒绝警告以及诊断仍校验
参数/拓扑/新鲜度。后台运行器 6 项测试验证状态、日志保留、选项范围和超时语义。
Debug/Release 的单器件压缩与谐波定向 CTest 各 2/2 通过。新诊断测试已纳入
六配置 CI；全部模型兼容目标仍未完成。
