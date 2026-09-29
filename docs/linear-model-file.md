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
终端温度和源噪声不会自动加入。独立信号源通过下述 signal_boundaries 提供，
不用于 S 参数和内生噪声提取。

## 器件和数值

每个器件有唯一非空字符串 id，以及恰好一个 s、s_samples 或 model：

- s 是在所有频率复用的方形矩阵；
- s_samples 是与 frequencies_hz 一一对应的矩阵数组，各矩阵端口数必须相同。
- model 是下述参数化模型，在每个频率调用 C++ 实现计算矩阵。

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

当前文件格式覆盖显式 S 矩阵、两类传输线及小信号放大器的线性网络扫描，尚未实现其他参数化器件、Touchstone
文件引用、自动重归一化、非线性或变频网表。它不是 SystemVue 工作区导入器，也不代表
整个 RF System Analysis 工作流已经验收。

2026-09-29 验证：Debug/Release 动态库上的 Python 专项回归各通过十二项；
构建并安装 Python wheel 后，Release 动态库上的十二项测试再次通过，包含命令行子进程。
仓库示例另行成功输出至 `build-reference/linear-noise-result.json`。本轮未改 C++ 核心，
没有重复执行全部 C++ 测试；本次提交的跨平台 CI 结果仍待核验。

## 参数化传输线

model.type 为 `transmission_line` 时，必填 characteristic_ohms、delay_s，可选
propagation_loss_db（默认 0）。特性阻抗必须为正，延迟及单程传播损耗非负。
model.type 为 `rlgc_line` 时，必填 length_m，可选 resistance_ohms_per_m、
inductance_h_per_m、conductance_s_per_m、capacitance_f_per_m，默认均为 0；
所有参数非负有限。两个模型均采用顶层 reference_ohms，不接受每器件独立参考阻抗。
每个对象的未知字段或未知 type 都被拒绝，没有表达式执行或动态插件导入。

`examples/rlgc-line.json` 给出 0.2 m、L=1 µH/m、C=100 pF/m 的无损线，在 50 Ω
参考下计算 0、125 MHz、250 MHz 三个频点。125 MHz 处 S11=0.6、S21=−0.8j。
参数模型可与显式矩阵器件混合连接，两种噪声输入方式继续适用。
物理和数值范围见 [均匀传输线](transmission-line.md) 与 [RLGC 传输线](rlgc-transmission-line.md)。

2026-09-29 参数模型扩展验证：MSVC Debug/Release 全套各 42/42，安装消费者各 2/2；
Python 十三项测试在安装后的 Release 动态库上通过。包含两个模型的 JSON 数值一致性、
解析四分之一波长响应、直流电阻极限及未知参数拒绝。跨平台及 SystemVue 实测仍待核验。

## 双向小信号放大器

model.type 为 `linear_amplifier` 时，必填 gain_db，可选 gain_phase_degrees（默认 0）、
reverse_isolation_db（默认 50）、reverse_phase_degrees（默认 0）、
input_impedance_ohms 和 output_impedance_ohms（默认都为 50 Ω，可用实部/虚部数组）。
参考阻抗来自顶层；即使顶层参考不是 50 Ω，两个端口阻抗的默认值仍为 50 Ω，
需要匹配到其他阻抗时须显式给出。gain_db 定义 |S21| 的幅度，不是任意失配条件下的传输功率增益。

`examples/amplifier-chain.json` 将幅度 0.5 的匹配衰减器与 20 dB、90° 放大器连接，
输出 S21=5j、S12=0.005。此模型使用现有 C++ LinearAmplifierModel，
不包含压缩、AM/PM、NF 自动噪声或 DC 阻断；模型语义见 [放大器说明](linear-amplifier.md)。
包含有源器件时不能使用全器件被动 temperature_k 模式；可显式提供完整的
intrinsic_noise_samples，由核心校验与传播。这不是 SystemVue RFAMP 完整兼容实现。

2026-09-29 放大器接口验证：MSVC Debug/Release 全套各 42/42，安装消费者各 2/2；
Python 十五项测试在安装后的 Release 动态库上通过。新增案例覆盖复数阻抗映射、
正反向相位、匹配衰减器级联、奇异阻抗错误码及有源器件拒绝被动噪声计算。
本次跨平台 CI 及 SystemVue 新实测均未计入此验收。

## 逐器件内生噪声

器件可增加 noise 字段，四选一：

- `{"temperature_k": 290}`：对该器件每个频点调用被动热噪声计算，器件须无源；
- `{"covariance": 矩阵}`：在各频点复用 W/Hz 内生协方差；
- `{"covariance_samples": [矩阵, ...]}`：与 frequencies_hz 逐点对应；
- `{"noiseless": true}`：显式设置零内生噪声，不暗示真实器件物理无噪声。

