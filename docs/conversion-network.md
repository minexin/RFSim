# 多器件频率转换网络

本阶段将固定泵转换矩阵、普通线性器件和物理端口接线装配为同一个频域方程，支持反射往返与完整出射噪声 C/P。C++17、C ABI、Python、JSON/CLI 均有入口。它扩大了 RF System Analysis 的联合信号/噪声能力；尚未通过 SystemVue Mixer 实测验收。

## 方程与接线

各器件局部关系为 b=Aa+B conj(a)+c，通道身份是 (物理端口, 非负频率 bin)，实际频率为 bin × spacing_hz。全网使用一个有限正实参考阻抗（默认 50 Ω）及一个正频率间隔。所有器件合计不超过 512 个通道。

一条物理接线连接两个端口的全部频率通道：a_left=b_right、a_right=b_left。两端 bin 集合必须完全相同，局部排列可以不同。每个物理端口只连接一次，不允许自连同一端口；允许连接同一器件的不同端口。未接线通道采用 a=Γb+s。所有接线通道的 s、Γ、源噪声 C/P 对应行列必须严格为零；器件内生噪声可以位于接线通道。

装配后的实正交分量方程为 (I-MR)b=M s+c。其中 M 包含各器件直接/共轭转换，R 包含物理连接置换及外部反射。统一求解保留所有已声明通道的往返反馈，不按前向增益逐级近似。奇异方程、非有限结果和超限残差会报错；有代数解并不证明动态稳定。

局部噪声使用 C=E[n n†] 和 P=E[n nᵀ]。每个器件内部的跨端口、跨频率相关性保留，器件之间的内生噪声独立；外部源噪声也按器件独立，且与内生噪声独立。经全网传播后，各器件出射波通常存在非零互相关。统计量必须对应有效的实对称半正定正交分量协方差，DC 必须为实数。完整定义见 [转换矩阵接口](frequency-conversion.md)。

## 普通线性器件接入

C++ lift_linear_conversion 接收递增的非负 bins、每个频点的 SMatrix 和 NoiseCorrelation。每个频点的 S 写入直接矩阵 A 的对角分块，B=0；平稳噪声不同频点相互独立。正频率 P=0，DC 使用 P=C。通道按 bin 优先、端口其次排序。

JSON linear 模型支持静态 S 或现有参数模型（包括 Touchstone、RLC、传输线、滤波器和功分/耦合模型），复用原有频点采样及噪声验证。没有自动频率扩展：用户必须明确给出连接所需频率集合，遗漏混频产物或两端集合不同会失败。

## 接口

C++ 头文件为 rfmodel/conversion_network.hpp：

~~~cpp
FrequencyConversionNetwork network(spacing_hz, reference_ohms);
const auto input_id = network.add(input_device);
const auto mixer_id = network.add(mixer_device);
network.connect(input_id, 1, mixer_id, 0);
const auto result = network.analyze();
~~~

ConversionDevice 包含 FrequencyConversionModel、source、reflection、source_noise、intrinsic_noise，向量和矩阵均须与局部通道数匹配。add(FrequencyConversionModel) 可创建零源、匹配边界、零噪声器件。channel_offset(device) 返回该器件在全网输出中的起点。

C 入口 rfmodel_conversion_network_analyze 接收一个 rfmodel_conversion_request 数组和 rfmodel_conversion_connection 数组；连接描述使用设备数组下标及物理端口号。输出沿用 rfmodel_conversion_output，两个波数组容量至少 N，两个噪声数组至少 N²。输入描述、连接描述和全部输入数组均不得与输出重叠，输出之间也不得重叠；任何失败均不改写输出。每个请求的 spacing_hz/reference_ohms 必须完全一致。原单器件 C 入口复用同一实现。

Python Library.conversion_network(spacing_hz, devices, connections=(), reference_ohms=50) 接收：

- devices：每项必需 channels=[(port,bin),...]、direct；可选 conjugate、source、reflection、source_covariance、source_complementary、intrinsic_covariance、intrinsic_complementary，省略项为零。
- connections：[((device_index,physical_port),(device_index,physical_port)),...]。
- 返回 ConversionResult，所有波和噪声矩阵按设备声明顺序、再按设备局部通道顺序排列。

## JSON 契约

