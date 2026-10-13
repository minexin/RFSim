# 独立复参考阻抗与功率波

Linear Analysis 可在每个频率为每个外部端口指定独立复参考阻抗，并一起变换 S 参数和内生噪声相关矩阵。C++、C、Python 与线性 JSON 网络也支持器件端口各自的复参考阻抗及物理连接。输出坐标转换不会改变物理网络。

## 波定义和单位

采用 Kurokawa 功率波。V、I 为 RMS 复相量，电流正向流入端口：

- a = (V + Zref I) / (2 sqrt(Re Zref))
- b = (V - conj(Zref) I) / (2 sqrt(Re Zref))
- 端口净输入功率 = |a|² - |b|² = Re(V conj(I))

每个参考阻抗必须有限且实部严格为正；允许虚部不为零。复参考下不能把这里的 S 参数直接当作伪波或普通行波 S 参数。公式来源及不同波定义可参见 [scikit-rf 官方 Z→S 文档](https://scikit-rf.readthedocs.io/en/latest/api/generated/skrf.network.z2s.html)，其中引用 Kurokawa (1965)。

将波坐标转换写成 a'=A a+B b、b'=C a+D b，A/B/C/D 都是按端口排列的对角矩阵。实现直接求解 S'=(C+D S)(A+B S)^-1，不先转换到 Z；因此理想开路、短路、直通及公共结点仍可重归一化。转换矩阵奇异或数值无法可靠求解时明确报错。

对于 b=S a+c，噪声传递矩阵 T=D-S'B，内生噪声变换为 Cnoise'=T Cnoise T†，单位仍为 W/Hz。不能仅转换 S 而保留原噪声矩阵。

## C++ 与 C ABI

头文件 include/rfmodel/power_wave_reference.hpp 提供：

- renormalize_power_waves(s, old_refs, new_refs)：返回 scattering、noise_transfer。
- renormalize_noise(s, intrinsic, old_refs, new_refs)：返回新参考下的噪声相关矩阵。
- s_to_z(s, refs)、s_to_y(s, refs)：得到物理 Z（Ω）或 Y（S）。开路的 Z、短路的 Y 等奇异情况报错。

refs 为 std::vector<Complex>，长度等于端口数。analyze_linear 的第五个可选参数是 output_references(frequency_hz)，返回当频点按 external_ports 顺序排列的参考阻抗；未提供时保持既有输出。

C ABI 对应 rfmodel_power_wave_renormalize、rfmodel_power_wave_s_to_parameters。矩阵按行排列，长度 N²，参考数组长度 N，1≤N≤1024。噪声输入和输出同时为空或同时提供。输出缓冲区不能与任一输入或另一输出重叠；只读输入允许共用。容量不足、非法数值、噪声非半正定、奇异转换均返回错误，输出保持不变。

## Python 和 JSON

~~~python
changed = library.renormalize_power_waves(
    scattering, [50, 50], [25 + 10j, 100 - 15j], noise=covariance
)
physical_z = library.power_wave_parameters(
    changed.scattering, [25 + 10j, 100 - 15j]
)
~~~

噪声可省略，此时 changed.noise_correlation 为 None。admittance=True 选择物理 Y。

线性 JSON 支持互斥的两个可选字段：

| 字段 | 形状和顺序 |
|---|---|
| output_reference_impedances_ohms | 长度 N；所有频率复用，顺序与 external_ports 相同 |
| output_reference_samples_ohms | M×N；先频率，后 external_ports |

复数仍用 [实部, 虚部]。不接受 null、缺少频点/端口或非正实部。示例 examples/complex-reference-network.json 包含两个频点的不同复参考阻抗。

请求转换时，每个结果频点增加 port_impedances_ohms 和 wave_definition="power"，s 与 noise_w_per_hz 使用该参考。未指定器件独立参考时，顶层 reference_ohms 表示内部网络的公共参考。此时 signal 和 loaded_noise 是实际边界下、公共参考坐标中的结果，分别增加 reference_ohms 说明；它们不随输出参考选择改变。使用器件独立参考后的逐端口结果标注见下文。未请求转换时，既有结果格式保持不变。

## 验证与兼容边界

新增回归覆盖解析复负载、理想开短路、二端口直通、三端口公共结点、非互易矩阵、V/I 与净功率一致性、噪声传递、kT(I-SS†)、往返恢复、端口重排、逐频率参考、奇异输入及 C 缓冲区失败原子性。C/C++ 安装消费和 Python wheel 验证随本阶段执行，结果以本次工程验证记录为准。

这补齐 RFModel 的独立复参考输出表达能力。SystemVue 2023 的 ZPORT 波定义、复参考 S 和 CS 的单位/数值对照仍待实测；本阶段不宣称这部分已与厂商一致。内部线性混合参考接线及跨语言输入见下节；RF 相干/频谱线性阶段的后续接入见[阶段说明](power-wave-stages.md)，变频系统图的独立复参考仍未扩展。复参考噪声参数后处理由[二端口噪声接口](power-wave-noise.md)提供。

## 本阶段工程验证（2026-10-09）

- MSVC 2022 x64：Debug/Release clean-first 构建完成，CTest 分别 85/85。
- 安装目录 build-install-capi，独立 tests/consumer：Debug/Release 各 2/2，覆盖新增 C++ 头文件与 C 导出符号。
- 独立 Python 3.12 从 build-reference/power-wave-dist 的 wheel 加载：API 82/82，系统图 51/51。
- wheel 命令行运行 examples/complex-reference-network.json，输出两个频率、功率波标记和独立复参考数组，检查通过。
- C++ 格式检查 118 个文件通过。跨平台 CI 由本次提交触发，其结论以 GitHub Actions 为准。
- 工作目录保留另一项尚未实测成功的 SystemVue 高阶模型采集草稿；该草稿不包含在本阶段提交中，也未作为上述复参考阻抗验证证据。

## 原生内部混合参考接线（2026-10-12）

`LinearNetwork::add_power_waves(s, references)` 接收每端口一项 `Complex` 参考阻抗。实部须为正，实部、虚部均须有限；S 必须已经用这些 Kurokawa 功率波坐标表示。该方法不把输入当作 50 Ω S 自动换算。它返回首个全局端口索引，仍受整个网络 1024 端口限制。`port_reference_impedance_ohms(port)` 返回单个端口的实际参考。

原有 `add(s, reference_ohms)` 继续要求与构造时的公共正实参考一致。`reference_impedance_ohms()` 只有全部端口都使用该公共参考时才返回；混合参考或独立复参考时抛错，防止依赖统一电阻的相干/谱分析误用新网络。原生类布局已改变，下游 C++ 消费者须重新编译。

`connect(p,q)` 表示理想导线：电压相等、电流（正向流入各自器件）互为相反数。在端口原有坐标中，记 Rp=Re(Zp)、Rq=Re(Zq)、D=conj(Zp+Zq)，连接约束为：

~~~text
ap = (conj(Zq)-Zp)/D * bp + 2 sqrt(Rp Rq)/D * bq
aq = 2 sqrt(Rp Rq)/D * bp + (conj(Zp)-Zq)/D * bq
~~~

这些式子直接由 `V=(conj(Z)a+Zb)/sqrt(Re Z)`、`I=(a-b)/sqrt(Re Z)` 消元得到。相同正实参考时退化为交换波；相同但非实的复参考一般不能直接交换。实现缩放阻抗以避免直接相加和乘积溢出，非有限连接系数在改变边界前报错。

信号方程使用完整连接矩阵 K：`(I-KS)a=e`；内生噪声使用同一 K：`(I-SK)b=c`。连接本身没有附加噪声或损耗。`solve()` 中的 incident/outgoing、`external_s()` 和 `external_noise()` 均保留每个端口的原始局部坐标；内生相关矩阵及跨器件相关项也必须以这些坐标提供。`external_noise_contributions()` 继续表示各器件噪声相互独立的分项。

`terminate(p, reflection, source)` 仍是 `ap=source+reflection*bp`，参数须在端口的局部坐标定义。对于接在端口 p 的物理负载 ZL，reflection 为 `(ZL-Zp)/(ZL+conj(Zp))`，因此复参考下 reflection=0 对应 ZL=Zp。此接口不隐式添加终端热噪声。

`analyze_linear()` 现在按所选外部端口顺序记录实际参考阻抗，并从这些参考转换到可选 output_references；S 与噪声同步转换。逐频率构建回调可以提供不同的局部参考。未使用新方法的既有网络行为不变。

`tests/mixed_reference_network_test.cpp` 覆盖四端口独立物理 Z 方程、连接电压电流与净功率守恒、先重归一化再接线的独立对照、非互易器件、复失配源/负载、双端激励、理想直通的奇异 Z、同复/共轭/不同实参考、无源热平衡和噪声分项、输出端口重排、线性分析参考标注及失败输入。C++ 安装消费者实际调用新接口。

这是 A05 的原生物理接线实现，后续跨语言接口见下一节。SystemVue ZPORT/CS 对照、通用器件 DC 偏置和线性化仍待完成，不能据此关闭 A05。

### 本次原生接线工程验证

[2026-10-12 机读记录](../validation/mixed-reference-network-validation-20261012.json)保存源码、日志、安装库和 wheel 的哈希。Windows MSVC 与 Ubuntu WSL2 GCC 均为 Debug/Release clean-first 全目标构建：四组全量 CTest 各 130/130、独立安装 C/C++ 消费者各 2/2。四组安装库分别加载既有未改动的 wheel，全部 23 组 379 项通过、零跳过；这份历史记录验证既有公共参考接口，当时新混合参考输入仅在 C++ 暴露。156 个 C/C++ 文件格式检查通过。

Linux 构建目录位于原生 /tmp，最终构建与消费者日志没有共享盘时间戳告警。原有八份未提交文件哈希保持不变，CTest 仍包含这些参考脚本草稿，不能称为纯净提交检出。未新增 SystemVue 分析、未推送、未执行远程 CI 或 macOS 验证。

## C/Python/JSON 内部混合参考接口

C ABI 增加 `rfmodel_network_add_power_waves(network, ports, values, value_count, references, reference_count, first_port)`，S 为 N² 个逐行排列的 rfmodel_complex，references 为 N 项，均复制到句柄。新函数失败时网络和 first_port 不变；first_port 不得与输入或句柄重叠。旧结构体布局及 ABI 版本 1 保持不变。

`rfmodel_network_get_references(network, references, capacity)` 返回所有全局端口的实际参考，顺序与 add 一致。先用 port_count 查询长度；capacity 以复数元素计，超出实际端口的尾部不写。空网络可传 NULL/0；非空网络容量不足、NULL、容量溢出或输出与句柄重叠均报错且不写输出。已有 solve/external_s/external_noise 返回端口局部坐标；输出 S 和噪声不会隐式归一到构造时的公共电阻。

Python 对应：

~~~python
with library.network(50) as network:
    network.add_power_waves([[0, 1], [1, 0]], [50, 25])
    network.add_power_waves([[0, 1], [1, 0]], [100, 50])
    network.connect(1, 2)
    assert network.reference_impedances_ohms == (50, 25, 100, 50)
    scattering = network.external_s([0, 3])  # [[0.6, 0.8], [0.8, -0.6]]
~~~

新 Python 包需要包含上述新增导出符号的原生库，ABI 数字仍为 1 不表示旧库已提供新入口。

线性 JSON version=1 的每个 device 新增两个互斥可选字段：

| 字段 | 契约 |
|---|---|
| reference_impedances_ohms | N 项；每项为实数或 [实部, 虚部]，所有频率复用 |
| reference_samples_ohms | M×N，先按 frequencies_hz、再按器件本地端口排列 |

未指定的器件继续采用顶层 reference_ohms。参考须有限且实部为正，不能为 null、布尔值、缺项、变长或同时提供两种字段。端口数从 S 或实际模型得到。求解前校验全部参考频点。

输入的坐标解释如下：

- 显式 s/s_samples 已经在器件声明的局部参考下，不再次转换。
- 参数 model（含 Touchstone）先按顶层 reference_ohms 求值，再用原生重归一化转换到指定局部参考。Touchstone 内嵌噪声与 S 一起转换。
- 显式 device.noise.covariance/covariance_samples 和顶层 intrinsic_noise_samples 使用局部参考，后者也覆盖跨器件相关项。它们不因来自 parameter model 而自动转换。
- 顶层或器件温度模式从局部 S 生成热噪声；noiseless 仍为零噪声。原有噪声模式互斥规则不变。
- terminations、signal_boundaries 和 noise_boundaries 的反射/波激励使用相应端口的局部参考。阻抗随频率变化时，固定反射值是固定波比，并不表示固定物理负载。

只要请求了器件参考，每个结果频点的 s/noise_w_per_hz 就附带 external_ports 顺序的 port_impedances_ohms 和 wave_definition="power"。每个 signal.ports 条目额外带 reference_impedance_ohms；loaded_noise 带外部端口局部 port_impedances_ohms。signal 和 loaded_noise 也标为 power 波。顶层 reference_ohms 此时表示缺省及参数模型求值参考，不能用来替代这些实际参考。

可选 output_reference_* 只转换最终提取的 S 和内生噪声，信号波、已加载噪声及其参考标签保持原坐标。noise_analysis 使用最终输出坐标；其未显式给定的物理源阻抗仍默认为顶层 reference_ohms，并非随输出参考改变。

可运行输入见 [mixed-reference-network.json](../examples/mixed-reference-network.json)：两个串联电阻在两个频率使用不同内部复参考，同时计算信号和加载噪声。新测试还覆盖显式局部 S、跨器件复相关噪声、Touchstone 噪声/NF、内部物理负载、输出重归一化、CLI 失败保护和新 C ABI 缓冲区错误。

后续[RF 线性阶段](power-wave-stages.md)已支持常量局部复参考，并在阶段边界转换回公共实参考；新增网络快照可供既有传播接口使用。原传播入口直接接收混合参考网络仍会拒绝，变频图跨语言独立复参考尚未接入。SystemVue ZPORT/CS 与通用 DC 线性化仍待验收，A05 继续开放。

### 跨语言接口工程验证（2026-10-12）

[本阶段机读记录](../validation/mixed-reference-api-validation-20261012.json)保存源码、构建/测试日志、安装库、新 wheel 与实际 CLI 示例输出的 SHA256。Windows MSVC 和 Ubuntu WSL2 GCC 均执行 Debug/Release clean-first 全目标构建；四组 CTest 各 132/132、安装 C/C++ 消费者各 2/2。新 wheel 从独立安装目录加载对应原生库，四组各通过 24 组 392 项、零跳过，其中混合参考新套件 13 项，每组核查 66 次 CLI 调用的安装来源。

安装后的 Windows Debug CLI 运行公开例子，两个频点的 S21=0.5，并从输出的局部波与参考重建电压电流，连接连续性误差小于 1e-12。157 个 C/C++ 文件格式检查通过。原有八份未提交文件哈希未变；全量 CTest 包含这些参考脚本草稿，不能当作纯净提交检出。没有新增 SystemVue 分析、推送、macOS 或远程 CI 执行。

后续[变频网络逐通道复参考](conversion-power-wave.md)已支持 A/B、偏置和 C/P 联合转换、内部物理接线及 C/Python/JSON 显式参考输入。[混合参考非线性工作点](conversion-operating-point-power-wave.md)提供独立原生 C++ 入口；既有及跨语言自动工作点仍要求公共实参考。

变频复参考阶段还修正了大阻抗比下内生噪声传递的相消：普通 S 变换的噪声系数改用逆坐标方程直接求解；既有 C/Python 线性重归一化调用同一原生实现。参见[数值契约及独立回归](conversion-power-wave.md)。
