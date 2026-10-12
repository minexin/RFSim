# SystemVue 2023 RF Design 器件目录基线

本机 `Help/systemvue.qch` 的 `rfdesign/RF_Design_Kit_Library.html` 索引包含 243 个不同的器件帮助链接：241 个位于 rfdesign，2 个位于 algorithm。以帮助页路径去重，同一器件在多个分组出现时只计一次。原始索引页面的 SHA-256 和每项名称、路径保存在 `validation/systemvue-2023-rf-catalog.json`。

这是索引链接清单，不证明安装产品的所有许可扩展模块均已枚举，更不证明 RFModel 的参数、端口、算法或结果兼容。所有条目的 assessment 暂为 unassessed、systemvue_comparison 为 not_executed。不得根据通用 S 参数或理想器件原语自动将同类条目标为支持。

## 可复现提取

```powershell
& 'C:/Program Files/Keysight/SystemVue2023/Python/python/python.exe' `
  scripts/reference/extract-rf-catalog.py `
  'C:/Program Files/Keysight/SystemVue2023/Help/systemvue.qch' `
  validation/systemvue-2023-rf-catalog.json
```

脚本仅依赖 Python 标准库，只读访问 QCH，校验 Qt 解压长度、索引唯一性及目标帮助页存在性。输出只含事实性器件名称和链接，没有复制厂商模型实现或完整帮助正文。2026-09-24 再次提取与已保存 JSON 完全一致。

## 后续逐项验收

### 模型级清单（2026-09-24）

`validation/systemvue-2023-rf-models.json` 进一步读取全部 243 个器件页的 Model 表，得到 281 个不同模型页、401 条器件—模型关联。所有器件页均找到模型链接；这仅说明目录链接完整解析，不能作为模型行为已经实现的证据。算法库的 Delay、Mixer 等跨库引用保留在范围中，不因所在目录不同而排除。

```powershell
& 'C:/Program Files/Keysight/SystemVue2023/Python/python/python.exe' `
  scripts/reference/extract-rf-models.py `
  'C:/Program Files/Keysight/SystemVue2023/Help/systemvue.qch' `
  validation/systemvue-2023-rf-catalog.json `
  validation/systemvue-2023-rf-models.json
