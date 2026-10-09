# 显式高阶放大器与共同压缩

`CoherentHighOrderAmplifier` 将基波压缩、公共输入限幅与二至十一阶电压多项式
组合成原生模型，并提供 C ABI、Python 和 JSON 系统图入口。调用者显式提供
非线性系数；本阶段不自动推导 SystemVue RFAMP 的高阶系数。

## 参数和响应

构造参数为功率增益、输出 P1dB、输出饱和功率、`nonlinear_voltage_coefficients`
以及正实数参考阻抗。系数数组依次表示 `a2, a3, ..., an`，最多十项；
省略的高阶系数视为零，空数组表示只传播压缩后的直接项。
数组不包含常数项或线性增益。若使用
[截点转换接口](polynomial-intercepts.md)，应传入返回数组的 `coefficients[2:]`。

输入与输出复波沿用平方模为 W 的固定参考阻抗归一化。电压多项式系数仍是
电压域系数，不是功率或 dB 系数。模型先按实际相干分组归并输入，
同组加复波、不同组加功率，再用该总驱动计算唯一的公共工作点：

- 所有直接项乘 `fundamental_amplitude_gain`，采用现有 P1dB/饱和响应。
- source 类输入乘同一个 `nonlinear_input_scale`，再展开显式高阶多项式。
- 非线性产物包括落在输入载频上的来源项；它们与直接项分别返回。
  直接项、来源项及合并总谱的 SystemVue 语义不能相互替代。
- 返回的 `inputs` 是归并后的原始输入，未乘限幅比例。返回工作点包含限幅后
  总功率以及已有二、三阶系数字段；后两项只报告 a2/a3，缺省为零。

默认只接受 source 类输入。设置 `propagate_distortion=true` 后，已有谐波/互调
也参与公共驱动，按同一直接增益传播，但不参与本级新产物生成。
需要全部输入继续多项式混频时，可使用现有 `CoherentPolynomial`；当前放大器
接口不代表 RFAMP 的失真再混频、噪声、AM-PM 或反馈求解。

## C++、C 和 Python

公开头文件为 `rfmodel/coherent_highorder_amplifier.hpp`。C++ 提供
`operating_point(total_input_power_w)` 与
`evaluate(spacing_hz, inputs, reserved_group_max, propagate_distortion)`。

新增 C 符号为 `rfmodel_get_highorder_amplifier_operating_point` 和
`rfmodel_highorder_amplifier_evaluate`。结果复用十一索引的
`rfmodel_coherent_polynomial_term_v2`，旧结构和函数保持原有 ABI。
空系数数组可用 NULL/0；输出缓冲区、计数与工作点必须互不重叠，容量不足或
非法参数时所有输出保持不变。Python wheel 需要搭配包含新符号的原生库。

```python
from rfmodel import CoherentComponent, Library, SpectrumKind

library = Library("build-msvc/Release/rfmodel_c.dll")
response = library.highorder_amplifier(
    1e8,
    [CoherentComponent(10, SpectrumKind.SOURCE, 1.0, 1, 0.05 + 0.01j)],
    [0.1, -0.2, 0.01],  # 显式 a2、a3、a4 示例值，不是 RFAMP 默认值
    power_gain_db=10,
    output_p1db_dbm=20,
    output_saturation_dbm=23,
)
print(response.operating_point)
```

`Library.highorder_amplifier_operating_point` 可单独求公共工作点。
原生每项保留本地阶数及原始归并输入的一基带符号索引，负号表示共轭。
直接项保留输入相干编号；新编号只保证避开输入与保留区间，是不透明的本地
身份，不保证与既有三阶枚举器逐数字相同。完全相消后的零直接组仍保留。

原生输入最多 4096 项；存在非零非线性系数时最多 64 个活跃载波输入；
直接与非线性输出合计最多 4096 项，枚举还受现有多项式工作量上限约束。
这些资源限制在 C++、C、Python 原生求值入口一致。

## 系统图

新增 `highorder_amplifier` 阶段，需要 `id/type/input/output`、三个增益/压缩
参数、`nonlinear_voltage_coefficients` 和 `max_source_order`；可选
`propagate_distortion`，默认 false。完整示例见
[coherent-highorder-amplifier.json](../examples/coherent-highorder-amplifier.json)。

```powershell
$env:PYTHONPATH = "$PWD/python"
python -m rfmodel examples/coherent-highorder-amplifier.json --library build-msvc/Release/rfmodel_c.dll --output build-reference/highorder-amplifier-result.json
```

图层先从真实物理流求总功率，再调用原生公共工作点与多项式接口，向各来源贡献
施加公共响应。不会把相消前各项的独立功率和用作驱动，也不通过总输出除以
总输入来反求增益，因此可保存总波为零时相互抵消的非零历史贡献。

`max_source_order` 必须是 1..256 的整数，按根来源因子数同时裁剪直接传播
和新生成的贡献；经过混频器的 RF/LO 因子都计入，沿用现有图层来源政策。
裁剪不会追溯改变该节点输入的物理总驱动。测量中记录公共响应、生成项数、
按来源阶数丢弃的新项，以及 `discarded_direct_contributions`。
图层有贡献展开和累计存储限制；其来源表达式数量不等同于原生物理输入项数。
这不是对 SystemVue 全局 MaxOrder 规则的兼容承诺。

## 证据与边界

- 原生测试对照独立稀疏傅里叶卷积和既有三阶模型，覆盖高阶相位、75 Ω、
  压缩、极大驱动、完全相消、失真传播与资源边界。
- C 测试覆盖十一阶来源结构及失败时输出保持不变；系统图测试覆盖物理驱动、
  混频后的相消历史以及直接/生成贡献的来源阶数限制。
- `scripts/reference/test_highorder_model_reference.py` 经新统一原生入口重放
  七组既有 SystemVue 采集，1413 个独立评分来源项在原 1e-7 相对误差容差下通过。
  奇数阶校准案例不参与奇数阶评分，10 个未记录标签仍按缺项检查。
- 重放使用[实测输入端口电压](systemvue-highorder-input-reference.md)和归档的
  偶数阶规则/固定参数奇数阶识别系数。本轮没有新增 SystemVue 采集，未验收
  直接项或合并总谱，未实现自动奇数阶系数或完整网络输入求解。

本阶段 MSVC Debug/Release 完整 CTest 各 83/83，安装后的独立 C/C++ 消费者
各 2/2。独立 Python 3.12 从新 wheel 导入，75 项 API、48 项系统图和归档重放
测试通过。wheel 命令行示例生成 670 个非线性项，输入功率 0.0046 W，
公共幅度增益 3.036621625415409。跨平台 CI 结果需绑定对应提交另行核验。
