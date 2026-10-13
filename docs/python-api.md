# Python 线性网络接口

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

`python/rfmodel` 使用标准库 ctypes 调用 C ABI，运行不依赖 NumPy；要求 Python 3.9 及以上。
这是 RFModel 自身的脚本接口，与用于采集参考数据的 SystemVue 自动化脚本分别使用。
原生动态库需由 CMake 单独构建或安装，Python 包不包含二进制，也不会自动下载或启动外部程序。

```powershell
python -m pip install ./python
```

通过 `Library` 显式提供原生库路径，Python 与库的架构必须一致。Windows Release 使用
`rfmodel_c.dll`、Debug 使用 `rfmodel_cd.dll`；Linux/macOS 使用对应的 `.so`/`.dylib`。
加载时检查 ABI 版本，当前要求 1。

```python
from rfmodel import Library

library = Library("build-msvc/Release/rfmodel_c.dll")
with library.network(reference_ohms=50.) as network:
    network.add([[0, -0.5j], [-0.5j, 0]])
    network.add([[0, 0.2], [0.2, 0]])
    network.connect(1, 2)
    s = network.external_s([0, 3])
    print(s[1][0])  # -0.1j
    network.terminate(0, source=2.)
    network.terminate(3)
    waves = network.solve()
    print(waves.outgoing[3])  # -0.2j
```

## 数据与生命周期

- `add` 接受方形嵌套序列，元素可为 Python 数值或 complex，返回器件首个全局端口编号。
  默认采用网络参考阻抗；显式指定不同阻抗会报错。输入会复制到原生库。
- `external_s` 返回按所选端口顺序排列的不可变复数行元组；只提取矩阵，不改变网络边界。
- `terminate` 的约定为 `a=source+reflection*b`；`solve` 返回 `Waves`，包含
  incident、outgoing 两个复数元组和 relative_residual。功率波单位为 sqrt(W)。
- 推荐使用 with 或显式 `close()`；close 可重复调用，垃圾回收仅作为释放兜底。
  已关闭的网络在进入原生库前即被拒绝；with 不吞掉调用者异常。
- 每个网络有独立锁，方法调用与 close 不会同时访问同一句柄；多个方法组成的事务
  若要求不被其他线程插入，仍需调用者同步。不同网络可以并行使用。
- 原生错误映射为 `RFModelError`，保留数值 status 和错误文本；Python 层形状、
  索引、类型校验使用 ValueError/TypeError。负索引、超大整数、浮点和 bool 索引不会截断成 size_t。

## 验证与范围

`python tests/test_python_api.py <native-library-path>` 使用真实动态库测试复数级联、
错误恢复、奇异网络、形状/非有限值/索引校验、显式及自动释放。CMake 找到 Python 3.9+
时会注册此测试；CI 设置 `RFMODEL_REQUIRE_PYTHON_TESTS=ON`，缺少解释器即配置失败。

当前封装覆盖已公开的线性网络 C ABI；[JSON 文件与批处理](linear-model-file.md)支持
显式频率样本、参数化传输线及小信号放大器的网络重放。其他器件、非线性及混频接口仍待扩展，完整系统分析接口尚未完成。

2026-09-29 本地验证：Python 3.10.6，六项 Python 测试通过；加入 CTest 后 MSVC
Debug/Release 各 42/42。通过 pip 默认隔离构建生成纯 Python wheel，并安装到
`build-python-package`；从该目录导入封装、分别加载安装后的 Debug/Release 原生库，
各六项测试再次通过。SystemVue 自带 setuptools 在禁用隔离构建时缺少 `_distutils_hack`，
因此未使用该模式，也未修改其 site-packages。安装后的测试可用 `--installed` 开关，
并通过环境变量 PYTHONPATH 指向包安装目录。本次跨平台结果待 CI 核验。

## 噪声传播

`Library.passive_noise(scattering, temperature_k=290.)` 返回被动器件内生噪声协方差。
`Network.external_noise(ports, intrinsic)` 接受按全局端口排列的完整协方差，返回
按 ports 顺序排列的输出噪声矩阵，单位 W/Hz。可表达器件内及跨器件相关噪声。
不等长矩阵、维度错误及非半正定/非厄米矩阵会报错，函数不修改网络。

```python
with library.network() as network:
    s = [[0, 0.5], [0.5, 0]]
    network.add(s)
    intrinsic = library.passive_noise(s, 290.)
    noise = network.external_noise([0, 1], intrinsic)
    print(noise[1][1].real)  # k * 290 * 0.75 W/Hz
```