顶层必需 format="rfmodel.conversion-network"、version=1、spacing_hz、devices、boundaries；可选 reference_ohms=50、connections=[]。每个设备 id 是唯一非空字符串。

设备有两种形式：

- 转换设备：必需 id、channels（{port,bin} 列表）、model、noise。model 为已有 matrix 或 ideal_real_mixer 格式。noise 必须为 {"noiseless":true}，或包含 covariance、可选 complementary 的局部完整矩阵。
- 线性设备：必需 id、bins、model、noise。model={"type":"linear","s":矩阵}，或 {"type":"linear","device":参数模型}，二者只能选其一。noise 复用线性格式，明确选择 temperature_k、covariance、covariance_samples 或 noiseless。Touchstone 还可显式采用文件内噪声参数，遵循原有 Touchstone 输入约束。

每个设备可选 source_noise，格式与转换设备 noise 相同；用于表达同一器件外部通道之间的相关源噪声。

connections 每项为 [[设备id,物理端口],[设备id,物理端口]]。boundaries 每项必需 channel=[设备id,端口,bin]；可选 source=0、reflection=0、noise_w_per_hz=0。每个未接线通道恰好声明一个边界，接线通道不允许边界条目。noise_w_per_hz 是非负独立源噪声密度，DC 自动令 Pii=Cii。同一个设备不能同时声明 source_noise 和边界 noise_w_per_hz。

复数编码为 [实部,虚部]，实数也可以直接写数字。未知字段、非有限数、布尔索引、重复通道/设备及非法噪声会被拒绝。

~~~powershell
python -m rfmodel examples/filtered-conversion-network.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/filtered-conversion-network-result.json
~~~

示例为 0.8/1.2 GHz RF 滤波器 → LO=1 GHz 的实数混频器 → IF 滤波器。0.2/1.8/2.2 GHz 产物都保留。结果 format=rfmodel.conversion-network-result，包含所有通道的设备身份、频率、连接标志、入射/出射波、出射信号功率、全网 C/P 和残差。C/P 的单位是 W/Hz，波幅为 sqrt(W)。失败不会覆盖已有结果，CLI 同时阻止覆盖模型、动态库及嵌套 Touchstone 输入。

## 验证与边界

解析回归覆盖：独立 Butterworth/Chebyshev 极点公式得到的复幅相；两个温热衰减器夹混频器的信号、热噪声、镜像折叠与跨频率 C/P；不同通道排列的物理连接；DC 实噪声；普通线性网络退化对照；反射往返的波和完整噪声闭式解；严格边界、非法接线及 C 输出原子性。安装消费者实际调用新 C/C++ 网络入口。

仍待实现：自动频率闭包/截谱策略、稀疏大规模求解、跨设备相关输入噪声接口、泵驱动与压缩工作点联立、LO 相噪、转换矩阵标定、宽带噪声后处理及 SystemVue 厂商参数/数值验证。现有相干非线性前馈图未自动迁移到本求解器。此阶段不宣称 RF Design 完整兼容。

## 工程验证记录（2026-10-09）

- MSVC 2022 x64 Debug/Release 清理重建，完整 CTest 各 96/96 通过。
- 安装后独立 C/C++ 消费者在两个配置下各 2/2 通过。
- 独立 Python 3.12 从离线 wheel 导入：网络专项 13/13、单块转换 11/11、公共 API 85/85、系统图 51/51、线性噪声 9/9 通过。
- 129 个 C/C++ 文件格式检查及 git diff --check 通过。
- 跨平台结果以本阶段提交的 GitHub Actions 为准；SystemVue 混频网络实测仍待采集和验收。

[参考温度噪声分析](conversion-noise-analysis.md) 已支持显式信号/热源频带与 SSB/多频带归一化；输出热负载和宽带路径约定仍待补齐。

[热负载与噪声功率流](conversion-loaded-noise.md) 已提供 loaded_noise 可选输出及 noise_temperature_k 边界参数，保留原默认结果和 C ABI。

[channel_measurements](channel-noise-measurements.md) 已提供端口入射/出射方向的带宽噪声积分和显式所需谱线载噪比；频率采样覆盖不足会报错。

可用 [phase_noise_sources](phase-noise.md) 从外部载波边界生成一阶相位相关边带，与现有源噪声独立相加。
