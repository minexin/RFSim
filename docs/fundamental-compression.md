# P1dB 标定的单音主信号压缩

`rfmodel/fundamental_compression.hpp` 提供 P1dBFundamentalCompression，构造参数为
小信号功率增益 dB 和实际输出 OP1dB（dBm）。输入和输出为 sqrt(W) 复功率波，
适用于外部匹配条件下的单音基波；相位保持不变。它不是多音频谱传输器。

```cpp
rfmodel::P1dBFundamentalCompression amplifier(20., 10.);
auto output = amplifier.transmit_fundamental({0.001, 0.002});
double input_limit_w = amplifier.input_p1db_watts();
```

设 r=10^(-1/20)、功率增益 G，则 Pi1=Po1/(G*r²)，输入 1 dB 点的 dBm 为
OP1dB−GdB+1。输出波为 sqrt(G)*a*[1−(1−r)*|a|²/Pi1]。
因此在 Pi1 处输出恰好为 Po1，增益恰好比小信号低 1 dB。
接口接收零输入；高于 Pi1 则报 out_of_range，仅容纳边界的浮点舍入误差。
非法参数和非有限输入报 invalid_argument，输出溢出报 overflow_error。

该模型实现的是已独立验证的低功率主信号假设，不包含谐波、互调、噪声、AM/PM、
反向传输、失配反馈或饱和；不实现 SpectrumTransmissionProvider，不能逐音调用
后宣称获得多音总功率压缩结果。现有 MatchedPolynomialAmplifier 保留 IIP3 标定和
谐波/互调语义，二者不能混同。JSON 批处理通过 p1db_fundamental 级显式限制单音输入。

模型公式与本机 SystemVue 帮助描述的 P1dB 以下三阶主信号行为一致，四个远低于
P1dB 的实测点支持此假设，但尚未在靠近 P1dB 的隔离器件中完成厂商数值验证。
不改变既有 SystemVue 兼容报告，也不覆盖其高阶专有拟合和饱和算法。

回归检查 P1dB 的实际输出功率及 1 dB 增益下降、复数相位、零输入、小信号极限、
半标定功率、越界、非有限参数，以及已归档两级链路解析结果。独立安装消费者
同时使用此公开头文件验证标定点。

2026-09-29：MSVC Debug/Release 全套各 45/45，独立安装消费者各 2/2。
本轮没有新增 SystemVue 采集；跨平台 CI 仍待核验。

## C 和 Python

C 函数 rfmodel_p1db_fundamental 接受 power_gain_db、output_p1db_dbm、复数 incident，
并将基波写入非空 output 指针。输入和输出均为 sqrt(W)，无需额外指定参考电阻。
任何失败均保持 output 原值；越界及非法参数返回 RFMODEL_INVALID_ARGUMENT。
接口不生成谐波或多音频谱，也不隐式钳制越界功率。

```python
output = library.p1db_fundamental(
    0.001 + 0.002j, power_gain_db=20., output_p1db_dbm=10.)
```

Python 封装调用同一个 C++ 核心，错误以 RFModelError 返回。该符号属于 ABI 1
增量扩展，需要使用更新后的 Python 包和动态库组合。标定点、相位、零输入、
非有限输入及越界由 Python 回归覆盖；C 测试另外检查空指针及失败不改写输出。

2026-09-29 接口验证：Debug/Release 全套各 45/45，安装消费者各 2/2，
安装后的 Release 动态库通过 Python 39 项测试。未新增厂商实测。

## 原生 JSON 链路与归档 SystemVue 信号比较

2026-09-29：新增 `scripts/reference/compare-antenna-compression.py`，通过公开
Python → C ABI → C++ 路径执行两级线性衰减器和两级 `p1db_fundamental`。
输入为已归档的四点功率扫描及非线性参数审计；通过 capture SHA256 关联两者，
核验源功率、节点顺序、OP1dB 和 AM/PM 关闭条件。衰减和增益沿用天线案例的
固定参数映射；此脚本不是任意工作区的自动模型提取器。

```powershell
& 'C:/Program Files/Keysight/SystemVue2023/Python/python/python.exe' `
  scripts/reference/compare-antenna-compression.py `
  build-msvc/Release/rfmodel_c.dll `
  validation/systemvue-2023-antenna-power-sweep.json `
  validation/systemvue-2023-antenna-nonlinear-settings.json `
  build-reference/native-compression-result.json
```

报告 `validation/systemvue-2023-antenna-native-compression.json` 记录动态库及两个
输入文件的 SHA256。四点（−70、−60、−50、−40 dBm）× 五节点 × 增益/信号功率
共 40/40 项通过，固定相对阈值仍为 1e-7。增益最大相对误差约 2.0000001e-8，
信号功率最大相对误差约 4.4999999e-8。没有用实测输出拟合校正系数。

新增 CTest `native_compression_reference` 包含三项测试：归档结果比较、篡改测量
必须失败、不一致证据必须拒绝；由现有三平台 CI 的 CTest 步骤执行。
本机 Debug/Release 均通过该测试及 Python API 的 42 项测试。本次没有修改
C++ 核心或重新采集 SystemVue，也未核验此提交的远端 CI。

这是信号子集的独立报告，不替换包含噪声的原始兼容报告。低功率吻合不能证明
靠近 P1dB、饱和、多音互调、AM/PM、失配或噪声已兼容；这些仍须独立验证。

## 接近压缩点的新实测：存在未通过项

2026-09-29 通过既有 Python/COM 驱动，在专用天线参考工作区分别运行源功率
+10 和 +11 dBm。其他线性参数及两台放大器的 16 个非线性参数均重新审计通过。
两级放大器 OP1dB 均为 60 dBm（1000 W）；+11 dBm 输入时末级实际输出约
978.756 W，已接近输出压缩标定功率。本实验是完整四级链路，不是隔离放大器。

原生 JSON 链路比较结果为 **16/20**，保持 1e-7 相对阈值。失败均在 RFAmp2：

| 源功率 | 增益相对误差 | 信号功率相对误差 |
| --- | --- | --- |
| +10 dBm | +1.1690274e-6 | +1.1940274e-6 |
| +11 dBm | +1.8931755e-6 | +1.9181755e-6 |

RFModel 的预测略高。这证明远低于压缩点的吻合不能直接推广到压缩点附近；
现有证据还不能将差异唯一归因于高阶系数、反馈或厂商其他内部处理。未引入
经验修正，未改变数值核心，也未把信号比较当成噪声兼容验证。

证据文件位于 validation：`systemvue-2023-antenna-plus10.json`、
`systemvue-2023-antenna-plus11.json`，以及 `systemvue-2023-antenna-near-compression-`
前缀的 `sweep.json`、`settings.json` 和 `comparison.json`。原始大体积采集仅保留
在 build-reference 中；归档的派生证据包含采集散列、时间和参数。复跑上一节命令时
将 sweep/settings 换为这里的文件即可，比较程序应返回 1，表示数值未通过。

结束后重新采集 −50 dBm（build-reference/antenna-power-restored-003），CGAIN、
CNF、CND、DCP 四组共 20 个测量值与 antenna-run-003 完全一致；未保存工作区。
新增回归要求明确保留这四项已知差异；将来改进模型后应连同证据重新评估，不能
仅放宽阈值。下一步需隔离单放大器并控制反向传输，区分模型项与链路效应。
