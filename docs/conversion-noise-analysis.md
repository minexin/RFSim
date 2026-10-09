# 变频网络的参考温度噪声分析

本接口对已装配的转换网络运行独立噪声参考实验，求解单个输出频点的增益、噪声因子、噪声系数和等效输入噪声温度。用户显式指定参考信号频带及受热激励的频带，镜像频带不会被自动忽略，也不会固定加减 3 dB。

它复用全部转换、共轭、物理连接、反射反馈和器件内生噪声 C/P。普通信号分析及其源噪声不变。当前结果是逐频点、相位平均、无噪声输出负载条件下的参考实验，不代表 SystemVue CNF 全部路径/宽带/相噪语义已经实现。

## 定义

令 R 为参考信号通道集合，T 为施加参考温度 T0 的源通道集合，要求 R 非空、无重复且 R⊆T。T 也非空且无重复。集合使用外部正频通道；测量输出是另一个外部正频通道，不得属于 T。所有外部反射保持原值，且不允许有源终端 |Γ|>1。

在温度实验中，原确定性源和源 C/P 替换为零；仅 T 内的通道注入独立、圆对称热噪声，Cii=k T0 (1-|Γi|²)、P=0。k=1.380649e-23 J/K。所有器件内生 C/P 按原网络保留。未选源及输出负载均无自身噪声。

第二次参考求解关闭内生噪声，只在 R 内注入 Cii=(1-|Γi|²)、P=0，以计算独立参考频带的相位平均传输增益。两次求解都保留网络的直接/共轭转换及所有反射往返。

如果 H、J 是包含反馈的直接及共轭源到输出传输，则：

~~~text
g_i = (1-|Γout|²) (1-|Γi|²) (|Hout,i|² + |Jout,i|²)
Gref = sum(g_i, i in R)
Nout = (1-|Γout|²) Cout,out  [W/Hz]
Nref = k T0 Gref            [W/Hz]
F = Nout / Nref
NF = 10 log10(F)           [dB]
Te = T0 (F-1)              [K]
~~~

Gref 是每个独立参考频带增益的总和，不是一次相干多音输入的总输出功率除以总输入功率。具有相敏共轭转换的网络中，它也不是某个固定输入相位的增益。例如 H=2、J=1 时，相位平均增益是 5，而实相位与正交相位的增益分别为 9 和 1。

选择一个参考信号频带且把镜像等相关输入带列入 T，可得到所选通道集合下的 SSB 参考结果。选择两个独立参考带时采用双频带归一化；只有两带增益相同，SSB 与双频带结果才相差 10log10(2)。可以扩展至更多参考带，但未声明的转换通道不会自动补入。

被加热源必须有可表示的非零耦合噪声，输出负载必须能接收功率。参考增益为零、非有限值、DC 参考、重复/内部通道、参考集合不是热源集合子集、奇异网络或非法内生噪声均报错。未加热的其他终端允许 |Γ|=1；不会为它们隐含增加热噪声。

## C++、C、Python

C++ 入口：

~~~cpp
auto metric = network.reference_noise_analysis(
    reference_channels, thermal_channels, output_channel, 290.);
~~~

索引沿用网络设备顺序、再按局部通道顺序。ConversionNoiseAnalysis 包含 reference_gain、reference_output_noise_w_per_hz、output_noise_w_per_hz、noise_factor、noise_figure_db、equivalent_input_temperature_k。成员函数是 const，运行参考实验不会修改原网络。

C 入口 rfmodel_conversion_network_noise_analysis 沿用设备和连接数组，附加 rfmodel_conversion_noise_request 和 rfmodel_conversion_noise_result。请求包含参考/热源索引数组及计数、输出索引、参考温度。结果不得与任何输入或描述结构重叠；失败保留整个原结果。

Python：

~~~python
metric = library.conversion_noise_analysis(
    spacing_hz, devices, connections,
    reference_channels=[desired],
    thermal_channels=[desired, image],
    output_channel=if_output,
    reference_temperature_k=290.0,
)
~~~

