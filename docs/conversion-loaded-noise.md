# 变频网络的热负载与噪声功率流

本阶段在已有出射噪声 C/P 的基础上，按需返回入射噪声、入射—出射交叉统计量和每个通道的净吸收噪声密度。普通线性器件、固定泵直接/共轭转换、物理接线、反射反馈及热负载使用同一套方程。C++、C、Python、JSON/CLI 均可使用。

## 统计量和方向

所有统计量使用功率波，单位 W/Hz，通道顺序为设备声明顺序、再按设备局部通道顺序：

- Caa=E[a a†]、Paa=E[a aᵀ]：入射噪声。
- Cbb=E[b b†]、Pbb=E[b bᵀ]：原有出射噪声。
- Cab=E[a b†]、Pab=E[a bᵀ]：入射—出射交叉噪声，行对应入射通道，列对应出射通道。
- p_i=Caa(i,i)-Cbb(i,i)：净进入该设备端口的噪声密度。正值流入设备，负值流向其连接对象或边界负载。

Cab 一般不是 Hermitian，Pab 一般也不是对称矩阵，不得单独进行 PSD 投影。它们保留源噪声既直接进入端口、又经网络反射返回所产生的相关性；不能把入射噪声近似成 |Γ|²Cbb。

在共同正实参考阻抗 Z0 下，端口电压噪声密度可由 Z0*(Caa+Cbb+2Re(Cab)) 的对角项得到，电流噪声密度对应 (Caa+Cbb-2Re(Cab))/Z0。这也说明交叉项的方向和共轭定义必须明确。

## 联合传播

在实正交分量形式下，网络满足 b=M a+c、a=R b+s。其中 R 是连接置换及边界反射，s 是外部源发射噪声，c 是器件内生噪声，两组独立，各自内部可相关。

~~~text
T = (I-MR)^-1
J = T M
U = I + R J
V = R T

b = J s + T c
a = U s + V c

Qbb = J Qs Jᵀ + T Qc Tᵀ
Qaa = U Qs Uᵀ + V Qc Vᵀ
Qab = U Qs Jᵀ + V Qc Tᵀ
~~~

最终把实 Q 转回复 C/P。该方法同时保留反射、跨频率共轭及源/反射波相关性。DC 的虚分量严格为零，实 DC 的 C=P。原有两组噪声独立的限制不变，不自动引入跨器件相关噪声。

连接通道 i、j 满足 a_i=b_j、a_j=b_i，因此 p_i+p_j=0。普通被动网络在器件与所有终端等温时，每个外部端口的净噪声功率应为零。固定泵混频器可以与泵交换能量，不能要求所有变频通道的净噪声密度之和为零，也不能直接对转换矩阵套用普通无源 S 参数的热噪声公式。

## 接口

C++：FrequencyConversionNetwork::analyze(true) 开启附加统计量；单块 FrequencyConversionModel::analyze 的最后一个 loaded_noise 参数也可置 true。ConversionResult 新增 incident_noise、incident_outgoing_noise、net_noise_into_device_w_per_hz。默认 false 时这些字段为空，原有出射求解和参考温度 NF 不承担额外矩阵传播成本。

C：rfmodel_conversion_network_analyze_loaded 沿用设备/连接数组及原 rfmodel_conversion_output，另接收 rfmodel_conversion_loaded_output。附加描述包含四个 N² 复数矩阵缓冲区、一个 N 元素 double 功率缓冲区和相应容量。所有普通/附加输出必须互不重叠，也不能覆盖任何输入或描述结构。任何参数、噪声或求解失败均保留全部输出。旧 C ABI 结构及入口不变。

Python：

~~~python
result = library.conversion_network(
    spacing_hz, devices, connections, loaded_noise=True,
)
~~~

返回 ConversionLoadedResult，保留原结果的前五个字段，并新增 incident_noise_covariance、incident_noise_complementary、incident_outgoing_noise_covariance、incident_outgoing_noise_complementary、net_noise_into_device_w_per_hz。默认仍返回原 ConversionResult。

## JSON 和热边界

rfmodel.conversion-network 顶层可选 loaded_noise=true，必须是布尔值。开启后输出四个附加矩阵（字段名为 Python 名称加 _w_per_hz），每个通道同时输出 incident_noise_w_per_hz、outgoing_noise_w_per_hz、net_noise_into_device_w_per_hz。原 noise_covariance_w_per_hz/noise_complementary_w_per_hz 和确定性波不变。

每个边界新增可选 noise_temperature_k。它与 noise_w_per_hz 二选一，也不能和该设备的 source_noise 同时指定。温度必须有限且非负，反射必须被动 |Γ|≤1。该边界发射 kT*(1-|Γ|²) 的噪声，DC 同时设置 Pii=Cii；正频 Pii=0。温度不是净吸收功率，净功率仍需结合器件噪声和反射共同求解。

~~~json
{
  "channel": ["resistor", 0, 1],
  "reflection": [0.3, -0.1],
  "noise_temperature_k": 580
}
~~~

~~~powershell
python -m rfmodel examples/conversion-thermal-load.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/conversion-thermal-load-result.json
~~~

示例中一端口 S=0.5+0.2j，器件温度 Td=290 K，终端 Γ=0.3-0.1j、Ts=580 K。净进入器件的噪声为：

~~~text
p = k*(Ts-Td)*(1-|S|²)*(1-|Γ|²)/|1-SΓ|²
~~~

Ts=Td 时 p=0；降低 Ts 后 p 变负，表示器件向冷终端传递噪声能量。示例是解析验证，不是 SystemVue 实测。

## 验证与后续边界

回归包括：含共轭项和复反射的标量复数逆解、确定性噪声基的完整 C/P 重构、普通两端口对既有 loaded_noise 的退化对照、等温/冷热终端的净功率、物理接线交叉统计和功率守恒、镜像折叠的 Cab/Pab、DC 实噪声，以及 C 全输出原子性、安装消费者和 CLI 失败保护。

本接口提供用户给定热负载下的实际噪声统计，原 reference_noise_analysis 的无噪声输出负载约定保持不变。带热输出负载的 NF 定义/校准、宽带积分、LO 相噪和 SystemVue 路径测量语义仍待完成。没有把通道净噪声自动命名为厂商 CNF、CNP 或 IMGNP。

## 工程验证记录（2026-10-09）

- MSVC 2022 x64 Debug/Release 清理重建，完整 CTest 各 99/99 通过。
- 安装后的独立 C/C++ 消费者在两个配置下各 2/2 通过。
- 独立 Python 3.12 从离线 wheel 导入：热负载专项 9/9、参考 NF 12/12、转换网络 13/13、单块转换 11/11、公共 API 85/85、系统图 51/51、线性噪声 9/9 通过。
- 130 个 C/C++ 文件格式检查及 git diff --check 通过。
- 本阶段证据为解析、退化对照和工程验证；跨平台结果以对应提交的 GitHub Actions 为准。SystemVue 实测热负载/路径测量验收仍待完成。
