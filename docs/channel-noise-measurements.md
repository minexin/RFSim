# 通道噪声积分与载噪比

本阶段将采样噪声谱密度转换为指定频带内的噪声功率，并结合明确选择的离散信号谱线计算载噪比。C++17、C ABI 和 Python 提供独立测量接口；转换网络 JSON/CLI 可直接选择物理端口和行波方向执行测量。

## 频带、插值与功率

请求给出 center_hz≥0、bandwidth_hz>0，均须有限。积分频带为：

~~~text
lower = max(0, center_hz - bandwidth_hz/2)
upper = center_hz + bandwidth_hz/2
effective_bandwidth = upper-lower
N = integral(PSD(f), lower..upper)       [W]
mean_density = N/effective_bandwidth    [W/Hz]
S = sum(selected line powers in band)  [W]
C/N = 10*(log10(S)-log10(N))            [dB], S>0 and N>0
~~~

PSD 样本以 W/Hz 表示，非负且有限，频率严格递增。至少两个样本必须覆盖整个截边后的频带。采用 W/Hz 域的分段线性插值，并准确积分这一插值函数：积分边缘可以落在样本之间，不能外推，也不把孤立单点默认为平坦噪声带。

结果准确度仍取决于采样是否能描述真实器件频响。跨越大间隔的两个样本只是用户提供的线性近似，不证明中间没有窄带峰谷。应通过加密频点检查积分收敛，尤其是滤波器边缘和多次变频附近。本阶段不自动生成或加密网络频率通道。

信号是已在每个频率合并好的离散功率谱线。频带两端均包含；带外线不计入。信号功率直接相加，不乘采样间隔。相同频率的相干项必须先合并幅度，不能以重复功率点代替。转换网络在同一通道已经完成复幅度叠加，JSON 使用其平方模。

零值情况不输出虚构的 0 dB：ratio_state 为 finite、noise_free、no_signal 或 empty。后三种情况下 C++ optional 和 Python/JSON 的 carrier_to_noise_db 均为空；C 结果中的该数字字段只在 ratio_state=0 时有效。计算有限载噪比使用对数之差，避免先除法造成的上溢/下溢。

## C++ / C / Python

C++ 头文件 rfmodel/channel_noise.hpp：

~~~cpp
auto result = rfmodel::measure_channel_noise(
    {{0., 1e-20}, {10., 3e-20}},  // Hz, W/Hz
    {{5., 1e-6}},                // Hz, W
    5., 4.);                    // center, requested bandwidth in Hz
~~~

NoiseDensitySample 保存 frequency_hz、watts_per_hz；ChannelSignalLine 保存 frequency_hz、power_w。ChannelNoiseMeasurement 返回实际上下边界、有效带宽、积分噪声、平均密度、所需信号功率、载噪比/状态、使用的插值区间数和带内所选谱线数。噪声样本和信号谱线各不超过 1000000 个，信号可以为空。

C 入口 rfmodel_measure_channel_noise 使用 rfmodel_channel_noise_request/result。输入为独立频率/密度和频率/功率数组；信号计数为零时信号指针可空。输出不得覆盖输入数组或请求结构，任何输入、范围或数值错误均不修改结果。

Python：

~~~python
metric = library.channel_noise(
    [(0, 1e-20), (10, 3e-20)],
    center_hz=5, bandwidth_hz=4,
    desired_lines=[(5, 1e-6)],
)
~~~

本例积分区间为 3..7 Hz，N=8e-20 W。独立接口也可以用于线性扫描得到的 PSD，不依赖变频网络类型。

## 转换网络 JSON

rfmodel.conversion-network 可选 channel_measurements 列表，包含 1..512 个测量项：

~~~json
{
  "name": "if_channel",
  "port": ["if_filter", 1],
  "wave": "outgoing",
  "center_hz": 200000000,
  "bandwidth_hz": 200000000,
  "desired_bins": [2]
}
~~~

name 唯一且非空；port=[设备id,物理端口]；wave 为 outgoing（默认）或 incident。desired_bins 必需，允许空列表表示只测噪声；索引不得重复，且必须存在于该端口。仅处于实际积分频带内的所选谱线参与信号功率。其他已知谱线，包括谐波或杂散，不自动当作所需信号。

指定端口的全部频点按频率排序，噪声密度取对应行波 C 矩阵的对角项。入射测量会自动请求所需入射统计量，不要求用户同时开启完整 loaded_noise 矩阵输出。测量不改写原有波、噪声矩阵或参考温度 NF 设置。

结果 channel_measurements 保留测量设置，包含全部指标和 interpolation=linear_w_per_hz。频点覆盖不足、未知端口/谱线、重复名称、非法方向或参数均报错，CLI 失败保留已有结果。

~~~powershell
python -m rfmodel examples/channel-noise-conversion.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/channel-noise-conversion-result.json
~~~

示例为 Butterworth RF 滤波器、LO=1 GHz 的理想实混频器和 Chebyshev IF 滤波器。RF 在镜像及所需频带采样，IF 在 0.1/0.2/0.3 GHz 及对应上边带采样；测量 0.1..0.3 GHz 的出射通道噪声，同时测量 RF 输入的入射噪声。所需信号明确选择 0.2 GHz，镜像噪声保留在输出 PSD 内。

## 与 SystemVue 2023 的对应关系

本机官方帮助把 CNP 定义为主通道的噪声功率积分，并明确规定频带跨 DC 时下边界截到零。CND 使用 1 Hz 通道积分，不等于任意宽带积分后除以有效带宽：例如中心为 0 Hz、请求带宽 1 Hz 时，实际只积分 0..0.5 Hz。CNR 还涉及所需信号及相噪通道功率；当前接口只使用明确提供的总噪声 PSD 和所选离散信号，不自动生成相噪或识别谱来源。

来源页及 SHA256 见 [本机帮助证据](../validation/systemvue-2023-channel-noise-help-provenance.json)。当前物理端口/行波选择不等同于厂商完整路径方向和首节点内部源参考面。尚待完成调制谱积分、相噪 PNCP、自动谱来源分类、自适应采样、完整路径测量映射及 SystemVue 实测验收。

通道平均功率使用各频率噪声密度的对角项；网络仍保留完整跨频率 C/P。这里不计算有限观测时间内的拍频、相敏探测或循环平稳时变功率，这些测量需要额外的时间窗和接收机定义。

## 工程验证记录

2026-10-09，本阶段验证结果：

- Windows MSVC Debug / Release clean-first 构建成功，CTest 各 101/101 通过。
- 两种配置安装后，独立 C/C++ consumer 各 2/2 通过，包含新接口的调用。
- 独立 Python 3.12 从本阶段 wheel 加载，连接安装后的 Release 动态库；通道测量 11、加载噪声 9、变频 NF 12、变频网络 13、单器件变频 11、Python API 85、相干系统 51、线性噪声 9 项测试全部通过。
- 132 个 C/C++ 文件格式检查和 git diff --check 通过。

测试包含非平坦 PSD 的解析积分、DC 截断、离散谱线边界、零信号/零噪声状态、动态范围、采样加密收敛、滤波—混频链路、C 输出原子性与别名保护，以及 CLI 失败保留已有结果。本记录是工程回归证据；SystemVue 实测验收仍未完成。
