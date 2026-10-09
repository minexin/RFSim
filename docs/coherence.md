# 显式相干分组与离散功率合并

相干合并接口已连接 C++、C、Python 和 JSON/CLI。输入为同一观察点、同一功率波
参考约定下的确定性 RF 分量，幅度单位 sqrt(W)，不是噪声功率谱密度。

## 契约

每个 CoherentComponent 包含正整数 bin、SpectrumKind、正带宽 bandwidth_hz、
正 uint64 coherence_group 和复幅度 amplitude。频率等于 bin×spacing_hz。
kind 支持 source、harmonic、intermod；不把随机噪声伪装成确定性电压。

只有 (bin,kind,bandwidth_hz,coherence_group) 全部相同才按复幅度相加。
不同键按各组合并后的幅度平方相加，即使它们频率相同。相干组完全抵消时仍保留
零幅度组和零功率 bin。频率、带宽及类别精确匹配，无隐含容差聚类。

相干 ID 由调用者显式赋予，只在当前观察点/分析上下文中有效。同一组代表调用者
已确认相关的源/参考时钟关系；不相关源必须使用不同 ID。重复键是有意合并的
路径贡献，不作为重复输入拒绝。此接口不推导时钟，不按频率猜测相干性，
不自动给多级非线性产物分配身份，也不取 sqrt(总功率) 伪造一个总复电压。

带宽是相干匹配的元数据。接口合并的是离散分量功率，不进行频谱形状积分、
邻道带宽测量或宽带重叠积分。三种类别下的不同组均假定互不相干；一般部分相关
情形需要另行提供相关性模型，不能随意分配同组/异组近似。

最多 4096 个输入分量；spacing、频率、带宽与幅度功率必须有限。带宽两端不能
越过负频率或溢出；DC、零组 ID、未知类别及非有限量均拒绝。使用补偿求和减少
大分量抵消时丢失弱分量，组功率或总功率溢出整体拒绝。输出组按上述键排序，
每 bin 的功率按频率排序，不修改输入。

## 公共接口

C++ 头文件 rfmodel/coherence.hpp：

    auto result = rfmodel::reduce_coherent_components(spacing_hz, components);

结果 CoherentReduction 包含 components、power_by_bin_w、total_power_w。
原多项式分阶接口不改变；调用者必须保留来源并给出有效相干关系后才使用此合并层。

C 函数 rfmodel_reduce_coherent_components 接收 rfmodel_coherent_component 数组，
输出合并组数组、rfmodel_bin_power 数组、两个 count 和 total_power_w。
容量以各自数组元素计。所有输入/输出缓冲区与标量须互不重叠，任何失败均不写
任何输出。空输入允许 NULL 数组，但仍要求两个 count 和 total 指针。
新增接口保持 ABI 版本 1，已有符号和结构未修改。

Python 提供 SpectrumKind、CoherentComponent 和
Library.reduce_coherent_components(spacing_hz, components)，返回 CoherentReduction。
kind、bin 和组 ID 拒绝布尔值和错误整数范围，uint64 最大值可无损往返。

## 文件与示例

rfmodel.coherence version=1 包含 spacing_hz 和 components，每项要求 bin、
kind（source/harmonic/intermod 字符串）、bandwidth_hz、coherence_group、amplitude。
amplitude 为实数或 [real,imag]。严格拒绝缺项、未知字段及非有限 JSON 数值。
输出格式 rfmodel.coherence-result 含合并组、power_by_bin 和 total_power_w。

    python -m rfmodel examples/coherence.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/coherence-result.json

示例同组 1 与 −1 完全抵消，另一 source 组保留 1 W，intermod 类别保留
0.0625 W，合计 1.0625 W。虽然均位于 1 GHz，也不会全部相干相加。

## 验证与 SystemVue 边界

解析回归包括同相功率增至四倍、反相抵消、不同组/类别/带宽/频率隔离、抵消后的
弱分量、零项保留、非法元数据、资源限制和溢出。C 检查输出原子性，Python
覆盖 uint64、文件与 CLI，独立安装消费者验证 C++ 头文件和 C 导出符号。

compare-coherent-carriers.py 复用四组已验收的复幅度采集，先逐项通过原生模型
比较，再把两个载频的六项传给原生合并接口。各载频包含一个 source 与两个
独立三阶 intermod，使用 SystemVue 实测相干编号及边界宽度。
输出与 Node Total from 'RFAmp' 的精确载频采样比较，共 8 点。
这里的分组是从参考软件导入，尚未验证 RFModel 自行生成相干编号。

节点总谱包含噪声；本接口不预测噪声。比较器要求导出的节点噪声最大功率相对
待比较总功率不超过 1e-8，再使用原 1e-7 误差阈值，噪声占比过大直接拒绝。
没有从节点总功率反推预测值，也没有通过改变噪声来消除差异。
归档报告 validation/systemvue-2023-coherent-carriers.json 保留参考和 DLL 散列。
回归还检查总谱改动、错误合并相干编号、噪声占比过大不能变成通过。

本阶段仍缺跨级相干 ID 推导、分支/再合路真实参考、部分相关信号、噪声/变频联合
传播和第二个非线性级。该基础接口不代表完整 RF System Analysis 已兼容。

2026-10-09 验证记录：MSVC Debug/Release 清理重建后全套 CTest 各 60/60，
独立安装 C/C++ 消费者两种配置各 2/2，格式检查覆盖 92 个 C/C++ 文件。
载频参考比较器 4 项回归通过；8 个实测节点功率点最大相对差异
2.28578e-8，小于原 1e-7 阈值。Release 报告保留精简采集和 DLL SHA256。
跨平台结果以本阶段提交的六项 CI 作业为准。

多端口传播扩展见 [coherent-network.md](coherent-network.md)：显式输入组已可进入完整线性网络，分支/再合路的 SystemVue 实测验收仍待完成。
