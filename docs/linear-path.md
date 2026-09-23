# 多级线性链路功率预算

`analyze_linear_path`（`include/rfmodel/linear_path.hpp`）在一个频率点连接有序二端口列表，求解完整双向网络，再从各端口入射/出射波计算功率。有限 S12、级间失配以及源/负载反射会同时影响所有级；不采用单向增益连乘近似。

## 输入与结果

每个 `LinearPathStage` 提供唯一非空名称、2×2 S 矩阵和共同正实数参考阻抗。源输入使用**可用功率 W**，必须为有限正值；源反射系数模小于 1，负载反射系数模不大于 1。求解器的激励设为 `sqrt(Pavailable)*sqrt(1-abs(Gamma_source)^2)`，与其边界式 `a=excitation+Gamma*b` 一致。超过 512 级、参考阻抗不一致、非法矩阵或奇异网络会报错。

`LinearPathResult` 返回完整 `NetworkWaves`、源可用功率、首级净接受功率、负载净吸收功率、首级净接受功率与源可用功率之比，以及链路换能增益。全局端口索引 `2*i` 为第 i 级输入，`2*i+1` 为输出。

每级结果提供输入/输出端口的 `PortPower`（入射、出射及净吸收 W），以及：

- `operating_gain`：该级净输出 / 该级净输入。
- `cumulative_operating_gain`：该级净输出 / 整条路径首级净输入。

输出端口的 absorbed_w 为器件吸收功率，所以向后级交付功率应取其负值。保留真实功率流方向；分母非正或交付功率为负时，增益返回空 optional，而不输出伪造的零值或负功率 dB。负载交付功率为零且分母为正时，增益为零。`input_power_fraction` 也是有符号数，主动输入端可能向源回送功率。

```cpp
std::vector<rfmodel::LinearPathStage> stages = {
    {"amplifier", {2, {0.0, 0.0, 10.0, 0.0}}},
    {"attenuator", {2, {0.0, 0.5, 0.5, 0.0}}}
};
const auto budget = rfmodel::analyze_linear_path(stages, 0.001);
// 1 mW available input -> 100 mW after amplifier -> 25 mW at matched load.
```

## 验证和当前范围

`linear_path_test` 检查匹配链路解析预算、复失配双向链路与提取的整链 S 矩阵一致性、级间功率连续性、级增益与累计增益关系、首级源失配、功率缩放、完全反射负载和主动输入回送功率。2026-09-24 MSVC Debug/Release 全套各 34/34 通过，62 个 C++ 文件通过格式检查。

这是单频线性二端口链路接口；调用者可按频率构建各级矩阵并逐点调用。尚未提供任意多端口拓扑中的路径选择、路径噪声预算、频率转换/非线性联立或 SystemVue 路径结果数值对照。它与 `spectrum_analysis.hpp` 的前向多频谱链路目前是两类独立分析，不能据此声称已完成 RF System Analysis。

前一提交 `30ecb5d15e2e4eba59f81a3ef8e8b12ea9cac5bb` 的 Windows、Ubuntu、macOS × Debug/Release 六项 CI 均成功（包括目录提取工具、核心测试和安装包消费者步骤），证据保存在 `validation/cross-platform-30ecb5d.json`，运行编号 35935485130。这份跨平台证据不覆盖本次新增路径接口；其远程验证由本次提交后的 CI 执行。
