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
4. 将每个模型的解析测试与 SystemVue 导出的结果分开记录。参考自动化尚未成功执行时，保持 `systemvue_comparison: not_executed`，不把自洽回归标成产品兼容。
