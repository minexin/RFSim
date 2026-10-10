# 共同限幅高阶放大器的工作点导数

`highorder_linearization.hpp` 将既有 `CoherentHighOrderAmplifier` 的公共基波压缩、输入限幅和二至十一阶生成项接入通用 C++ 工作点求解器。输入是每个物理端口/频点的一份已合成复波；通过 `generating_bins` 显式选择哪些输入频点参与本级新产物生成。其余 RF 输入仍参与公共功率，并以同一直接增益传播。

该阶段提供 C++ 核心与安装 SDK，专用 C/Python/JSON 网络类型尚待接入。已有高阶前馈系统图接口和来源项计算保持不变。数学回归证明的是既有近似模型的组合与导数，不能替代 SystemVue RFAMP_HO 系数、限幅、MaxOrder 或反馈行为的实测验收。

## 固定模型方程

对所有输入端口的正频 RF 波求 P=sum(abs(a_j)^2)，r=sqrt(P)。公共基波增益为 g(P)，公共限幅比例为 s(P)。仅将选定生成频点的输入乘 s，再以原电压多项式产生 RF 输出：

```
z_j = s(P)*a_j                   (j 属于 generating_bins)
F_i = g(P)*a_i + H_i(z, conj(z)) (没有同频输入时直接项为零)
```

H 包含二至十一阶全部落在正频输出上的产物，包括重新落入输入载频的项。DC 按既有 RF 高阶模型阻断，不能和完整 DC 电压多项式的行为混用。输入、输出侧 DC 通道可以声明，但其入射波必须严格为零，输出和导数均为零。输出侧 RF 入射不参与驱动。

g 沿用 P1dB 以下三次基波、以上增量 tanh 饱和。限幅的膝点/极限输入功率继续使用既有 `(OP1dB-Gain-34)` 与 `(OPsat-Gain-31)` 的 dBm→W 指数公式；对应的 -4/-1 dB 偏移来自既有经验识别，未据此新增厂商兼容结论。

`CoherentHighOrderAmplifier` 增加只读 `fundamental_response(P)`、`limiter_response(P)`、`voltage_coefficients()` 和 `reference_ohms()`。`SoftInputLimiter::gain_response(P)` 返回比例 s、径向斜率 d(r*s)/dr 和两者之差。饱和段用 `4*exp(-2u)/(1+exp(-2u))^2` 计算斜率，避免 tanh 已舍入为 1 时过早失去可表示的导数。旧 nominal/limit/evaluate 行为不变。

## 公共功率的链式导数

令 Dg=2P*dg/dP、Ds=2P*ds/dP，u_j=a_j/r。直接基波项使用：

```
A_direct[i,j] = g*delta_ij + (Dg/2)*u_i*conj(u_j)
B_direct[i,j] =              (Dg/2)*u_i*u_j
```

多项式在已限幅工作点 z 上返回直接/共轭矩阵 Ah/Bh。定义：

```
T_i = sum_generating(Ah[i,k]*u_k + Bh[i,k]*conj(u_k))
A_generated[i,j] = s*Ah[i,j] + (Ds/2)*T_i*conj(u_j)
B_generated[i,j] = s*Bh[i,j] + (Ds/2)*T_i*u_j
```

第一项只存在于被选为生成源的输入列；公共功率项对所有 RF 输入列存在。因此，传播中的失真虽然不直接生成新高阶项，其扰动仍会改变公共压缩/限幅，从而影响载波和新生成谐波。P=0 使用 g 的小信号极限、s=1 和零公共耦合，避免除零。

该组合保持一般仿射偏置 F-Aa-Bconj(a)，由通用工作点求解器生成；不将其简化成纯多项式或双线性公式。

## 频点和来源契约

- 输入、输出物理端口默认为 0/1，支持显式更改；参考阻抗取模型构造值。
- 每个 RF 输入必须有同频输出通道，供直接传播。
- `generating_bins` 必须是输入正频点的无重复子集。空列表明确表示只传播直接项。
- 输出必须包含所选生成频点的所有可能正频产物，即使工作点为零。内部多项式仍计算 DC 以保持准确闭合，再明确投影到 RF；用户不必为这个被阻断的 DC 声明输出。
- 总物理通道数遵守 512 限制。内部用于多项式链式导数的输入/完整输出通道（包括内部 DC）也遵守 512 限制；频谱和工作量限制沿用多项式核心。

每个频点只有一份物理复波，模型选择的是“这个频点是否生成新项”。它不保存同频不同来源之间的身份，也不能把同一频点上的载波贡献和传播失真分别选取。需要来源表达式、独立源功率和历史贡献时，仍使用现有相干系统图；两类数据不能未经解析合并便相互替代。

## C++ 接口

```cpp
#include <rfmodel/highorder_linearization.hpp>
using namespace rfmodel;

const CoherentHighOrderAmplifier amplifier(20., 20., 23., {0., .1});
const std::vector<ConversionChannel> channels{{0, 1}, {1, 1}, {1, 3}};
const auto local = linearize_highorder_amplifier(
    1e6, channels, {.01, 0., 0.}, amplifier, {1});
// local.outgoing[2] = 2.5e-6 sqrt(W)，此处限幅尚未启动。

ConversionNonlinearDevice device{
    0, [=](const std::vector<Complex>& incident) {
        return linearize_highorder_amplifier(
            1e6, channels, incident, amplifier, {1});
    }
};
// device 可直接传给 solve_conversion_operating_point。
```

模型参数与生成频点集合在一次求解中固定。噪声由器件源/内禀 C/P 显式给定；此适配提供确定性响应及一阶导数，不推断器件噪声系数或随机高阶乘积。

## 数值验证

原生专项覆盖限幅膝点前后与深饱和导数、二/三/五/十一阶、单/双生成频点和空生成集合，在零驱动、膝点、P1dB、压缩区对比旧高阶模型逐项复波求和及每个实/虚方向的中心差分。时间平移按每个输入频率加相位，输出按实际生成频率旋转，验证和差频共轭关系。

传播失真单独携带噪声的测试用旧响应差分构造独立 C/P 预期，检验公共限幅对新生成三次谐波的噪声作用。物理负反馈测试独立实现三次/tanh 基波与输入限幅曲线，以二分根核对工作点；在实际接受的工作点按径向/切向闭环公式检查基波、谐波及二者相关噪声。

零点缺失频点、重复或不存在的生成频点、非零 DC、空系数退化、极大驱动也有专项检查。安装 C++ consumer 直接使用新增公开头文件。

## 工程验证记录

2026-10-10：MSVC Debug/Release clean-first 构建成功，CTest 各 117/117；两种配置安装后的独立 C/C++ consumer 各 2/2。147 个 C/C++ 文件格式检查及 git diff --check 通过。

本阶段未修改 C ABI/Python 源码。独立 Python 3.12 使用上一阶段 wheel 连接本阶段安装的 Release DLL，15 组共 293 项旧接口回归通过（多项式工作点 15、压缩放大器 14、既有工作点 13、仿射 11、混频线性化 13、共享相噪 13、相噪 13、通道测量 11、加载噪声 9、变频 NF 12、转换网络 13、单器件变频 11、API 85、相干系统 51、线性噪声 9）。新增组合适配本身由原生专项与安装 C++ consumer 验证。

本轮检查发现 SystemVue 和原采集进程均已退出，旧采集文件仍为空；该次采集没有产生有效厂商数据。重新实测需要从已知工作区基线启动新采集，不能将此前超时状态视为成功或恢复完成。
