# 放大器和衰减器模型差异初审

基准为本机 SystemVue 2023 的 `Help/systemvue.qch`，2026-09-24 读取 `rfdesign/Amp_(RF)_Part.html`、`rfdesign/Attenuator_Part.html`、`rfdesign/RFAMP.html` 和 `rfdesign/ATTN_Linear.html`。页面摘要和完整器件—模型关系见 `validation/systemvue-2023-rf-models.json`。以下为帮助语义核对，尚无 SystemVue 数值对照，不构成兼容验收。

## 一个器件对应多个模型

Amp (RF) 对应 RFAMP、RFAMP_HO、RFAMP_IP2、SDATA_NL、SDATA_NLI、SDATA_NL_HO、NPO2、NPOD2。

Attenuator 对应 ATTN_Linear、ATTN_NonLinear、AttnPwr、MOD_DSA、SDATA_NL、SDATA_NLI、NPO2、NPOD2。功率控制和数字步进模型不能用恒定衰减量代替。

## 当前实现与官方行为

| 模型 / 行为 | 官方帮助信息 | RFModel 当前状态与下一步 |
|---|---|---|
| ATTN_Linear 参数 | L 默认 3 dB；Zref、ZIN、ZOUT 默认 50 Ω；可指定 Ta、ParamFreqList、FrequencyDataName | MatchedTransmissionModel 仅有固定损耗、延迟、共同实数参考阻抗；尚无独立输入/输出阻抗和厂商频率参数映射 |
| ATTN_Linear 反射 | 帮助给出回波损耗与衰减量的关系，并指出极端输入/输出阻抗比会改变插入损耗 | 当前模型严格匹配；需要实测 S11/S22 和阻抗变化，不能因匹配情形下 S21 相似就声明等价 |
| ATTN_Linear 频率行为 | 模型正文称衰减跨频恒定，器件索引描述与频率参数又体现频率依赖能力 | 保留文档层面的不确定性；以单值和多频点参数分别建立 SystemVue 用例澄清 |
| ATTN_Linear 温度与 DC | Ta 可覆盖分析温度；DC 不阻断 | 现有传输模型支持 DC，热噪声需调用独立 API；缺少环境温度与器件温度的统一解析入口 |
| RFAMP 小信号参数 | G 默认 20 dB，NF 3 dB，RISO 50 dB，参考阻抗默认 50 Ω；支持多种端口参数表示和频率数组 | MatchedPolynomialAmplifier 仅匹配、单向、固定小信号增益；缺失有限 S12、失配参数映射和频率相关增益/噪声适配 |
| RFAMP 非线性参数 | OP1dB 默认 60 dBm，OPSAT 63 dBm，OIP3 70 dBm，OIP2 80 dBm；支持压缩曲线和 AM/PM | 现有 IIP3 工厂仅建立三阶多项式，P1dB 与 IIP3 由该多项式绑定；不能独立拟合官方全部参数，亦无饱和和 AM/PM |
| RFAMP 高阶与噪声 | 委托 RFAMP_HO；提供高阶选择、残余相位噪声及频率相关参数 | 现有实电压多项式最高九阶，未建立厂商高阶拟合、相位噪声和系统噪声传播语义 |
| RFAMP DC 与分析类型 | 阻断 DC；Spectrasys 之外的仿真器使用线性部分 | 现有多项式放大器可传播/产生 DC；仍是独立数学原语，需要专门的厂商模型适配和分析分派 |

## 实现与验收顺序

2026-09-24 进展：已根据上述增益和端口定义实现 [LinearAmplifierModel](linear-amplifier.md)，具备复阻抗、有限反向隔离和显式相位的小信号矩阵。它补充现有匹配多项式模型，尚未完成 RFAMP 参数适配、DC 阻断及噪声默认值；上表列出的完整兼容缺口继续保留。

1. 提取 RFAMP 引用的 Gain and Impedance、Port Parameter Types、Noise Parameters 和 RFAMP_HO 公式，确定增益归一化、反射、反向隔离和噪声定义。避免直接猜测失配时的 S21。
2. 建立器件参数到 S 矩阵及噪声协方差的适配层；验证匹配、复阻抗、有限反向隔离、DC 和多频点用例。通用理想器件保留自身明确的数学定义。
3. 对官方 OP1dB、OPSAT、OIP2/OIP3 的独立设置建立单音压缩及双音互调参考，再实现对应非线性拟合、AM/PM 和高阶行为。

## 2026-09-29 压缩路径核对与解析诊断

重新读取本机 systemvue.qch 的 rfdesign/RFAMP.html、
sim/Gain_Compression_and_Intermod_Generation.html、
sim/Input_vs_Output_1_dB_Compression.html 和 sim/AM_to_AM_and_AM_to_PM.html。
官方说明区分主信号压缩和互调生成：主信号在 P1dB 以下采用三阶多项式，以上采用
双曲正切；高阶奇数系数由 OP1dB、OPSAT、OIP3 经专有算法生成，部分偶数阶截点
采用经验估计。饱和时还限制输入音调贡献以约束总输出谱功率。
帮助并未公开完整系数生成公式，不能宣称现有单一三阶多项式完整复现该模型。
RFAMP 参数表明确 AMtoPM_Mode=0 为 Off，因此当前采集中的 −5 参数并不表示
该案例已启用 AM-to-PM。先前模式枚举待确认项由本次帮助核对解除。

据 1 dB 点的定义构造一个独立、无拟合系数的低功率假设：令小信号功率增益为 G，
输出 1 dB 点为 Po1，r=10^(-1/20)，则 Pi1=Po1/(G*r²)。
主信号输出功率假设为 `G*Pi*[1-(1-r)*Pi/Pi1]²`，仅用于 0<Pi≤Pi1。
该式在 Pi1 处严格给出 Po1，并在小信号极限恢复 G；它不是高阶互调或饱和模型。

diagnose-antenna-compression.py 将两级上述主信号模型和既有衰减器串接，使用采集
核验过的 OP1dB，不从观测误差拟合系数；原始采集哈希必须与参数报告相符。
四个功率点的末级增益残差均约 +2.0e-8，见
validation/systemvue-2023-antenna-compression-diagnostic.json。
这强烈支持低功率主信号压缩解释了当前功率相关变化；剩余常量残差仍未定因，
也没有解释噪声密度误差。原始兼容报告、线性核心及容差均保持不变。

```powershell
python scripts/reference/diagnose-antenna-compression.py `
  validation/systemvue-2023-antenna-power-sweep.json `
  validation/systemvue-2023-antenna-nonlinear-settings.json `
  build-reference/compression-diagnostic.json
```

解析单点、低功率极限和越界拒绝回归通过，已接入 CI。下一步应将独立可校准的
主信号压缩行为纳入明确命名的模型接口，并通过接近 P1dB 的隔离器件实测验证；
不能将这四个远低于压缩点的样本当作深压缩、互调或噪声兼容验收。
4. 将每个模型的解析测试与 SystemVue 导出的结果分开记录。参考自动化尚未成功执行时，保持 `systemvue_comparison: not_executed`，不把自洽回归标成产品兼容。
