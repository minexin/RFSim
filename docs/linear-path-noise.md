# 线性链路噪声接口

`rfmodel/linear_path_noise.hpp` 提供单频点 `analyze_linear_path_noise`，输入按连接顺序
排列的 LinearPathStage 和对应 NoiseCorrelation。各级为双端口，共用同一正实参考阻抗；
器件之间噪声独立，但每个器件的两个噪声波允许相关。完整网络求解保留双向传输和反射。

输出包含等效 S、等效内部噪声协方差、换能增益，以及交付给负载的源噪声、器件附加噪声、
总噪声密度（W/Hz）。噪声因子使用独立的参考温度，默认 290 K；改变实际源温度不会
改变这一标准化噪声因子。无信号传输或完全反射负载时噪声因子为空，而非伪造有限值。

```cpp
#include <rfmodel/linear_path_noise.hpp>

const rfmodel::SMatrix pad{2, {0., 0.5, 0.5, 0.}};
const auto covariance = rfmodel::passive_thermal_noise(pad, 290.);
const auto result = rfmodel::analyze_linear_path_noise(
    {{"pad1", pad}, {"pad2", pad}}, {covariance, covariance});
// Matched thermal equilibrium: total_output_w_per_hz = k * 290.
// Two 6.0206 dB pads: noise_factor = 16.
```

源的可用噪声为 kT，负载无噪声；器件温度已经包含在调用者提供的协方差中。
负载噪声、跨器件相关噪声、每级功率谱/贡献分解以及变频噪声尚未由此接口支持。
当前分解区分整个链路的器件附加噪声与源噪声，不提供逐器件归因。
最多 512 级，底层仍是稠密网络算法，该上限不是性能承诺。

输出噪声通过等效网络 `b = S Γ b + c` 求解，将出射噪声乘负载吸收比例；
源噪声则由可用 kT 乘换能增益。这样相关项和两端反射都保留在计算中。
输入校验拒绝无效温度、非被动端接、重复器件名、维数错误和非半正定协方差。

解析测试覆盖 Friis 两级放大器/衰减器、两级被动热平衡、冷源、复数相关噪声的独立
标量解、失配端接、完全反射负载和零传输。所有噪声断言按 kT 归一化后比较，
避免绝对容差大于热噪声本身。参考衰减器程序也使用此接口；真实多器件 SystemVue
对照仍待补充，不能用解析测试代替。

## 本轮验证（2026-09-28）

MSVC Debug/Release 完整 CTest 均为 35/35；独立安装消费者均为 1/1。
消费者通过安装包的 RFModel::rfmodel 使用新接口，未使用源码头文件路径。
已有消费者构建目录曾缓存旧 RFModel_DIR；显式改为本轮安装包目录后通过。
65 个 C++ 文件格式检查通过。

参考探针切换到新接口后，损耗、零点邻域、温度、源功率四组共 72 项历史比较
保持原判定（49 项通过，23 项失败），最大相对数值变化为 7.11e-16。
重放摘要见 `validation/linear-path-noise-reference-replay.json`。
本轮没有重新运行 SystemVue，也未消除此前量化的噪声与零点差异。
