# 源与参考时钟的相干关系

source_coherence.hpp 提供源级关系解析，衔接显式相干分量与实际信号源配置。
规则来自本机 SystemVue 2023 帮助 sim/Coherency.html：同一源的贡献可以相干；
不同源只有在声明相同参考时钟时才具备相干关系，之后仍须匹配类别、中心频率和
带宽。本功能不推导物理时钟锁定状态，参考时钟名称表示调用者的关系声明。

## 原生接口与身份范围

SourceCoherence 包含 source_id 和 reference_clock。source_id 必须非空且唯一，
reference_clock 可为空。assign_source_coherence 接收完整源定义列表，
返回与输入顺序一一对应的正 uint64 分组编号。

- 无时钟：每个不同 source_id 分到独立组；同一源在多个端口/路径使用其同一编号。
- 相同非空时钟：不同源共享一组。
- 不同非空时钟：保持不同组。
- 源 ID 和时钟标签使用独立命名空间，例如独立源 clock 不会意外与时钟 clock 合并。
- 标签精确且区分大小写；不做空格裁剪、Unicode 归一化或语言区域折叠。
- 同一完整定义集合仅改变输入顺序时，各源的分组编号保持一致。
- 增删定义可能重新编号；编号只在此分析源集合内有效，不能跨分析混用数字身份。

每次最多 4096 个定义，标签最长 1024 字节，不接受字符串内的 NUL。
C++/C 将标签作为字节序列比较；Python 将字符串编码为 UTF-8 后执行相同限制。
不相关频率、不同 kind 或 bandwidth 即使获得相同源组 ID，也仍由原相干合并层
分别处理。此解析器不生成谐波/互调来源签名，也不建立 Mixer LO 的传播关系。

C 函数 rfmodel_assign_source_coherence 接收 rfmodel_source_coherence 数组，
输出 count 个 uint64_t。source_id 非 NULL，reference_clock 为 NULL 等价于空串。
所有缓冲区须互不重叠；非法标签、重复 ID 或容量不足时输出保持原值。
空输入可传 NULL。它是 ABI 版本 1 的追加符号，不改变已有结构布局。

Python 接口：

    from rfmodel import Library, SourceCoherence
    library = Library("build-msvc/Release/rfmodel_c.dll")
    groups = library.assign_source_coherence([
        SourceCoherence("first", "lab-clock"),
        SourceCoherence("second", "lab-clock"),
        SourceCoherence("independent"),
    ])
    assert groups[0] == groups[1]
    assert groups[0] != groups[2]

## JSON 源定义模式

rfmodel.coherent-network version=1 新增可选 sources 字段。
未提供 sources 时完全保留已有显式 coherence_group 模式。
提供 sources 后，每项定义必须有 id，可选 reference_clock，缺省为空：

    "sources": [
      {"id": "first", "reference_clock": "lab-clock"},
      {"id": "second", "reference_clock": "lab-clock"}
    ]

该模式下每个 inputs 项仍含 port 和 component，但 component 改为
source、bin、bandwidth_hz、amplitude；source 引用上述 id，kind 固定为 source。
禁止同时填写 kind/coherence_group，防止手动编号与生成编号发生无意碰撞。
混合谐波/互调分析应继续使用显式分组模式，直至完整产物身份传播层完成。

结果增加 sources 数组，保存 id、reference_clock 和 coherence_group 对照。
不用数字编号倒推源关系，调用者应保存源定义或结果映射。
允许定义未激励的源，便于在相同源集合下多次选择输入，维持编号一致性。

examples/clocked-coherent-combiner.json 含两路反相、共用时钟的等幅输入，
经过匹配合路网络后总输出为 0。把第二路 reference_clock 移除，
它成为独立源，输出总功率变为 0.5 W。这是解析验算，尚非反相实测验收。

    python -m rfmodel examples/clocked-coherent-combiner.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/clocked-coherent-result.json

## 验证与边界

原生测试检查共时钟、独立源、源名/时钟名碰撞、大小写、顺序稳定性、资源限制，
以及同组但不同频率/类别/带宽仍不合并。C 检查失败时输出原子性；
Python 检查 UTF-8 字节长度、NUL、重复定义，模型文件检查重复源、未知源、
错误类型与手工编号混入。独立安装消费者调用新增 C++ 头文件和 C 符号。

现有 SystemVue 双源同相采集由 compare-coherent-network.py 重新验证：
先核对源参数回读，再将源 ID/RefClk 送入原生解析器，预测传播与合路输入功率。
参考相干编号只用于检验“同组/异组关系”，不要求与 RFModel 的数值编号相等，
也不作为 RFModel 预测输入。回归将原生分组故意改成独立组，确认实测比较失败。

实测范围仍只有此前一组同相、共参考时钟的双源案例。独立时钟和 180 度反相
采集仍因 SystemVue 错误提示待恢复；不把本次解析实现等同于这些案例已验收。
下一层工作是非线性产物来源及 LO/参考时钟关系的逐级传播，并补齐实测相位扫描。

2026-10-09 验证：MSVC Debug/Release 清理重建后全套 CTest 各 64/64，
独立安装 C/C++ 消费者各 2/2。独立 Python 3.12 从新 wheel 导入后，Python API
59 项和文件接口 9 项测试通过；C/C++ 格式检查 96 个文件通过。
使用最终 Release DLL 更新实测报告，现有同相路径复幅度及合并输入功率仍通过，
并新增参考时钟关系等价性检查。跨平台状态以本次提交的六项 CI 为准。
