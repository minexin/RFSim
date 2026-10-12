# 转换网络的结构频率规划

`plan_conversion_frequencies` 为 C++17 转换网络补齐求解前的通道集合。它从源、噪声、观测所需的种子频点出发，反复应用器件频率规则和物理接线，直到所有集合闭合，再返回可供既有器件线性化和工作点求解使用的 `ConversionChannel` 数组。

本接口计算声明规则的结构频谱，不读取当前波幅，也不根据当前相消、弱信号或零初值删除频点。C ABI、Python 和显式启用的 JSON/CLI 现已接入同一原生规划器；SystemVue 自动选频、频谱削减或完整器件库行为仍未实现。

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

## C ABI 与 Python 接入

`rfmodel_plan_conversion_frequencies(request, output, diagnostics)` 是 ABI 1 的新增符号，旧结构与函数布局保持不变。`request` 指向器件数组、物理接线、公共频率步长及可选options；NULL options使用C++默认限制。每项器件包含端口数组、种子通道数组和规则数组。

规则kind分别为COPY、POLYNOMIAL、MIXER。input_port/second_input_port/output_port在混频规则中分别为RF/LO/IF；其他两类second_input_port必须为0。多项式需1..12个有限实系数；其他两类必须使用NULL系数指针和零计数，防止未使用字段掩盖错误输入。

output为NULL时仅查询diagnostics中的channel_count、seed_count等尺寸。实际输出包括拼接后的channels、长度为device_count+1的device_offsets，以及按原器件/种子顺序拼接的seed_channel_indices；**seed索引指向所在器件的本地通道**。所有缓冲区容量按元素计数，大小足够即可，尾部未使用元素不写。无种子且由常数项与接线闭合时seed输出可为NULL/零容量。

所有输出，包括diagnostics，必须与全部输入及嵌套数组、请求/输出描述符和其他输出分离。检查整个声明容量，而不只检查实际写入部分。尺寸、非法参数、溢出、闭合失败或别名错误均不修改任何输出；调用者仍负责指针地址和生命周期。

```python
from rfmodel import Library
library = Library("build-msvc/Release/rfmodel_c.dll")
plan = library.plan_conversion_frequencies(1e6, [
    {"ports": [0, 1, 2], "seeds": [(1, 3), (0, 5)],
     "rules": [{"type": "mixer", "rf_port": 0, "lo_port": 1, "if_port": 2}]}
])
# plan.channels == (((0, 5), (1, 3), (2, 2), (2, 8)),)
# plan.seed_channel_indices == ((1, 0),)
```

Python接受type为copy/polynomial/mixer的严格规则对象，返回ConversionFrequencyPlan，所有通道与映射为tuple。Copy与Polynomial用input_port/output_port，后者还需voltage_coefficients。可传max_total_channels、max_bin、max_transitions；布尔索引、未知字段、重复或越界通道均拒绝。纯Python wheel不包含DLL，必须连接含新增符号的原生库。

## JSON 显式自动规划

在 `rfmodel.conversion-network` 版本1顶层增加frequency_plan对象后启用。缺省不启用，旧通道顺序和求解流程保持原有契约。当前可自动推导规则的model.type为linear、bilinear_real_mixer、polynomial_amplifier和saturating_amplifier；其他类型明确拒绝，不猜测矩阵或高阶放大器的特殊选频规则。

```json
"frequency_plan": {
  "max_bin": 16,
  "new_channel_boundary": {"reflection": 0, "noise_temperature_k": 290}
}
```

这些限制仍是失败界限，不是频谱过滤器。启用后，非线性器件的channels和线性器件的bins表示**初始种子**，可以为空；物理接线和规则必须最终为每个声明端口产生通道。静态线性S矩阵给出端口数；参数化线性model.device必须另填器件级frequency_ports正整数，并在实际构造模型后核对。线性端口按0..N−1声明，采用同频全端口并集，不依据某个S值恰好为零剪枝。

新通道仍需物理边界。显式boundaries可引用生成后的通道，并优先使用其指定条件。new_channel_boundary仅给**新生成、未接线且未显式指定**的通道补边界，不会替漏写的原始种子补终端。该对象可含reflection和noise_w_per_hz或noise_temperature_k之一，不允许source；默认源波为零。没有此对象时，原有“每个外部通道必须显式给出边界”的检查仍有效。

新增终端可明确选择匹配热终端、复反射或无噪声边界。若已使用器件source_noise矩阵，就不能再混用边界噪声字段；此时new_channel_boundary可只给reflection，噪声按下述矩阵契约处理。

### 种子顺序与数据映射

- 非线性器件：原channels数组顺序就是种子索引顺序。
- 线性器件：原bins顺序下，每个bin依次排列0..N−1端口，与旧linear_device接口一致。
- 器件source_noise、非线性器件noise的C/P矩阵、output_offset，以及全局additional_source_noise和operating_point.initial_incident均按**原种子顺序**提供；扩展时保留所有原值和相关项，新位置填零。
- 线性器件noise仍是原有物理N端口噪声模型，会在规划后的每个频率重新计算；它不是种子通道矩阵。
- am_pm.output_channels仍选择原种子的本地索引，经映射后作用于相同物理通道；不会自动对所有新生成通道开启AM-PM。
- boundaries、相噪源/共享参考组、测量和噪声分析继续使用device/port/bin标识，无需靠位置重排。

矩阵补零表示用户声明的有限频带噪声支持，不能据此声称未知频带的物理噪声为零。需要额外噪声频带时，应将对应通道加入种子并提供统计量。扩频也不会凭空生成相噪偏移采样或连续PSD数据。

结果新增frequency_plan报告，列出通道总数、新增数、迭代/工作量、限制、各器件种子映射和自动添加的边界通道。新模式统一输出每个器件按port/bin排序的通道；线性模型的S及内生C/P也按该顺序重排。输入document不被修改。

[完整示例](../examples/auto-frequency-mixer-polynomial.json)从5个JSON种子通道得到24个闭合通道：线性输入两个频点、一个LO及下游多项式，输出包含DC至8 MHz。19个通道自动补齐，9个输出通道显式采用匹配290 K热终端。独立时域投影核验最终信号，手工展开的完整模型核验全部输出C/P。

```powershell
python -m rfmodel examples/auto-frequency-mixer-polynomial.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/auto-frequency-result.json
```

跨语言阶段的实际构建、测试与wheel记录见 [接口验证JSON](../validation/conversion-frequency-plan-api-validation-20261012.json)。厂商自动选频、频谱削减和非公度频率仍未验收；新增接口没有改变AM/PM实测差异结论。
跨语言验证（2026-10-12）：MSVC Debug/Release clean-first与全量CTest各126/126，安装后的C/C++消费者各2/2；153个C/C++文件格式检查通过。Python 3.12.14从新wheel导入并加载安装后的Release DLL，自动规划10项、既有转换网络13项、AM-PM14项、通用API85项，共122项通过。全量CTest使用Python 3.10及保留既有草稿的工作区，不是纯净提交检出或新的跨平台CI证据。本轮未推送、未新增SystemVue分析。
