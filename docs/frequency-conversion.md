# 固定泵频率转换、反射反馈与相关噪声

本阶段新增有限频率通道的转换矩阵求解器，提供 C++、C、Python 和 JSON/CLI。它同时求解确定性复功率波与噪声二阶统计量，支持正负频率折叠所需的共轭转换项、DC 和跨频率往返反馈。它是 RF System Analysis 的转换核心；完整器件图装配、厂商 Mixer 标定和 SystemVue 数值验收仍待完成。

## 通道和方程

一个通道由 (physical_port, bin) 唯一标识，频率为 bin×spacing_hz。bin 为 0..INT_MAX，物理端口编号为 0..1023，共有 1..512 个通道。通道顺序同时定义矩阵的行列和波矢量顺序。spacing_hz 及公共正实 reference_ohms 必须有限且严格为正；所有通道频率必须有限。

~~~text
b = A a + B conj(a) + c
a = Γ b + s + e
~~~

A/B 是无量纲的直接/共轭转换矩阵，行对应出射通道，列对应入射通道。A 可包含普通反射、正向转换、逆向转换或泄漏；B 描述依赖输入共轭的转换。Γ 是逐通道复反射系数，s 是确定性源波；e 是边界源噪声，c 是转换器内生出射噪声。源波和出射波单位 sqrt(W)，正频率采用 RMS 复功率波。所有频率相位相对于调用者给定的共同时间参考与固定泵相位。

内部使用每通道 [Re,Im] 两个实分量，将 A/B 转为实矩阵 M：

~~~text
Mxx = Re(A+B), Mxy = -Im(A-B)
Myx = Im(A+B), Myy = Re(A-B)
T = (I-M R)^(-1)
b_quadrature = T M s_quadrature
~~~

R 是 Γ 的实分量矩阵。求解包含矩阵残差及最终波方程残差检查；奇异或数值不可解的反馈会报错。代数方程可解不代表动态稳定性已经验收。

DC 通道只有实分量：源、反射和结果必须为实数。模型的 DC 输出行不得依赖虚分量产生虚 DC。输入 DC 的虚列不参与运算。矩阵、通道或物理边界非法时拒绝运行。

## 噪声契约

每组噪声提供两个 N×N 复矩阵，单位 W/Hz：

~~~text
C = E[z z^H]       covariance
P = E[z z^T]       complementary covariance
~~~

