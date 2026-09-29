# 线性网络 JSON 文件 v1

此格式将网络输入及频率样本保存在可重放文件中。Python 负责读取、校验及编排，
数值求解和噪声传播全部调用 C++ 核心。可运行示例为 `examples/linear-noise.json`。

```powershell
python -m rfmodel examples/linear-noise.json `
  --library build-msvc/Release/rfmodel_c.dll `
  --output build-reference/linear-noise-result.json
```

需先安装当前版本的 Python 包，或设置 PYTHONPATH 指向仓库 python 目录。
输出目录应已存在。只有读取、验证、全部频点求解及 JSON 序列化成功后才写结果，
错误返回码为 1，并在 stderr 给出原因。输出路径不能等于输入模型或动态库路径。
该保证不覆盖磁盘写入途中断电或 I/O 失败；输出成功时会覆盖指定的已有结果文件。

## 顶层字段

| 字段 | 含义 |
|---|---|
| format | 必须为 `rfmodel.linear-network` |
| version | 整数 1，其他版本明确拒绝 |
| reference_ohms | 所有端口共用的正实参考阻抗，默认 50 |
| frequencies_hz | 1–10000 个严格递增、非负、有限的频率，单位 Hz |
| devices | 非空器件数组，按其排列顺序分配全局端口，总端口不超过 1024 |
| connections | 可选，每项为两个端点的数组 |
| terminations | 可选，每项为 `{port: 端点, reflection: 复数}`，默认反射系数 0 |
| external_ports | 有序外部端点数组，决定结果矩阵的行列顺序 |
| temperature_k | 可选，给所有器件指定统一非负开尔文温度并计算被动内生噪声 |
| intrinsic_noise_samples | 可选，每个频率一张全局端口内生噪声矩阵，单位 W/Hz |

两种噪声输入方式互斥。temperature_k 模式要求所有器件无源，假设器件间噪声独立；
显式矩阵模式可以包含器件内与跨器件复数相关项，由核心校验厄米性及半正定性。
没有噪声字段时只输出 S 参数。外部端口匹配且无噪声，其他端口必须连接或设置终端；
终端温度和源噪声不会自动加入。本格式不允许独立信号源。

## 器件和数值

每个器件有唯一非空字符串 id，以及恰好一个 s 或 s_samples：

- s 是在所有频率复用的方形矩阵；
- s_samples 是与 frequencies_hz 一一对应的矩阵数组，各矩阵端口数必须相同。

不进行频率插值、外推或自动排序。矩阵行是出射端口，列是入射端口；实数可直接写数字，
复数写为 `[实部, 虚部]`，例如 `[0, -0.5]` 表示 −0.5j。
端点写为 `["device_id", 局部端口编号]`，编号从 0 起，不接受 bool、负数或浮点编号。
重复 JSON 键、未知字段、非标准 NaN/Infinity、非有限数、重复 ID、缺失频率样本、
重复端口连接、未分配内部端口均报错，不忽略或自动补齐。

## 结果与验证

结果 format 为 `rfmodel.linear-results`、version 为 1，保留参考阻抗与外部端点顺序。
samples 的每项包含 frequency_hz、s，以及请求噪声时的 noise_w_per_hz。
输出复数始终采用 `[实部, 虚部]`，即使虚部为零。

示例第一频点 S21 为 −0.5j，第二频点为 −0.5；两者输出内生噪声均为
`k*290*0.75 W/Hz`。回归另覆盖复相关噪声、反射终端、格式版本、重复字段、
非法索引及命令行失败时保留既有结果。

当前文件格式覆盖显式 S 矩阵的线性网络扫描，尚未实现参数化器件序列化、Touchstone
文件引用、自动重归一化、非线性或变频网表。它不是 SystemVue 工作区导入器，也不代表
整个 RF System Analysis 工作流已经验收。

2026-09-29 验证：Debug/Release 动态库上的 Python 专项回归各通过十二项；
构建并安装 Python wheel 后，Release 动态库上的十二项测试再次通过，包含命令行子进程。
仓库示例另行成功输出至 `build-reference/linear-noise-result.json`。本轮未改 C++ 核心，
没有重复执行全部 C++ 测试；本次提交的跨平台 CI 结果仍待核验。