python scripts/reference/test_extract_rf_models.py
```

模型提取器只识别 Model 列，排除导航和描述列链接；按解码后的帮助页路径去重，保留别名、反向器件索引以及器件页和模型页的 SHA-256。目录源摘要不匹配或模型页缺失时明确失败，未识别的器件页列入人工审查清单。合成 QCH 回归覆盖双列表格、描述列干扰链接、转义路径、片段去重、别名、遗漏报告、源不一致和缺页；Windows 上也检查数据库连接退出后能释放文件。

首批语义核对见 [放大器和衰减器差异](rf-amplifier-attenuator-assessment.md)。
功分器及混合耦合器的参数、端口相位与受限映射见 [多端口模型评估](systemvue-multiport-assessment.md)。

每个帮助页路径作为稳定的条目标识，后续建立独立评估记录，包含参数和单位、端口/参考约定、适用分析、当前 API 映射、未覆盖行为、参考用例和误差阈值。生成的目录清单不替代人工评估记录。

优先核对 Amp (RF)、Mixer、File N-Port 和理想集总元件与当前实现之间的差异，然后扩展滤波、耦合/功分、传输线、微带、开关和相控阵等类别。Linear Analysis 帮助不包含微带设计工具，不等于可以排除 RF Design 目录内的微带器件模型；最终范围仍以用户要求的整个相关器件库为目标。

## 理想原语接口覆盖（2026-10-09）

已有 R/L/C、匹配衰减/延迟、隔离功分器和正交耦合器核心现提供[统一 C/Python/JSON 入口](passive-model-interfaces.md)，可形成参数化 RC 和相干网络案例。这是 RFModel 原语层的工程覆盖，未完成厂商模型参数及实测对照；原目录条目的 assessment 不因此自动升级。

新增[理想无损 Butterworth 原型](butterworth-filter.md)，四类频率变换已贯通接口。该实现尚无滤波器实测证据，未升级 LPF_BUTTER/HPF_BUTTER/BPF_BUTTER/BSF_BUTTER 的兼容评定。

新增[理想无损 Chebyshev I](chebyshev-filter.md) 四类参数化响应及接口，含独立纹波和带边衰减。仅完成解析/工程回归，LPF_CHEBY/HPF_CHEBY/BPF_CHEBY/BSF_CHEBY 的厂商行为评定仍待实测。

## 全部模型的参数表元数据（2026-10-12）

[参数清单](../validation/systemvue-2023-rf-parameters.json)逐页读取既有 281 个模型，保留全部 243 个器件和 401 条关联。共提取 **3,404 条参数记录**及 **13 条显式端口表记录**；参数记录包含不同表中的重复项，并非 3,404 个独立功能。默认值、单位、类型、运行时可调标记和 Range 列均保留为未经求值的文本。空单元格为 `""`，不存在的列为 `null`；`None`、`none`、枚举值及数组表达式不会自动转换成 Python 值。没有 Range 列不代表参数无限制，正文可能另有约束。

每条参数用帮助页、表序号和行序号定位，保存页面与规范化表内容的 SHA-256。输入目录 JSON 指纹将 CRLF 统一为 LF，避免 Git 跨平台检出行尾导致误报；QCH 与解压页面仍按原始字节取指纹。清单不复制参数描述正文；描述只留摘要指纹。原目录、模型关系、已有兼容报告保持原状态，新记录统一标为 `metadata_only` / `not_assessed_here`，不会把已有厂商失败改成“未执行”。

| 需人工复核的页面 | 保留的问题 |
|---|---|
| `rfdesign/ArrayXParam.html` | 主参数表已提取；补充参数说明表含嵌套表，正文契约仍需人工核对 |
| `rfdesign/GND.html` | 未发现参数记录；不据此推断它没有参数或已经兼容 |
| `rfdesign/MultiSource.html` | 主表和参数选项表都列出 PORT/R，保留两份原文及位置，不自动合并或决定优先级 |

算法库与硬件库引用仍在范围内。只有五个模型页提供本提取器识别的显式端口表；其余页面的 `not_found_in_explicit_tables` 表示端口图/正文尚未解释，不能用于猜测端口数。每个模型仍需补充参数枚举、正文范围、参考面、适用分析、API 映射和数值证据，A07 仍开放。

```powershell
python scripts/reference/extract-rf-parameters.py `
  'C:/Program Files/Keysight/SystemVue2023/Help/systemvue.qch' `
  validation/systemvue-2023-rf-catalog.json `
  validation/systemvue-2023-rf-models.json `
  build-reference/rf-parameters-replay.json
python scripts/reference/test_extract_rf_parameters.py
# 独立 DOM 交叉核对需要 lxml；提取器和上述测试仅用标准库。
python scripts/reference/verify-rf-parameters.py `
  'C:/Program Files/Keysight/SystemVue2023/Help/systemvue.qch' `
  validation/systemvue-2023-rf-parameters.json
```

提取前重新验证目录和全部模型关联，核对页面指纹及提取前后 QCH 指纹；复杂、缺列、重复及未识别表格显式列入复核。CLI 拒绝覆盖输入或其硬链接，失败保留旧输出，成功后原子替换。目录提取器同时修复 SQLite 连接退出时未显式关闭的问题。

[本阶段验证](../validation/rf-parameter-inventory-validation-20261012.json)记录 Windows/Linux 的 15 项新测试及 2 项既有测试、全量独立 DOM 核对、原目录和用户草稿保护。测试加入三平台六配置 CI；当前提交远程状态另行核验。没有改动原生求解器、ABI、wheel 或新增 SystemVue 分析调用。
