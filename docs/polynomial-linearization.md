# 高阶多项式的工作点、谐波与噪声导数

`polynomial_linearization.hpp` 将既有 `MemorylessPolynomial` 的零至十一阶电压多项式接入通用非线性工作点求解器。它同时返回真实 DC/谐波/互调输出和直接、共轭导数，支持在收敛点计算跨频率 C/P 噪声。此阶段提供 C++ 接口与安装 SDK；专用 C/Python/JSON 模型类型尚待接入。

这是明确的无记忆电压多项式模型。它复用已有稀疏频谱计算，不把 RFAMP_HO 的饱和、限幅、IMN 标定或来源追踪规则隐含成同一种行为，也不构成新增的 SystemVue 实测验收。

## 模型及波归一化

器件为匹配、单向的两物理端口模型：输入侧出射为零，输出侧入射不参与非线性驱动。参数允许任意有限实系数，包括非零常数项：

```
y(t) = sum(c[n] * v(t)^n), n = 0..11
```

`c[n]` 的单位是 `V_out / V_in^n`。两个端口使用共同正实参考阻抗 R。若 q[0]=1、q[k>0]=sqrt(2)，则输入电压傅里叶系数 V[k]=sqrt(R)/q[k]*a[k]，输出功率波 b[k]=q[k]/sqrt(R)*Y[k]。DC 的功率波必须为实数；正频点使用复 RMS 功率波，负频点通过共轭重建。

已有 `MatchedPolynomialAmplifier::transmit` 的名义输出保持不变。`MemorylessPolynomial::derivative()` 新增准确的电压导数多项式；导数系数超出浮点表示范围时拒绝。

## 解析直接/共轭导数

在给定工作点 v0(t) 计算 d(t)=f'(v0(t))，记其傅里叶系数为 D[k]，D[-k]=conj(D[k])。链式法则得到 delta_y(t)=d(t)*delta_v(t)。对于正频输入 j、非负输出 k：

```
A[k,j] = q[k]/q[j] * D[k-j]
B[k,j] = q[k]/q[j] * D[k+j]
delta_b = A*delta_a + B*conj(delta_a)
```

对于实 DC 输入 j=0，将该实方向规范地等分到 A/B：两者都取 q[k]*D[k]/2。这样保留 DC 的实数约束，且不引入独立的虚 DC 自由度。公共阻抗在导数的输入/输出归一化因子中抵消，但仍影响工作点电压与高阶响应。

通用求解器继续使用 d=F(a)-A*a-B*conj(a) 形成仿射偏置。混合次数多项式不能使用单一的 -F(a) 偏置；若按次数分解，偏置等于各阶 (1-n)*F_n(a) 的和。

## 输出频点与闭合契约

`polynomial_output_bins(input_bins, polynomial)` 返回该多项式可能产生的非负整数频点，按升序排列。它按每个非零系数的次数，枚举声明输入频点的有符号和；常数项需要 DC，偶次项和互调也可能产生 DC。

所有声明的输入通道都参与推导，即使名义波为零。这样，Newton 试探点、噪声扰动以及未占用通道上的互调不会因当前载波列表为空而被裁掉。`linearize_polynomial_amplifier` 要求输出端口包含这份完整集合；允许额外输出通道，但不允许静默丢弃带外项。

输入、输出不必采用相同频点集合。物理端口接线仍要求两端集合一致，例如将完整谐波输出连接到一个显式滤波网络，再由滤波网络把选定频点反馈到输入。该辅助函数是局部频点规划，不是网络级自适应频率扩展或带截断误差估计的谐波平衡算法。

接口沿用转换网络的总通道上限 512、物理端口编号 0..1023、正间隔和正参考阻抗。结构推导与现有频谱计算采用 4096 个有符号频点、1000 万次组合/乘积的工作上限；频率索引或数值溢出明确失败。

## C++ 使用

```cpp
#include <rfmodel/polynomial_linearization.hpp>
using namespace rfmodel;

const MemorylessPolynomial polynomial({0., 2., 0., -.02});
const auto output_bins = polynomial_output_bins({1}, polynomial); // {1, 3}
const std::vector<ConversionChannel> channels{{0, 1}, {1, 1}, {1, 3}};
const auto point = linearize_polynomial_amplifier(
    1e6, channels, {.1, 0., 0.}, polynomial);
// point.outgoing[1] = .1985；point.outgoing[2] = -.0005，单位 sqrt(W)。

ConversionNonlinearDevice nonlinear{
    0, [=](const std::vector<Complex>& incident) {
        return linearize_polynomial_amplifier(
            1e6, channels, incident, polynomial);
    }
};
// 将 nonlinear 和同通道 ConversionDevice 加入 solve_conversion_operating_point。
```

函数可显式指定 input_port、output_port、reference_ohms，默认 0、1、50。返回的 `ConversionLinearization` 可直接加入通用求解器；噪声仍由器件显式提供的源/内禀 C/P 定义。

## 验证与边界

新增原生专项覆盖：

- 二、三、五、十一阶、DC 偏置、两个复 RF 输入和 1/50/75 欧姆参考；名义输出与既有传输模型以及独立时域采样/傅里叶计算一致。
- 每个输入实/虚方向的解析导数，同时对照独立时域链式法则和中心差分；输出侧入射导数严格为零。
- 平方律整流和二次谐波的解析 C/P、DC 与谐波相关项、相位扰动时 DC 不变而谐波相位倍增。
- 三次模型中未占用输入频点的扰动，与另一个频点的载波形成和/差互调；名义输出为零，但噪声非零，且两互调频点之间保留互补相关。
- 三次放大器和物理滤波反馈联立，独立二分解析根、基波/三次谐波输出、幅度/相位闭环噪声以及基波—谐波 C/P。
- 缺失输出频点在零工作点也拒绝；非法端口、复 DC、重复/负频点、频率索引/导数系数溢出及频谱规模边界。
- 安装后的独立 C++ consumer 直接使用新增公开头文件、频点推导和导数接口。

当前只计算收敛工作点的一阶噪声传播，不包括噪声乘噪声引起的均值偏移或高阶随机混频。模型没有饱和保护，不能把三次压缩多项式外推为物理 PA 的完整大信号响应。高阶 RFAMP/RFAMP_HO 标定、AM-PM、来源语义、网络频率自适应和厂商反馈基准仍需继续实现/验证。

## 工程验证记录

2026-10-10：MSVC Debug/Release clean-first 构建成功，CTest 各 114/114；两种配置安装后的独立 C/C++ consumer 各 2/2。144 个 C/C++ 文件格式检查及 git diff --check 通过。

本阶段未修改 Python/C ABI。独立 Python 3.12 使用上一阶段 wheel 连接本阶段安装的 Release DLL，14 组共 278 项旧接口回归通过：放大器工作点 14、既有工作点 13、仿射 11、混频线性化 13、共享相噪 13、相噪 13、通道测量 11、加载噪声 9、变频 NF 12、转换网络 13、单器件变频 11、API 85、相干系统 51、线性噪声 9。该证据用于旧接口兼容；新的高阶导数由原生专项和安装 C++ consumer 验证。

SystemVue 仍停留在既有 Error Running Script 提示，本阶段没有重发采集调用或新增厂商实测。
