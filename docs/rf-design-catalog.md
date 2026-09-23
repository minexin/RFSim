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

每个帮助页路径作为稳定的条目标识，后续建立独立评估记录，包含参数和单位、端口/参考约定、适用分析、当前 API 映射、未覆盖行为、参考用例和误差阈值。生成的目录清单不替代人工评估记录。

优先核对 Amp (RF)、Mixer、File N-Port 和理想集总元件与当前实现之间的差异，然后扩展滤波、耦合/功分、传输线、微带、开关和相控阵等类别。Linear Analysis 帮助不包含微带设计工具，不等于可以排除 RF Design 目录内的微带器件模型；最终范围仍以用户要求的整个相关器件库为目标。