外部端口按匹配且无噪声处理，其他端口需已经连接或设置无独立源的终端。
终端噪声、源噪声以及噪声系数换算不自动加入；不能把内生输出噪声当作完整系统总噪声。
本版 Python 封装需要同时部署带两个新噪声符号的原生库。

JSON 工作流还支持逐器件 temperature_k、固定/逐频点协方差或显式 noiseless，
详见 [逐器件内生噪声](linear-model-file.md)。它允许混合有源和无源器件，
仍不自动推断放大器噪声模型。

## 参数化传输线

`Library.transmission_line(frequency_hz, *, characteristic_ohms, delay_s,
propagation_loss_db=0, reference_ohms=50)` 和 `Library.rlgc_line(frequency_hz, *,
length_m, resistance_ohms_per_m=0, inductance_h_per_m=0, conductance_s_per_m=0,
capacitance_f_per_m=0, reference_ohms=50)` 返回 2×2 复数元组。
数值实现仍位于 C++，Python 不另写传输线方程；参数须符合对应 C++ 模型的范围。
本版需要动态库包含两个新增模型求值符号，不能与更早的 ABI 1 构建混用。

## 小信号放大器

`Library.linear_amplifier(frequency_hz, *, gain_db=20, gain_phase_degrees=0,
reverse_isolation_db=50, reverse_phase_degrees=0, input_impedance_ohms=50,
output_impedance_ohms=50, reference_ohms=50)` 返回 2×2 复数 S 矩阵。
两个端口阻抗接受 Python complex；所有相位单位为度，两个阻抗的默认值独立于参考阻抗。
该接口直接调用 C++ 双向小信号模型，不含 NF、压缩和 AM/PM。
需要动态库提供新增符号 rfmodel_linear_amplifier_s。

非线性单向传输现可通过 cubic_amplifier 和 ideal_mixer 调用，接口约定及限制见
[频谱接口](spectrum-api.md)。JSON 线性网络格式不随此扩展为非线性求解器。

Library.loaded_noise 与 Library.thermal_boundary_noise 现支持已提取 N 端口网络的
外部失配及边界发射噪声，返回入射、出射和净功率结果，见 [失配噪声接口](loaded-noise.md)。
不会自动改变 JSON signal_boundaries 的噪声含义。

显式相干分量的多输入网络传播见 [多端口相干接口](coherent-network.md)，使用 Network.transmit_coherent 并保留调用者提供的分组。

Library.compress_coherent_fundamentals 提供多个相干组的共同基波压缩，
返回输入驱动和完整 CoherentReduction，见 [相干压缩接口](coherent-compression.md)。

共同限幅高阶放大器的局部导数及混合器件工作点接口见 [高阶工作点接口](highorder-linearization.md)，支持显式 a2..a11、生成频点选择、反馈与收敛噪声。

旧式输出功率 AM-PM 已提供相位求值、局部 A/B 组合及工作点求解的 C/Python/JSON 接口，见 [AM-PM 接口](am-pm.md)。早期章节的接口缺项和测试数属于其标注日期，当前阶段证据见 [剩余验收审计](acceptance-audit-20261012.md)。

## 内部混合参考网络

`Network.add_power_waves(scattering, reference_impedances_ohms)` 添加已经按每端口局部复参考表示的 S 矩阵；`Network.reference_impedances_ohms` 查询全局端口顺序的实际参考。原生 C ABI 执行物理接线、求解和相关噪声传播，Python 不复制求解算法。旧 `add` 的公共实参考行为不变；新 wheel 须加载含新增符号的原生库。局部信号/噪声坐标、线性 JSON 及不支持的频谱入口见 [功率波参考说明](power-wave-references.md)。


Monte Carlo 保留原 19 字段结果，新增 `Library.run_monte_carlo_extended` 返回含原 result 的独立扩展快照，并由 JSON v2 开放潜在相关矩阵与参数/指标联合统计；详见 [相关采样及任务格式](monte-carlo.md)。

`Network.reduce_power_waves(external_ports, reference_ohms=50)` 返回独立拥有句柄的普通 Network，可交给相干、频谱和来源传播接口；JSON 线性阶段也支持常量局部复参考，见 [RF 线性阶段](power-wave-stages.md)。


固定泵变频的逐通道复参考、联合 A/B/C/P 与边界重归一化已提供新增入口，保留既有标量接口；契约和限制见[变频功率波参考](conversion-power-wave.md)。
