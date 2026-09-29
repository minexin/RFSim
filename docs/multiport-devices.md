# 理想功分/合路器和正交耦合器

`rfmodel/multiport_devices.hpp` 提供三个实现 `RFDeviceModel` 和
`SParameterProvider` 的多端口器件。矩阵可直接交给 `LinearNetwork`，支持带反射负载的
任意网络连接；热噪声由已有 `passive_thermal_noise(s, temperature_k)` 显式组合。
所有端口采用同一个正实数参考阻抗，频率为非负有限 Hz。

## 等分功分/合路器

`EqualPowerDividerModel(name, branches, excess_loss_db=0, reference_ohms=50)`
支持 2–64 个支路。端口 0 是公共端，1 至 branches 为匹配且相互隔离的支路。
额外损耗为非负 dB，不包含等分产生的 `10*log10(branches)` dB。
公共端到每个支路的双向幅度为 `a=10^(-excess_loss_db/20)/sqrt(branches)`，
其余 S 参数均为零。端口名称为 `port0`、`port1` 等。

合路遵循复数波叠加：两路等幅同相输入合成，两路反相输入被隔离网络吸收。
因此零额外损耗不代表整个多端口无耗，也不能用一个互易、匹配、无损的三端口模型
同时满足这些约束。双支路零额外损耗在温度 T 下的内生噪声为：
公共端零、两个支路各 `kT/2`、支路间相关项 `-kT/2`。

## 可配置幅相的隔离功分器

`IsolatedPowerDividerModel(name, branch_transmissions, reference_ohms=50)` 接受
2–64 个复数波幅传输系数，依次对应公共端 0 到各支路的 S 参数。反向系数相同，
不会取共轭；其余矩阵元素为零。各支路功率之和必须不超过 1（仅允许双精度求和误差），
不符合无源条件时拒绝构造，不自动归一化。零传输支路允许存在。

该接口支持不等功率分配及独立输出相位。相干合路需在输入波中补偿相位，不能只相加功率。
若传输向量为 v，则支路噪声子矩阵为 `kT*(I-v*v^H)`，公共端噪声为
`kT*(1-sum(abs(v)^2))`；复相位会产生复数跨端口噪声相关项。

SystemVue 参数映射的已知差异见 [多端口模型评估](systemvue-multiport-assessment.md)。

## 四端口正交耦合器

`QuadratureCouplerModel(name, coupled_power_fraction, excess_loss_db=0, reference_ohms=50)`
中耦合功率比例 q 的范围为 [0,1]，并非 dB 参数。
令 `A=10^(-excess_loss_db/20)`，`t=A*sqrt(1-q)`，`c=j*A*sqrt(q)`，矩阵为：

```text
    0  1  2  3
0 [ 0  t  c  0 ]
1 [ t  0  0  c ]
2 [ c  0  0  t ]
3 [ 0  c  t  0 ]
```

从端口 0 输入时，1 是直通端、2 是 +90 度耦合端、3 是隔离端。
所有端口匹配，矩阵互易，零额外损耗时幺正。统一非零损耗对应的内生噪声为
`kT*(1-A*A)*I`。端口号及正交相位约定是本 API 的约定，需要在导入厂商模型时核对。

```cpp
rfmodel::EqualPowerDividerModel divider("four way", 4, 0.5);
rfmodel::QuadratureCouplerModel hybrid("90 degree hybrid", 0.5);
rfmodel::LinearNetwork network;
network.add(hybrid.s_parameters(1e9));
network.terminate(1);
network.terminate(2, 1.); // Open the coupled port.
network.terminate(3);
auto input = network.external_s({0}); // S11 = -0.5.
```

## 验证与剩余范围

解析测试检查分配功率、相干合路及反相吸收、负载反射的往返相位、互易性、
耦合器完整矩阵幺正性和损耗、功分器噪声的跨端口相关性，以及输入校验。
安装包消费测试覆盖两个类的公开头文件和端口约定。

当前是频率无关的理想原语；尚未实现有限隔离度、有限方向性、幅相不平衡、频变损耗、
物理带宽和尺寸参数。SystemVue 2023 对应器件的参数映射及数值比对尚未执行，
RF Design 目录条目仍保持未验收，不能由这些解析测试推导出名称级兼容。

2026-09-28：MSVC Debug/Release 全套 CTest 各 39/39，安装消费测试各 1/1，
70 个 C++ 文件通过格式检查。本次多端口实现的跨平台 CI 需在推送后另行核验。

2026-09-29：新增复数支路系数模型，验证不等功率、相位补偿合路、正交输入吸收、
复数噪声相关及无源约束。MSVC Debug/Release 全套仍各 39/39 通过。
