# 带复幅度的来源表达式

本阶段为一个物理相干分量保存多个来源乘积及其各自的复波贡献，并提供 C++17、
C ABI 和 Python 运算。它是混频折叠后继续做非线性展开的基础接口，现已接入
[混频与多项式的多来源系统图](mixed-polynomial-graphs.md)。

## 数值语义

每项由规范排序的 `(root_id, sign)` 因子列表与 `amplitude` 组成。
`amplitude` 是已经包含源波和路径增益的实际复波贡献，不能再次乘入源波。
表达式各项必须属于调用者确定的同一个物理相干分量；接口没有频率、带宽、
相干组等上下文，也不会替调用者判断不同分量能否相干相加。

- 加法合并相同因子列表，使用补偿求和；不同列表保持独立。
- 乘法按分配律展开 1..11 个带符号的一基父表达式索引，并乘指定复系数。
- 负索引同时共轭每项的复波及所有因子符号。
- 重复因子和正负因子均保留；不约消 `x*conj(x)`，不隐式截断来源阶数。
- 精确零幅度项保留来源身份；空表达式也可表示零。
- 全过程不除以父表达式的总波，因此父项相消不造成除零或丢失不同来源。

例如两个根的贡献为 +1 和 -1，总波为零。平方仍保存
`root7²: +1`、`root7*root9: -2`、`root9²: +1` 三项，总波仍为零。
这与一个来源不明的零波不同，可继续共轭、展开或按明确规则筛选阶数。

## 接口

C++ 头文件 `rfmodel/origin_expression.hpp` 提供 `OriginContribution`、
`OriginExpression`、`sum_origin_expressions`、`product_origin_expressions`、
`origin_expression_amplitude`。求和与乘法返回各项；最后一个函数计算总波。
各项及总波求值均要求复数分量和模平方有限，溢出整体报错。

C ABI 的 `rfmodel_sum_origin_expressions` 和
`rfmodel_product_origin_expressions` 同时返回总波、项描述数组及扁平因子数组。
项描述的 `factor_offset` / `factor_count` 定位因子。调用者提供不重叠的输出
缓冲区和计数器；容量不足、非法输入或非有限结果均不修改任何输出。
空结果允许空输出缓冲区，但仍必须提供计数器和总波指针。

Python `Library.sum_origin_expressions(parents)` 和
`Library.product_origin_expressions(parents, indices, coefficient=1.)`
返回 `OriginExpressionResult(terms, amplitude)`；每项是
`OriginContribution(factors, amplitude)`，可以直接作为后续运算的输入。

```powershell
$env:PYTHONPATH = "$PWD/python"
python examples/origin-expressions.py build-msvc/Release/rfmodel_c.dll
```

## 资源边界

输入最多 4096 个父表达式，每个最多 4096 项，合计最多 65536 项和
1048576 个因子。每项要求 1..256 个因子，根 ID 非零且符号为 ±1。
输出（包括乘法中间结果）最多 4096 项、65536 个因子；乘法最多执行
1000000 次项对组合。选中的父表达式先合并重复项，以避免重复编码放大计算量。
超限整体拒绝，不静默裁剪；因此中间展开超限也会拒绝，即使最终可能抵消。

## 验证与剩余工作

原生回归用独立复数幂校验 1..11 阶二项展开，覆盖交叉项重数、递归乘积、
复系数、全因子共轭、补偿相加、相消来源保留、空表达式和输入/输出资源边界。
C 与 Python 验证接口行为，安装后的独立 C/C++ 消费者验证公开头文件和符号。

后续图层已按流保存实际贡献，并分开管理物理相干类别与原始来源历史。
混合图显式采用 RF/LO 因子共同计数政策；共同压缩已由
[公共工作点](common-amplifier-response.md)接入。SystemVue 阶数语义仍未验收，
详见[混合图边界](mixed-polynomial-graphs.md)。

本阶段不新增 SystemVue 原始采集，也不声称高阶 RFAMP 系数、全局阶数、
子谱合并或混频递归兼容性已验收。

## 本阶段工程结果

- MSVC Debug / Release 完整 CTest 各 75/75。
- 两种配置安装后的独立 C/C++ 消费者各 2/2。
- 独立 Python 3.12 wheel：68 项 API、27 项系统图测试通过。
- 相消来源平方示例运行成功，C/C++ 格式检查 108 个文件通过。

Release 验证曾发现内部 C++ 容器辅助函数误处于 C 链接区域，触发 MSVC
C4190 警告及 C 消费者崩溃。辅助函数移回 C++ 链接区域后重新验证通过；
公开导出函数仍使用 C ABI。