返回同名 ConversionNoiseAnalysis。reference_ohms 默认 50，与普通转换网络相同。参考温度只改变输入噪声实验，不改变器件已指定的内生温度或 C/P。

## JSON / CLI

rfmodel.conversion-network 顶层可选 noise_analyses，包含 1..512 个分析项。每项必需 name、reference_channels、thermal_channels、output_channel，可选 reference_temperature_k=290。name 为唯一非空字符串，通道写为 [设备id,端口,bin]。

~~~json
{
  "name": "ssb",
  "reference_channels": [["rf_pad", 0, 12]],
  "thermal_channels": [["rf_pad", 0, 8], ["rf_pad", 0, 12]],
  "output_channel": ["if_pad", 1, 2],
  "reference_temperature_k": 290
}
~~~

普通结果中的波和完整 C/P 仍对应用户原始源条件；noise_analyses 中的指标对应独立参考实验。每项输出保留分析设置，并标注 normalization=independent_phase_averaged_available_power、output_load_noise=excluded。任何分析失败时 CLI 不覆盖已有结果。

~~~powershell
python -m rfmodel examples/conversion-noise-figure.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/conversion-noise-figure-result.json
~~~

示例中，RF/IF 衰减器电压传输分别为 0.5/0.25，温度均为 290 K；理想实混频器把两个 RF 频带转换到同一 IF。解析值为 Gref,SSB=1/64、Nout=(17/16)kT0，因此 FSSB=68；两参考带时 Gref=1/32，F=34。原始信号幅度和源噪声密度与该参考实验相互独立。

## 厂商依据与验收边界

本机 SystemVue 2023 帮助中，MIXER_BASIC 区分输入 NF 参数、镜像输入噪声及 ImageSelfNoise；Image Rejection 只影响到达输入的谱，不应直接缩放内部镜像自噪声。SNF 是器件级参数量，CNF 则依赖通道噪声、路径增益及源温度约定。IMGNR/IMGNP 是路径上主/镜像通道积分测量，不能将当前输出噪声贡献比直接命名为这些厂商测量。

来源页及 SHA256 记录在 [本机帮助证据](../validation/systemvue-2023-conversion-noise-help-provenance.json)，它记录定义来源，不代表数值验收。

Keysight 的 [Mixer DesignGuide，第 13–15 页](https://edadownload.software.keysight.com/eedl/ads/2011_01/pdf/dgmixer.pdf) 同样区分单信号频带与多边带归一化，并讨论镜像反射和负载噪声重新混频。该资料用于核对术语，不能替代 SystemVue 2023 实测。

后续仍需实现宽带积分、路径和噪声来源追踪、输出热负载噪声重新混频的测量约定、LO 相噪、实际泵/压缩工作点，以及 MIXER_BASIC 的 NF/ImageSelfNoise 校准。当前没有直接接受厂商 NF 参数后自动构造器件 C/P；内生噪声必须由模型明确提供。

## 工程验证记录（2026-10-09）

- MSVC 2022 x64 Debug/Release 清理重建，完整 CTest 各 97/97 通过。
- 安装后独立 C/C++ 消费者两个配置各 2/2 通过，实际调用新增噪声入口。
- 独立 Python 3.12 从离线 wheel 导入：噪声分析 12/12、转换网络 13/13、单块转换 11/11、公共 API 85/85、系统图 51/51、线性噪声 9/9 通过。
- 原生退化测试对照既有两端口 NF，Python 使用独立复反射闭式解；另覆盖相敏转换、镜像抑制、温度缩放、参考实验不修改源及失败保护。
- 129 个 C/C++ 文件格式检查与 git diff --check 通过。跨平台结果以对应提交的 GitHub Actions 为准，SystemVue CNF 数值验收仍待完成。

[热负载统计量](conversion-loaded-noise.md) 可单独计算给定负载噪声下的入射/出射相关性与净功率；本参考温度实验仍使用明确的无噪声输出负载条件。
