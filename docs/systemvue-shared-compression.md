# SystemVue 多源共同基波压缩对照

2026-10-09 在 SystemVue 2023.0.0.11903 中完成 23 组受控记录：
21 组没有分析警告，2 组为完全相消的独立诊断。前者的 64 个直接基波
复幅度比较全部通过原相对 1e-7 阈值，最大相对误差约 1.250e-8。
比较对象为 [共同基波压缩接口](coherent-compression.md)，数值模型未作调整。

## 拓扑和控制量

使用已有专用 RFAMP 参考副本：MultiSource → RFAmp → 50 ohm 终端。
放大器 G=20 dB、OP1dB=20 dBm、OPSAT=23 dBm、OIP2=40 dBm、
OIP3=30 dBm、NF=3 dB、RISO=100 dB，端口为 50 ohm。
本次接受范围限定在原生输入 P1dB 及以下，不验收饱和共同驱动。

两来源均为 CW；比较 1.0/1.1 GHz 对照和 1.0/1.0 GHz 重叠输入。
显式设置两来源的 Name、Enable、SrcType、MultiCarrier、EnablePN、Freq、
BW、Pwr、Phase、RefClk；修改后 ClearModelCache 并执行分析。
同一非空 RefClk 为共时钟，两个空字符串表示两个独立来源。

| 扫描 | 覆盖 |
|---|---|
| 每音 −6 dBm | 共时钟 0°/90°；独立时钟 0°/90°/180° |
| 每音 −30 dBm | 共时钟 0°/90°；独立时钟 0°/180° |
| −6/−26 dBm | 共时钟及独立时钟各 0°/90°/180° |
| −26/−6 dBm | 共时钟 90°，强弱来源互换 |
| 不同频率对照 | 每音 −6 dBm，独立来源，0°/90° |
| 关闭热噪声 | 与四组 −30 dBm 案例成对重复 |
| 完全相消诊断 | 每音 −6 dBm，共时钟 180°，噪声开启/关闭各一组 |

每次核验源参数、放大器参数、CF、主频谱与路径数据集的新鲜度及消息。
参数表达式中字符串数组使用官方 Set 接口，脚本明确采用 VBScript。

## 直接基波与输出总谱的区别

按 IDNo/IDName 中完整 D[(Source.SourceN)],Source,RFAmp 表达式定位直接谱，
不依赖数字 ID 或谱线顺序。同频共时钟输入在放大器之前归并，输出只有一个
直接基波组；独立来源保留两组。每组必须具有中心频率 ±0.5 Hz 的两个 CW 边界。

由实际 V2/Z2 还原 sqrt(W) 复幅度，检查与 P2 自洽后对照原生输出。
RFModel 从源/参考时钟生成自己的分组，不使用厂商相干编号作为模型输入。
零相位、正交相位、20 dB 不平衡和强弱互换均在本组直接波比较中通过。

ShowTotals=1 用于保存方向总谱，但输出总谱还包含三阶载频重叠项和噪声，
不能拿它直接验收仅输出基波的节点。本报告的直接基波通过不表示完整
谐波、互调、噪声、DC 或完整输出总谱已兼容。

## 小信号 RFPwrIn 差额的成对诊断

开启热噪声时，四组 −30 dBm 案例的 RFPwrIn 比纯载波输入功率高约
1.6e-11 W，超出原 1e-7 相对阈值。关闭 CalcNoise 后，四组比较全部通过。
开启和关闭噪声两次实测的差值均约 1.601554432e-11 W：

| 来源关系 | 纯载波输入 W | 噪声开启 RFPwrIn W | 噪声关闭 RFPwrIn W |
|---|---:|---:|---:|
| 共时钟 0° | 4e-6 | 4.00001591554232e-6 | 3.999999899998e-6 |
| 共时钟 90° | 2e-6 | 2.00001596554332e-6 | 1.999999949999e-6 |
| 独立 0°/180° | 2e-6 | 2.00001597554432e-6 | 1.999999960000e-6 |

这些控制支持该差额来自本例热噪声计算，而不是共同压缩模型的载波归并错误。
不把测得差额拟合进模型，也不放宽阈值。是否以及如何把噪声计入压缩内部驱动，
仍需要独立验证；本组没有证明 RFAMP 的全部内部驱动语义。

本报告保留 direct_carrier_passed=true、drive_power_passed=false、passed=false。
较高功率的含噪声输入功率比较通过，只表示差额落在容差内，不表示噪声不存在。

这与 [tee 合路器案例](systemvue-coherent-network.md) 是不同现象：
后者 CalcNoise=0 时仍有五组 RFPwrIn 差异，不能用此次噪声诊断宣称已解决。

## 完全相消的诊断记录

等功率共时钟 180° 输入触发两条已确认警告：DCP 低于 −200 dBm 阈值，
以及路径没有期望信号。采集器仅在显式诊断模式下保留这组完整消息；
新增、缺失或变更的警告不能通过白名单。状态为 captured_diagnostic，
eligible_for_compatibility=false，两个案例不参与兼容结论。

关闭噪声后，RFPwrIn 约 3.77e-36 W，SystemVue 不再输出 F2/P2/ID2/V2/Z2。
这是测量缺失，不能伪造为“输出功率实测等于零”。开启噪声时仍保留噪声输出谱，
RFPwrIn 约 1.601554432e-11 W，没有直接基波。报告分别保留输出谱存在状态。

## 重放与采集入口

归档：validation/systemvue-2023-shared-compression-captures.json。
报告：validation/systemvue-2023-shared-compression.json。
保留每次原始捕获 SHA256、磁盘源副本 SHA256、实际参数、设置、脚本及时间戳。
源副本散列不表示参数修改后的内存工作区快照。

```powershell
python scripts/reference/run-systemvue-reference.py compression build-reference/RFModel_AmplifierCompression.wsv build-reference/new-shared-run --source-power-dbm -6 --compression-two-tone --compression-same-frequency --compression-locked --compression-first-phase-deg 0 --compression-second-phase-deg 90 --compression-show-totals --compression-disable-noise
python scripts/reference/archive-shared-compression.py build-reference/new-shared-run --workspace build-reference/RFModel_AmplifierCompression.wsv --systemvue-version 2023.0.0.11903 --output build-reference/new-shared-capture.json
python scripts/reference/compare-shared-compression.py build-msvc/Release/rfmodel_c.dll validation/systemvue-2023-shared-compression-captures.json build-reference/shared-report.json
```

最后一条命令目前返回 1，因为保留了四项含噪声输入功率差异。
完全相消诊断需额外指定 --compression-cancellation-diagnostic；
这不是允许任意警告的开关。省略 --compression-disable-noise 会显式恢复 CalcNoise=1；
省略 --compression-show-totals 会恢复 ShowTotals=0。

开始前已将相干合路工作区保存到新的专用备份，然后切换至放大器副本。
结束后恢复 −30 dBm、1 GHz 单音、空参考时钟、CalcNoise=1、ShowTotals=0；
新鲜采集及原单音比较通过。SystemVue 保持打开，无待决采集。

## 后续范围

需要继续覆盖不同物理输入支路的合路驱动、较高输入和饱和区、多端口反向驱动、
噪声与压缩的联合语义，以及生成谐波/互调的跨级身份。当前通过范围仅是
表中条件下的直接基波共同压缩，不是完整 RF Design 放大器验收。

本阶段本地 CTest Debug/Release 各 68/68，通过既有数值模型及新的实测回归。
新比较/归档 8 项、后台运行器 14 项测试通过。测试明确保留四项含噪声驱动差异，
CI 绿色不表示这些报告项已变为通过；原生库与数值容差未改变。
