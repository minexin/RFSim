# SystemVue 双音互调参考

2026-09-30 用已安装的 SystemVue 2023、独立 RFAMP 专用副本采集等功率
1.0/1.1 GHz CW。G=20 dB、NF=3 dB、OP1dB=20 dBm、OPSAT=23 dBm、
OIP2=40 dBm、OIP3=30 dBm、RISO=100 dB，端口均为 50 ohm。
源 Phase=0、SrcType=0、MultiCarrier=0、EnablePN=0，两音均启用。

三组每音输入 −30、−6、−3 dBm，均无 manager 消息且数据时间戳属于本次分析。
路径中心频率显式设置为 1 GHz；首次保留自动选择时因双音产生中心频率歧义，
失败采集保存在 build-reference/compression-two-tone-minus30-001。

## 比较对象

compare-two-tone.py 严格核验拓扑、源数组、器件参数、测量通道和时间戳。
用 IDNo/IDName 的完整来源表达式识别产物，不硬编码随运行变化的数字 ID。
每个非 DC 产物取平坦谱的功率样本，验证恰有两个边界；n 阶产物宽 n Hz，
不积分 P 数组、不把边界加倍。输入器件 BW=1 MHz，但本实验 SrcType 为 CW。

每组比较 10 个唯一频率产物：

| 阶次 | 产物 | 频率 GHz |
| --- | --- | --- |
| 2 | 差频、和频、两音 H2 | 0.1、2.1、2.0、2.2 |
| 3 | 2f1−f2、2f2−f1、2f1+f2、f1+2f2、两音 H3 | 0.9、1.2、3.1、3.2、3.0、3.3 |

另按 D[(Source.SourceN)],Source,RFAmp 识别两个直接基波，用已有基波压缩接口
按总输入功率 2P 比较。基波频率处还存在独立三阶谱项；路径 DCP 含通道其他
功率，不能把它或完整多项式基波与此直接分量混同。本实验未验证重叠项相干
合并、失真相位、噪声、反射传播、级联或高阶产物。

## 结果及限幅诊断

| 每音输入 dBm | 未限幅二阶最大相对差异 | 未限幅三阶最大相对差异 | 总功率限幅假设最大相对差异 |
| --- | --- | --- | --- |
| −30 | 5.00e-8 | 7.50e-8 | 7.50e-8 |
| −6 | 0.00223831 | 0.00335934 | 7.39e-8 |
| −3 | 0.165785 | 0.258716 | 5.61e-8 |

六个直接基波功率点均在 1e-7 阈值内通过，最大相对差异 2.50e-8。
未限幅截点多项式在 −30 dBm 的 10 个产物通过，高功率两组保留失败结论。
报告整体 passed=false，不把已知差异改成通过。

诊断把已由单音提出的 −4/−1 dB 限幅偏移用于双音，总驱动取 2P。令 q 为
限幅后的总功率/原总功率，则 n 阶原预测乘 q^n；未从这三组观测重新拟合参数。
三组 q 分别为 1、0.998882745249、0.926170113553，30 个产物均在原阈值内。
逐音驱动假设在 −3 dBm 的三阶相对差异仍达 0.254631。
两种假设都标注 affects_compatibility_verdict=false；当前单音模型仍拒绝多音。
这些证据支持下一步实现统一总 RF 功率限幅，但不等于多音接口已经交付。

后续已实现 [原生多音分阶接口](multitone-amplifier.md)，贯通 C/Python/JSON，保留
直接响应与生成重叠项。下一步仍需完善来源和传播语义，并用不等功率、独立
相位、不同频距和器件参数组交叉验证。

## 重放和运行状态

精简采集 validation/systemvue-2023-two-tone-captures.json 保留原始采集 SHA256；
报告 validation/systemvue-2023-two-tone.json 保留精简采集和 DLL 的 SHA256。

```powershell
python scripts/reference/run-systemvue-reference.py compression `
  build-reference/RFModel_AmplifierCompression.wsv build-reference/new-two-tone-run `
  --source-power-dbm -6 --compression-two-tone
python scripts/reference/compare-two-tone.py build-msvc/Release/rfmodel_c.dll `
  validation/systemvue-2023-two-tone-captures.json build-reference/two-tone-replay.json
```

比较命令因保留高功率差异而返回 1；回归测试同时断言小信号通过、高功率失败、
两种假设的区分，以及参数/谱形/消息/时间戳拒绝、ID 重映射和改动观测暴露差异。
采集旗标仅接受 sample、显式每音功率，拒绝诊断告警或 OPSAT 覆盖组合。

恢复单音的 001/002 采集已经成功仿真，H2/H3 与此前单音 −30 dBm 完全一致。
初次比较失败来自参数表示：原默认枚举 GetValue 返回单元素数组，显式 Set
表达式经 Data 返回标量。比较器现在仅对单源枚举接受这两种等价表示，仍拒绝
多元素、布尔、错误数值和非有限数，不放宽数值比较阈值。

曾尝试用 SetValue(Array(...)) 强制恢复数组表示，003 调用未返回并进入
timeout_unresolved。该尝试已从脚本移除；当次采集进程 PID 3900 和 SystemVue
PID 13956 仍在，未杀进程、未重新提交仿真。002 是最后已完成且通过核验的
单音恢复证据，不能把它当作 003 结束后的工程状态。后续采集前必须先核查并
处理该待决调用；离线回归和开发不依赖它。

本次本机 Debug/Release 各 54/54 CTest 通过，新增双音回归含 5 个测试方法，
后台驱动 7 项通过。随后补充主频谱数据集新鲜度检查，两种配置的双音回归
再次通过。数值核心和公共 ABI 未改动；跨平台构建以本提交 CI 为准。

2026-10-08 后续检查：003 的 stderr 已记录分析返回和“未产生新鲜数据”异常，
当前没有 SystemVue 进程。原 timeout_unresolved 文件是 runner 退出时的快照，
不再表示当前仍有待决调用；该失败输出保持排除。有效双音参考数据不受影响。

后续 [不等功率参考](systemvue-unequal-two-tone.md) 已完成四组 64 项验证。
原等功率未限幅诊断及其失败结论保持不变。
