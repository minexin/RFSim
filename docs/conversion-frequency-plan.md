# 转换网络的结构频率规划

`plan_conversion_frequencies` 为 C++17 转换网络补齐求解前的通道集合。它从源、噪声、观测所需的种子频点出发，反复应用器件频率规则和物理接线，直到所有集合闭合，再返回可供既有器件线性化和工作点求解使用的 `ConversionChannel` 数组。

本接口计算声明规则的结构频谱，不读取当前波幅，也不根据当前相消、弱信号或零初值删除频点。它尚未接入 C ABI、Python、JSON/CLI，未实现 SystemVue 自动选频、频谱削减或完整器件库行为。

## 规则和接线

头文件为 `rfmodel/conversion_frequency_plan.hpp`。每个 `ConversionFrequencyDevice` 声明物理 `ports`、本地 `seeds` 和若干类型明确的规则：

| 规则 | 输出频率集合 |
|---|---|
| `FrequencyCopyRule{input_port, output_port}` | 输入端所有频点传到输出端；适合已知单向同频传输 |
| `FrequencyPolynomialRule{input_port, output_port, polynomial}` | 对每个非零系数阶次生成实信号正负频率的所有和，再保留非负频率；包含DC与2..11阶谐波/互调 |
| `FrequencyMixerRule{rf_port, lo_port, if_port}` | 所有RF/LO通道对的和频与绝对差频；支持DC、多个LO频点与相等频率产生DC |
| `linear_frequency_rules(ports)` | 所有端口的同频集合取并集，包含反向传播；可用于稠密线性N端口 |

多项式的常数项非零时生成DC；零系数阶次不产生输出，但较高非零阶次计算所需的中间频率仍保留。零多项式不会自动制造DC或其他输出。`linear_frequency_rules` 不检查S矩阵；已知结构性零耦合可用实际的有向Copy规则表达。瞬时数值恰好为零不等于结构性零。

`ConversionPortConnection` 与工作点求解器共用。物理接线使两端口的频点集合相等，每个端口最多连接一次，允许同一器件的两个不同端口相接。频点会沿器件和接线多次传播；既不要求前馈拓扑，也不只扫描一次。

## 用法

```cpp
#include <rfmodel/conversion_frequency_plan.hpp>
using namespace rfmodel;
std::vector<ConversionFrequencyDevice> description{
    {{0, 1}, {{0, 3}, {0, 2}}, linear_frequency_rules({0, 1})},
    {{0, 1, 2}, {{1, 1}}, {FrequencyMixerRule{0, 1, 2}}},
    {{0, 1}, {}, {FrequencyPolynomialRule{0, 1,
        MemorylessPolynomial({.2, 1., .1})}}}
};
std::vector<ConversionPortConnection> wires{{0, 1, 1, 0}, {1, 2, 2, 0}};
auto plan = plan_conversion_frequencies(1e6, description, wires);
// 线性器件各端口：2,3；混频RF：2,3；LO：1；IF：1,2,3,4。
// 多项式输入：1,2,3,4；输出：0..8。合计24个通道。
// 按 plan.channels[d] 构造每个器件的模型和源/反射/噪声数组，
// 再用相同 wires 调用 solve_conversion_operating_point。
```

返回的器件顺序与输入一致；每个器件的通道按物理端口编号、频点整数升序排列。`seed_channel_indices[d][i]` 将第i个原始种子映射到本地通道索引，便于散布源波、初值和种子噪声相关矩阵。种子只是频率要求，连接端也可有种子；它不自动成为外部信号源。

规划器不会自动重排已有矩阵或制造源/噪声值。调用者应在完成规划后按返回的通道顺序构造器件；`lift_linear_conversion` 等其他构造器可能采用不同通道顺序，必须按 `(port,bin)` 对齐。若噪声或测量需要名义载波之外的边带，应先显式加入种子，不能只从非零信号线推断完整噪声频带。

返回还包括 `total_channels`、`generated_channels`、`iterations` 和 `transitions`。iterations包含最后一次无新增通道的闭合检查；transitions统计种子/复制/接线/输出插入尝试和多项式中间和，不是执行时间或完整CPU操作计数。声明顺序可能改变迭代次数和工作量，未触及限制时最终集合不变。

## 范围、失败与反馈

频率为非负整数bin乘公共有限正数 `spacing_hz`，DC为0。物理端口号0..1023，端口不得重复；种子不得重复、为负或引用未声明端口。每个声明端口最终必须有通道，否则报告未播种端口，避免返回无法构造现有器件的计划。

`ConversionFrequencyOptions` 默认为最多512个全局通道、`INT_MAX`最大bin和10000000次transition；通道与transition上限只能在该范围内收紧，max_bin可为0。全局规则最多4096条，多项式中间有符号集合最多4096项，与既有多项式结构规划的资源边界一致。超出频率整数或Hz数值范围抛出overflow_error；超过配置范围或资源限制抛出length_error；非法声明抛出invalid_argument。函数失败不返回部分计划，输入为只读。

**限制是失败边界，不是带宽截断。** 例如平方器输出反馈到RF输入时，1→2→4→8等频点继续扩展，不能把达到max_bin后的有限集合称为闭合。相反，纯DC平方反馈或LO为DC的混频反馈可以在有限集合上闭合。结构频率闭合不等于非线性工作点一定收敛，也不证明动态稳定。

本阶段覆盖明确声明的同频规则、固定系数双线性混频和实电压多项式。任意固定泵矩阵的支持推导、自动器件识别、高阶放大器的特殊频点选择、截断误差控制、非公度频率、连续噪声频带与厂商频谱削减仍待完成。

## 验证

`conversion_frequency_plan_test` 检查多级闭合、规则/端口/接线声明顺序不变性、幂等性、种子索引映射、非连续物理端口、双向线性和结构性单向传输、DC、多LO、零多项式、十一阶奇偶性、有限/扩张反馈及范围和资源失败。额外零波边带种子必须产生结构产物。

完整24通道计划直接构造线性—混频—多项式网络并求解工作点；每个最终输出频点由独立时域乘法、多项式求值与傅里叶投影核对，未以规划器自己的集合计算作为数值参考。安装C++消费者包含新头文件，用自动生成的差频/和频通道调用既有混频线性化接口。

本阶段实际构建、测试及文件保护记录见 [验证JSON](../validation/conversion-frequency-plan-validation-20261012.json)。没有新增SystemVue调用或推送；厂商兼容状态不因数学和工程回归升级。

2026-10-12 验证：MSVC Debug/Release 全量构建和CTest各124/124通过，两个独立安装目录的C/C++消费者各2/2通过；152个C/C++文件格式检查通过。新增头文件未改变已有C ABI，本轮采用增量全目标构建，没有clean-first、其他操作系统或新远程CI证据。全量测试基于保留六份既有参考脚本草稿的工作区，不能等同于纯净提交检出测试。
