# 递归来源与多项式系统图

## 原始因子代数

`expand_mixing_origin` 把最多九个带符号的一基父来源下标展开为原始根因子。
每个因子为非零 uint64 root_id 与 sign=+1/−1。负父下标表示对整个父表达式
取共轭，因此翻转其中每一个因子的符号。结果按 root_id、sign 排序，保留重复
与正负因子，不约掉共轭对；`x*x*conj(x)` 的来源阶数仍是 3。

接口已贯通：

- C++：`MixingOrigin`、`OriginFactor`、`expand_mixing_origin(parents, indices)`。
- C：`rfmodel_expand_mixing_origin`，父来源为指针/长度数组。
- Python：`Library.expand_mixing_origin(parents, indices)`，返回 OriginFactor 元组。

每个父/输出最多 256 个因子，最多 4096 个父来源、65536 个总父因子。
非法根号、符号、下标或容量不足均拒绝；C 输出数组和 count 不得重叠，
失败时二者都保持不变。局部 API 不推断物理源、频率或相干时钟，由调用者
提供已经解析的根 ID。根 ID 与源相干组号是不同的命名空间。

## 系统图集成

`rfmodel.coherent-system` version=1 新增 `polynomial_amplifier` 阶段：

```json
{
  "id": "second",
  "type": "polynomial_amplifier",
  "input": "first",
  "output": "second",
  "voltage_coefficients": [0, 1, 0.5],
  "max_source_order": 4
}
```

电压系数沿用[九阶多项式](coherent-polynomial.md)的单位和 RF 投影契约。
`max_source_order` 必填，为 1..256 的整数，限制本阶段输出相对于原始 RF
根因子的总次数，而非仅限制本地多项式次数。限制同时作用于传播项和新生成项。

图级追踪从输入的相干组、频率 bin、带宽建立根标识。锁定到同一参考时钟
且频率/带宽相同的输入遵守已有源相干分组；线性网络、基波压缩和限幅
放大器传播均保留来源。多项式结果展开父表达式后，用相同的原始因子、
频率、类型和带宽识别同源项，再调用原生相干归并。

对于多项式新生成项，全部因子都为同一根的正号时归为 harmonic，其他为
intermod。一次传递保留原类型。故前级 H2 与本级直接生成的 H2 可以相干
合并；H2 与基波的和频可展开为同源 H3，而差频保留两个正因子和一个负因子，
仍为三阶 intermod，不能误标为一阶源。

同一根表达式的带宽用原始因子的宽度作补偿求和，避免不同递归括号顺序
产生舍入差别而遗漏同源归并。该宽度仍是谱支撑宽度记账，不是谱形积分。

## 可审计输出

含多项式阶段的结果增加：

- `origin_roots`：根 ID、源相干组、原始 bin、原始带宽和 RF 角色。
  通过 sources 表的 coherence_group 可回溯源声明/参考时钟。
- 每条流的 `origins`：对应分量的来源阶数与带符号根因子。
- 阶段 `origins`：本地 order/input_indices 与展开后的来源字段。
- `generated_term_count`、`discarded_term_count`、`discarded_by_source_order`：
  显式区分原生生成项、因全局阶数被舍弃的项及其阶数分布。

全局阶数过滤在本地多项式求值后、同源合并前执行。它不会放宽原生模型的
64 活跃输入、4096 输出项等计算上限；即使许多项最终会被过滤，原生计算
超限仍整体报错。来源表最多 65536 个根和 1048576 个已存储因子。
没有多项式阶段的旧系统图保留既有结果结构与相干行为。

## 混频组合的后续实现

[混频与多项式的多来源系统图](mixed-polynomial-graphs.md)现已接入线性网络、
理想混频和多项式联合传播。不同 RF 输入折叠到同一 IF 后，保留多个实际
复波贡献并继续相干相加；混合图使用独立的来源表达式结果结构。

本文上述单来源字段描述适用于没有混频器的既有多项式图。混合图明确将
RF 与 LO 因子共同计入 max_source_order，另报告 rf_order；这不是已经验证的
SystemVue 全局阶数政策。共同压缩、高阶 RFAMP 标定和厂商子谱合并仍待完成。

## 示例与数值验证

[两级示例](../examples/coherent-recursive-polynomial.json)使用两级
`v_out=v_in+0.5*v_in^2`，50 Ω、输入波 .01 sqrt(W)。第二级 H2 波为 .0005，
H3 为 .0000125，H4 为 .00000015625；H2 包含传播和新生成两个同源路径。
将第二级 max_source_order 改为 3 时，仅舍弃 H4；改为 2 时舍弃三个局部路径。

```powershell
$env:PYTHONPATH = "$PWD/python"
python -m rfmodel examples/coherent-recursive-polynomial.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/recursive-polynomial-result.json
```

测试覆盖共轭翻转、父项顺序变化、重复因子、边界拒绝、失败原子性、全局
截断计数、相位奇偶性、分数带宽一致性及 CLI 失败不覆盖既有结果。
独立双音稀疏卷积对照验证了完整两级复幅度；比较时对同频来源波求和，
不将其等同于系统图把不同相干类别功率相加的总功率读数。

## 本阶段工程验收

- MSVC Debug / Release 清理重建，完整 CTest 各 74/74。
- 安装后独立 C/C++ 消费，两种配置各 2/2。
- 独立 Python 3.12 wheel：65 项 API、27 项系统图测试通过。
- 递归 JSON/CLI 示例运行成功，C/C++ 格式检查 106 个文件通过。

这些是来源代数、RF 多项式链路和既有回归的证据，不是新的 SystemVue
递归 RFAMP 兼容性声明。本轮没有改写既有 SystemVue 原始采集或放宽比较阈值。
