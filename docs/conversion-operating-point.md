# 自动非线性频率转换工作点：C++ 核心

C++17 接口 solve_conversion_operating_point 根据固定器件方程、物理接线、外部源和反射自动求解入射/出射复功率波，再在收敛工作点计算一阶 C/P 噪声。它扩展了此前只能检查已给定工作点的仿射网络。

本阶段交付通用 C++ 求解器、固定系数双线性混频器、回归和安装 SDK。新增求解器的 C ABI、Python、JSON/CLI 尚未接入；原有 C/Python/JSON 仿射接口保持原语义，不会隐式运行迭代。完整 SystemVue RF Design 库兼容仍未完成。

## 固定器件方程

ConversionNonlinearDevice 以器件索引和 evaluate 回调描述非线性。回调接受当前本地入射向量 a，返回 ConversionLinearization，其中 outgoing 为真实 F(a)，jacobian 为直接矩阵 A 及共轭矩阵 B，满足 delta_b=A*delta_a+B*conj(delta_a)。器件通道顺序、物理端口、频点、公共参考阻抗和频率步长必须保持不变。求解器验证这些约束及有限数值、实 DC。

ConversionDevice 继续保存源、反射及独立源/内部噪声 C/P。回调替换该器件的确定性模型；普通器件保持原 A/B。全局 fixed_offset 可作为额外确定性内部出射项，适用于已接线通道。噪声数组在本次求解中固定，回调不隐式生成工作点相关器件噪声。

linearize_bilinear_mixer 使用固定系数 k=10^(gain_db/20)/lo_reference_amplitude，时域 RMS 波定义为 y=sqrt(2)*k*x_RF*x_LO。lo_reference_amplitude 的单位为 sqrt(W)，必须为有限正数。实际 LO 可为零、多谱线或 DC；它改变输出而不会重设 k。所有 RF/LO 通道对的和频与绝对差频 IF 通道必须显式提供，即使初始波为零也会检查。接口不自动扩频。

原 linearize_real_mixer 保持给定单泵工作点归一化与名义/一阶频率闭合语义。不能在每次迭代中按当前 LO 幅度重新归一化，然后将其当作固定的非线性器件。

## 求解与报告

未知量是全局入射向量。器件方程 b=F(a)+d_fixed、外部边界 a=source+Gamma*b、内部接线 a_i=b_j 共同定义残差。

每步从真实响应和导数建立 d=F(a)-A*a-B*conj(a)+d_fixed，通过仿射转换网络得到 Newton 候选点。真实非线性残差未充分下降时，以 1/2 逐次缩小步长。不能只看线性系统残差判断收敛。

每通道误差尺度为 absolute_tolerance+relative_tolerance*max(abs(actual_a),abs(expected_a))。最大尺度化残差 <=1 才接受工作点；默认相对容差 1e-9、绝对容差 1e-12 sqrt(W)。返回迭代次数、总回溯次数、最终 scaled_residual，以及该点的器件 Jacobian、仿射偏置和完整噪声分析结果。

返回波为最后接受的入射点及真实 F(a)+d_fixed；噪声由同一点的 Jacobian 计算。waves.relative_residual 为真实边界方程的最大相对误差，而非最后一次线性求解的残差。接近零的通道可能依靠绝对容差收敛，此时相对误差较大，收敛判断应使用 scaled_residual。

初值默认为外部 source，内部通道为零；也可提供完整全局向量。默认最多 50 次迭代、每步最多 24 次回溯，允许范围分别为 1..200 与 0..40；相对容差必须为有限正数且 <=0.01，绝对容差必须为有限正数。无收敛、步长搜索失败、奇异 Jacobian、非法模型或数值溢出均抛出异常，不返回半成品。多解可能依赖初值；不判定动态稳定性，也不保证任意网络都收敛。

## C++ 示例

```cpp
#include <rfmodel/conversion_operating_point.hpp>
#include <rfmodel/mixer_linearization.hpp>
using namespace rfmodel;
const std::vector<ConversionChannel> channels{{0, 0}, {1, 0}, {2, 0}};
const auto zero = conversion_detail::zero(3);
FrequencyConversionModel placeholder(1., channels, zero, zero);
ConversionDevice device{placeholder, {1., 2., 0.}, {0., 0., 0.},
                        placeholder.zero_noise(), placeholder.zero_noise()};
ConversionNonlinearDevice mixer{
    0, [channels](const std::vector<Complex>& incident) {
        auto point = linearize_bilinear_mixer(
            1., channels, incident, -10. * std::log10(2.), 1.);
        return ConversionLinearization{point.incremental_model, point.operating_outgoing};
    }
};
auto result = solve_conversion_operating_point(
    {device}, {}, {mixer}, {0., 0., 0.});
// DC 乘法器 IF 出射波为 2 sqrt(W)。
```

ConversionPortConnection 依次保存 first_device、first_port、second_device、second_port，接线规则与 FrequencyConversionNetwork 一致。additional_source_noise 支持不同器件外部通道的相关性；loaded_noise 返回入射、交叉和净噪声流。

## 独立验证

- 固定系数混频器：正负频率傅里叶卷积独立计算输出，覆盖 RF/LO DC、多 LO、折叠、复相位；逐列实/虚中心差分核对 A/B。LO 翻倍则输出翻倍；零波不能掩盖缺失频点。
- 非线性负反馈：F(a)=-abs(a)^2*a、source=1、Gamma=1，从 0.01 起始触发实际回溯；实根满足 a+a^3=1。解析径向/切向传递核对输出 C/P、入射 C 和额外噪声叠加。
- 双线性物理闭环：输出 b 同时反馈到 RF/LO，满足 b=(1+0.1*b)*(2+0.2*b)。从零求解低幅根 (30-sqrt(500))/2，核对真实器件输出与接线残差。
- 迭代上限、无回溯失败、奇异反馈、已收敛初值零迭代、重复回调、通道契约错误、非法噪声、内部边界源、复 DC 拒绝。

## 后续工作

接入 C ABI/Python/JSON 的显式求解选项和诊断；适配现有压缩/高阶放大器；支持工作点相关噪声、频率扩展及更高效的大网络求解；恢复 SystemVue 实测并逐项校准厂商语义。这些均未由本阶段数学回归证明完成。

## 工程验证记录

2026-10-09：MSVC Debug/Release clean-first 构建成功，CTest 各 110/110；两种配置安装后的独立 C/C++ consumer 各 2/2。140 个 C/C++ 文件格式检查及 git diff --check 通过。安装消费者直接包含新头文件并调用自动工作点求解。

Python 源码本阶段未变更。独立 Python 3.12 使用上一阶段 wheel 加载本阶段安装的 Release DLL，12 组共 251 项既有接口回归通过：仿射 11、混频线性化 13、共享相噪 13、相噪 13、通道测量 11、加载噪声 9、变频 NF 12、转换网络 13、单器件变频 11、API 85、相干系统 51、线性噪声 9。该记录证明旧接口兼容，不代表新的自动求解器已有 Python 入口。

SystemVue 实测仍被现有 Error Running Script 弹窗阻塞。
