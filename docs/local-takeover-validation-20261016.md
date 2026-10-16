# 本地同步、审查与回归（2026-10-16）

本轮从本地 `6870b47` 快进到 GitHub main
`8984da6f9d503b4927888071046ad66c551709a4`（tree
`ff723d70b490c5f47105d9ab234e2efa544e7136`），接收 22 个提交。
同步前工作区干净，没有重置、覆盖或丢弃本地修改。该精确基线的
[CI 38846356382](https://github.com/minexin/RFSim/actions/runs/38846356382)
已核实为 18/18 作业成功；不能把它当成本轮修复提交的 CI。

本轮关闭实际安装 X 文件的格式回放缺口，并修复本机复现的三类工具问题。
没有修改 C++ 数值核心、历史期望、厂商容差或独立样本身份。
本轮新的 SystemVue 分析提交为 **0**；长期兼容目标尚未完成。

## 上游变动与审查结论

| 变动 | 本轮检查与边界 |
| --- | --- |
| 线性 MNA DC/RMS AC、复参考 MNA→S | 检查盖章、源与负载、LU 和复功率波约定；未发现可复现的数值错误。两项安装测试遗漏默认配置，已修复。独立 oracle 的旧仓外生成脚本不全在仓内，不能声称生成过程已完全复现。 |
| PLL、小信号复相位、有限频带 jitter、弱噪声块保留 | 检查公式、单位、积分边界、独立块与重构契约，结合已有回归；本轮没有新增厂商噪声或 PLL 数值验收。 |
| Touchstone raw 噪声写出、X 数据 reader/catalog | 检查表示边界；实际四个 X 文件通过安装包 reader 的冻结格式比较。该比较不验证全部系数的物理归一化。 |
| 放大器、来源和两级级联的原生归档回放 | 属于历史数据重放，不增加独立仿真次数；RFAMP A/B/C/D 与 380 holdout 的既有身份保持。 |
| 行为系统及 MNA/噪声等安装 SDK 示例 | 保持“同一器件与接线 → 外部完整 S / 内部端口量”的产品方向；示例成功不自动等于厂商同名测量。 |

审查针对本次增量和已暴露问题，不是所有器件与输入空间的完整正确性证明。

## 修复与失败保留

1. **Windows pathname stat 与 descriptor fstat 的 ctime 口径不同。**
   Python 3.12.14 下，实际四文件的前者为 creation time、后者为 change time，
   原收集器均在读取前误报 `selected_file_changed_before_read`。
   仅 Windows 跨 API 的比较排除 ctime；device/inode/size/mtime/mode 仍逐项比较，
   同一 API 的前后快照仍包含 ctime，POSIX 保留全字段比较。任何真实变化仍失败且不重试。
   新增真实 metadata 修改夹具及两平台字段变异测试。
2. **中文 Windows 默认文本编码。**
   证据检查器读中文 JSON 时使用系统 GBK，复现 `UnicodeDecodeError`。
   七个 JSON 读取点显式使用 UTF-8，新增完整注册表与拒绝变异在 GBK 默认环境下的回归，纳入 CI。
3. **MNA 安装脚本缺少默认配置。**
   空 `CONFIG` 导致构建参数成为 `--config --parallel 2`，两个真实纯 SDK 流程均失败。
   修复默认 Release，并新增实际安装、整体迁移、双翻译单元编译/运行的回归，
   同时验证两个 producer/consumer 的配置均为 Release。

另外修正两个测试夹具的可移植性：显式输出 CRLF/LF 字节，避免宿主文本换行转换；
超时测试先等待真实子进程完成输出，再执行原 0.2 秒超时/终止流程。
后者保留单次调用、原始字节、非零退出和进程结束断言，没有放宽生产超时或允许重试。

第一次原生 Release 回归为 **148/149**：仅 `sdk_archive_test` 在深层输出路径中
触发 MSBuild 的 260 字符限制。改用仓库根下较短的新构建目录重新全量验证；
未改系统长路径设置、打包实现或测试判定。旧失败和新结果分别归档。
初次配置还暴露沙箱重复 PATH 与 MSBuild FileTracker 权限问题；后续通过进程局部环境
和已授权的宿主编译运行，不修改系统环境。

## 新验证结果

结果以 [机器可读汇总](../validation/local-takeover-validation-20261016.json)、
[原始证据归档](../validation/local-takeover-raw-20261016.zip)及
[逐文件哈希清单](../validation/local-takeover-manifest-20261016.json)为准。

| 检查 | 结果 |
| --- | --- |
| MSVC 19.44 / C++17 原生 Release | 150/150，无跳过，446.98 秒 |
| MSVC 19.44 / C++17 原生 Debug | 150/150，无跳过，540.77 秒 |
| X replay 完整合成与安装集成测试 | 38/38，无跳过；安装 SDK 和原生 probe 路径显式指定 |
| GBK 默认编码回归 | 1/1；完整检查器拒绝 34 个非法元数据变异、40 个 raw-CI 绑定变异 |
| MNA 定向回归 | Release 6/6，Debug 5/5；含两项原安装脚本及新增空配置流程 |
| 四个实际安装 X 文件 | 4/4 `exact_evidence_match`，3 个独立内容哈希，0 次仿真 |

工具为 CMake 3.31.6-msvc6、Visual Studio 2022 x64、Python 3.12.14。
原生全量回归关闭 C API/CLI，开启 C++ 示例；Python 只用于开发证据工具。
本地结果不代替 Linux/macOS 或 C API 的 CI。

实际 X 文件回放在 04:00:55–04:01:03 UTC 执行，来自显式安装的 RFModel SDK，
没有源码 include fallback，也没有启动 SystemVue。
冻结格式证据 SHA-256 为 `432022e69d4cb1010b27d73b18adc03488d106989f69900ccc5592421963ec59`，未更新。
工具结果里的 `observed_systemvue_version` 仍为 `unknown`，因为该工具不探测版本；
另一次只读 exe VersionInfo 检查观察到 `2023.0.0.11903`，不回填覆盖工具结果。
格式成功只覆盖登记的类型、顺序、计数和样本字段，`numerical_model_compatibility=false`。

## 已核定义与下一项实验

本地实际读取 SystemVue 2023 QCH 的 27 页，记录页面哈希与事实摘要；
完整帮助、QCH、安装 MDF/XNP、许可证及凭据不在公开归档中。
本次核清以下口径，不能把它们互相代换：

- Linear `ZPORT` 来自端口件的参考阻抗；RF System `RFZIN/RFZOUT` 是节点在中心频率的模拟输入/输出阻抗；`SZIN/SZOUT` 来自模型参数或默认值。
- `CP` 包含各谱类型和方向，`DCP` 仅为路径起点来源的前向 desired 分量；首节点的源内部参考面不等于首器件实际送入功率。
- `CV/CNV` 帮助使用 peak 与默认 dBV；不能直接等同 C++ RMS V/I，也不能据此推导任意随机噪声的 RMS 换算。
- CW 谱有有限宽度，通道积分边界必须明确；第一次实验用足够宽且固定的通道，避免边界截取混入测量差异。

下一步按[冻结的最小同电路实验](systemvue-linear-rfsystem-minimal-plan-20261016.md)，
先以两个 50 Ω 串联 RES、100 MHz、明确的 50 Ω 源/负载和 0 dBm available power，
分别核完整复 S 与有参考面的内部端口功率/电压。先回读设置与导出量身份，再评分；
本轮只冻结实验，没有采集或验收这些新厂商数值。

DC/RC 原八次分析仍为 **零项比较验收**。精确 I(V)/Q(V) 模型入口、求解节点 V/I 导出、
器件专有参数律、完整 X 系数归一化、路径归因、复参考及完整器件族覆盖仍需后续证据。
本次提交不创建 release/tag，也不关闭许可、公开分发或稳定 API/ABI 决策。