矩阵行列使用器件局部端口顺序，尺寸必须匹配。只要一个器件使用 noise，就要求
所有器件明确设置，且不允许同时设置顶层 temperature_k 或 intrinsic_noise_samples。
逐器件模式假设不同器件噪声独立，装配为块对角全局协方差，再由核心检查及传播；
需要跨器件相关项时仍用顶层完整矩阵模式。终端噪声和源噪声不会因此自动加入。

`examples/amplifier-noise.json` 为匹配、单向、功率增益 100 的放大器和功率增益 0.25
的被动衰减器。放大器仅输出端设置内生噪声 `k*290*100 W/Hz`，对应这个匹配案例
的噪声因子 2；这不是通用 RFAMP 噪声协方差模型。末端内生噪声为 `25.75*k*290`，
总信号功率增益为 25，因此匹配参考源下 F=`1+25.75/25=2.03`，与 Friis 公式一致。
JSON 结果仍只报告内生噪声，示例的噪声系数换算由测试单独完成。

2026-09-29：新增混合有源/无源链路、逐频点协方差、显式无噪声以及遗漏/冲突/非法矩阵
回归。Python 专项扩展至二十二项；完整跨平台和 SystemVue 比对仍待完成。

## 信号激励与端口功率

可选顶层 signal_boundaries 为数组，每个外部端口必须恰好对应一个对象。
对象必填 port（端点），可选 reflection（复数，默认 0），以及 source（常数复数波幅）
或 source_samples（与 frequencies_hz 对应的复数波幅数组）。两个源字段互斥；
没有源字段则波幅为 0。所有波幅单位 sqrt(W)，反射系数无量纲。
端口边界为 `a=source+reflection*b`，source 不是失配后求得的实际入射波，
也不是直接指定的源可用功率。内部终端仍没有独立源。

每个频点先提取原有 S 和内生噪声，再施加全部外部信号边界，调用核心完整网络求解。
输出额外 signal 对象，含 relative_residual 和 ports：每个全局端口按器件顺序列出
port、incident、outgoing、incident_power_w、outgoing_power_w、net_into_device_w。
净流入器件功率为 `|a|²−|b|²`，输出负值表示器件向该端口外部送出净功率；不是增益 dB。

必须区分结果条件：s 与 noise_w_per_hz 仍以原来未施加信号边界的外部参考条件提取，
signal 则是在给定源/负载反射下求解。noise_w_per_hz 不能直接当作这些信号边界下的
负载总噪声。需要加载条件下的噪声时，另行设置下面的 noise_boundaries。

`examples/mismatched-signal.json` 为单位直通、源反射 0.25、负载反射 0.5，
源波幅按频率从 1 变为 j。首频点输入入射波 8/7、输入反射波 4/7，负载吸收
48/49 W；二频点相位旋转 90°，功率不变。库不会使用 |S21|² 直接代替该反馈解。

2026-09-29：Python 专项增至二十四项，覆盖反馈解析波量、全局端口功率守恒、
逐频点源相位、边界缺失、源字段冲突及 S/噪声参考条件保持。

## 外部热噪声边界与失配反馈

可选顶层 noise_boundaries 为数组，每个 external_ports 中的端口必须恰好出现一次。
每项必须明确提供 port、reflection（常数复数）和 temperature_k（非负有限温度）。
反射幅度不得超过 1。必须同时指定已有的一种内生噪声模式；需要无噪声器件时
显式使用器件 noise.noiseless，而不是省略器件噪声。边界热噪声彼此独立，
其发射协方差对角元为 `k*T*(1-|reflection|²)`，并与器件内生噪声独立。

每个频点增加 loaded_noise 对象，incident_w_per_hz、outgoing_w_per_hz 为完整复数
协方差矩阵，net_into_device_w_per_hz 为入射与出射对角元之差。
矩阵和净功率数组均严格按 external_ports 顺序；负净功率表示网络向外部端口送出
净噪声功率密度，不能等同于含热发射负载接收的总噪声。单位均为 W/Hz。
核心求解包含多次反射；奇异反馈会报错，不输出伪结果。

noise_boundaries 与 signal_boundaries 是分别指定的分析条件，不自动继承反射参数。
若要在同一加载条件下比较信号与噪声，应明确给出相同反射系数。原 s 和
noise_w_per_hz 保持匹配参考条件。内部 terminations 仍为无独立热发射的反射边界；
此接口暂不提供频变边界或相关外部噪声，相关边界可使用 C/Python loaded_noise API。

examples/loaded-noise.json 为 290 K、幅度传输 0.5 的衰减器，输入接匹配 290 K 源，
输出接反射 0.5、0 K 负载。记 q=k*290，出射对角元为 [0.8125q,q]，
出射互相关为 0.25q，净流入为 [0.1875q,-0.75q]。测试还覆盖匹配热平衡、
外部端口重排、信号条件独立及缺失/重复/非法边界。

2026-09-29：本次 Python 专项共 29 项，在 MSVC Debug/Release 动态库上均通过，
并通过 Release CLI 执行新示例、生成 JSON 结果。本次未运行新的 SystemVue 对照，
也不把这些接口回归作为完整 RF Design 库兼容验收。