普通圆对称复噪声的 P=0。涉及共轭转换时，只保留 C 可能丢失边带间相关性；保留 C/P 的二阶描述和等价实双通道形式可参考 [Dang 与 Scharf 的研究](https://arxiv.org/abs/1105.5432)。这里的转换公式直接由 z=x+j y 展开实现：

~~~text
Qxx = Re(C+P)/2, Qyy = Re(C-P)/2
Qxy = Im(P-C)/2, Qyx = Im(P+C)/2
Qout = (T M) Qsource (T M)^T + T Qinherent T^T
~~~

输入必须对应有效的实对称半正定 Q；仅 C 半正定不够，P 也受联合约束。DC 虚分量的方差及互协方差必须为零，因此独立实 DC 噪声需要 Pii=Cii。不同通道、不同频率间的 C/P 均保留，不自动清零。

source_noise 与 intrinsic_noise 两组彼此独立，但每组内部允许任意有效相关性。它们都使用相同的噪声等效带宽约定；本接口不自动换算通道带宽、输入温度或单边/双边 PSD。没有隐含热噪声，也不对有源泵转换矩阵套用 kT(I-SS†)。输出 C/P 是出射波噪声，尚未输出净吸收噪声或自动 SSB/DSB 噪声系数。

## 理想实数混频矩阵

ideal_mixer_conversion 生成与已有 IdealRealMixer 一致的矩阵，数学定义为 y(t)=2 g x(t) cos(LO t+φ)，g=10^(gain_db/20)。LO 是给定参数，不是参与求解的第三个端口。

- 正频 RF 的上边带系数为 g exp(jφ)。高于 LO 的 RF 下变频采用 g exp(-jφ) 的直接项；低于 LO 的 RF 折叠采用 g exp(jφ) 的共轭项。
- RF 恰好等于 LO 时，DC 出射为 sqrt(2) Re(g exp(-jφ) a_RF)。
- 实 DC 输入上变频到 LO 的复幅度为 sqrt(2) g exp(jφ) a_DC。

必须声明每个 RF 通道生成的全部 IF 通道，缺少镜像或上边带会报错；不静默截谱。rf_port 与 if_port 不同，通道只能属于这两个端口。生成器为单向匹配理想转换，通用 matrix 模型则可显式给出反向转换和反射。

## Python 与 JSON

~~~python
channels = [(0, 8), (0, 12), (1, 2), (1, 18), (1, 22)]
a, b = library.ideal_mixer_conversion(1e8, channels, lo_bin=10)
result = library.frequency_conversion(
    1e8, channels, a, b,
    source=[0, 1, 0, 0, 0],
    source_covariance=source_covariance,
)
~~~

返回 ConversionResult：incident、outgoing、noise_covariance、noise_complementary、relative_residual。conjugate、source、reflection 和四个噪声矩阵默认全零。direct 必须明确提供。

JSON format=rfmodel.frequency-conversion、version=1，必需 spacing_hz、channels、model。channels 元素为 {port,bin}。model 可以是：

- type=matrix：必需 direct，可选 conjugate，均为 N×N 矩阵。
- type=ideal_real_mixer：必需 lo_bin，可选 gain_db=0、phase_radians=0、rf_port=0、if_port=1。

顶层可选 reference_ohms=50、source、reflection、source_noise、intrinsic_noise。两个噪声对象必需 covariance，可选 complementary。所有矩阵行列顺序遵循 channels，复数编码为 [实部,虚部]。未知字段、重复 JSON 键、布尔索引或不合法数值会被拒绝。

~~~powershell
python -m rfmodel examples/mixer-image-noise.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/mixer-image-noise-result.json
~~~

示例 LO 为 1 GHz，0.8/1.2 GHz 两个 RF 通道各有 1e-20 W/Hz 的独立噪声。两者均转换至 0.2 GHz，该 IF 的出射噪声为 2e-20 W/Hz；移除镜像 RF 噪声后为 1e-20 W/Hz。1.8/2.2 GHz 上边带也明确保留。它是解析案例，不是 SystemVue 实测噪声系数结论。

CLI 输出保留通道身份、频率、入射/出射复波、出射信号功率、完整 C/P 和残差。运行失败不会覆盖已有结果文件。

## C++ 与 C ABI

C++ 入口为 FrequencyConversionModel::analyze 和 ideal_mixer_conversion（frequency_conversion.hpp）。C API 为 rfmodel_conversion_analyze、rfmodel_ideal_mixer_conversion。C 请求结构定义通道、A/B、源、Γ 和 C/P，输出结构指定两个 N 元素波缓冲区、两个 N² 元素噪声缓冲区及残差地址。

C 输入允许彼此重叠，输出必须与全部输入、两个描述结构及其他输出互不重叠。所有输出缓冲区必需，容量以复数元素计。参数、容量、重叠、统计量或求解失败均不修改任何输出。空可选输入指针表示全零；direct/conjugate 在 C 中均需提供。

## 验证范围与后续工作

原生测试用既有实数混频算法验证波幅相与 DC，以确定性实基向量验证全矩阵 C/P，并覆盖反馈与统计量非法输入。Python 专项另验证两个边带汇入同一 IF、边带间互补相关、DC 噪声、复反射及内生噪声的独立标量闭式解、双向跨频率往返反馈、B=0 退化和 CLI 失败保留结果。

单块接口处理给定的整体转换矩阵及各通道边界；[多器件转换网络](conversion-network.md) 已提供物理连接装配、线性器件接入与联合 C/P 求解。频率通道自动扩展、泵驱动依赖、转换矩阵测量/标定、非线性压缩工作点、LO 相噪及 SystemVue Mixer 参数/数值验收仍待实现。已有相干前馈系统图未隐式改变为该求解器。

## 工程验证记录（2026-10-09）

- MSVC 2022 x64 Debug/Release 清理重建，完整 CTest 各 94/94 通过。
- 安装后的独立 C/C++ 消费者在两个配置下各 2/2 通过。
- 独立 Python 3.12 从离线 wheel 导入，转换专项 11/11、API 85/85、系统图 51/51、线性噪声 9/9 通过。
- 127 个 C/C++ 文件格式检查及 git diff --check 通过。
- 本阶段为解析和工程验证；跨平台结果以对应提交的 GitHub Actions 为准，SystemVue Mixer 实测兼容仍未验收。
