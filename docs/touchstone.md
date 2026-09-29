# Touchstone 数据接入

读取 legacy `.sNp` 全矩阵 S 参数，RI/MA/DB 转换，Hz/kHz/MHz/GHz，行内注释、前导空白、矩阵续行及尾部缺省选项。统一参考电阻保存在 `reference_impedance_ohms`，不隐式重归一化。

依据 [IBIS Touchstone 规范](https://www.ibis.org/touchstone_ver2.1/touchstone_ver2_1.pdf) 对旧版数据约定的说明，二端口 S11/S21/S12/S22 转为内部行优先存储，其余端口数使用行优先。

输入必须有选项行、有限数值、正参考电阻和非负严格递增频率。不支持的参数类型/单位、重复头、残缺记录、非法扩展名及版本 2 关键字明确报错。读取器资源上限为 1024 端口，并非格式标准上限。

测试包含非互易二端口传输方向、三端口续行顺序、75 欧姆参考、缺省 GHz/MA，以及 14 类非法输入；MSVC Debug/Release CTest 各 4/4 通过。

尚未支持：Touchstone 2 元数据、逐端口参考阻抗。尚未执行 SystemVue 导出数据的差异验证；不宣称完整 Touchstone 兼容。

## Legacy 二端口噪声段

依据上述 IBIS 规范的 Noise Parameter Data 一节，二端口网络数据之后可读取每行五列的噪声记录。首个噪声频率不大于最后一个网络频率时切换到噪声段；噪声段自身频率必须严格递增，之后不能恢复网络数据。噪声频点与 S 频点可以不同。

`TouchstoneData::noise_samples` 保存 Hz 频率、NFmin（dB）、复数 GammaOpt 和 Rn（欧姆）。GammaOpt 始终按幅度/角度读取，不受 S 的 RI/MA/DB 选项影响；legacy 归一化 Rn 乘选项行的参考电阻还原成欧姆。噪声记录不接受续行。

读取器校验语法、有限性和非负幅度/电阻，但保留原始 NFmin 和 GammaOpt 数值，不把物理不一致数据强制修正。`TabulatedNoiseModel::from_touchstone(name,path,policy,T0)` 进一步插值各噪声频点的 S，调用 noise_from_parameters 检查物理约束并生成相关矩阵。默认噪声频点超出 S 范围时报错，只有显式 Clamp 才使用 S 端点。T0 默认 290 K，不代表器件物理温度。

无噪声段的文件仍能作为普通 S 数据读取；构建带噪模型则报错。CLI 仍只导出 S 数据。新增回归覆盖单位、75 欧姆 Rn 还原、RI 选项下的噪声角度、相等频率段切换、原始参数到器件模型及错误记录。

## C/Python 模型接口

新增 C 句柄 rfmodel_touchstone，open 接受 UTF-8 文件路径及越界策略
（0=Reject，1=Clamp），close 释放。get_info 返回端口数、原始参考电阻、频率上下限
及内嵌噪声样本数；s 接受频率和目标正实参考电阻，写入行优先复数 S 矩阵。
短缓冲区、非法参数、越界或解析失败明确返回状态码；s 失败保持输出不变。
open 失败将输出句柄设为 NULL；close(NULL) 安全，其他句柄操作必须在 close 前完成。

```python
with library.touchstone("measurements/device.s2p", out_of_band="reject") as model:
    print(model.info)
    scattering = model.s_parameters(2e9, reference_ohms=50.)
```

Python reference_ohms 省略时使用文件参考电阻。先在原始参考下对实部/虚部线性插值，
随后使用核心 renormalize_s 转换参考，不插值幅相，也不做被动性修正。
默认拒绝频带外查询；显式 clamp 才使用端点，负数或非有限频率始终拒绝。
模型在打开时读取并持有数据快照，之后文件删除或变更不影响已打开的模型。
Python 句柄支持 with、显式 close 及回收释放，关闭后访问报错。
核心文件读取现在以 UTF-8 路径构造 C++17 filesystem 路径，支持 Windows 中文路径。

这是 S 参数接口，内嵌噪声只通过 info.noise_sample_count 提示存在。
没有通过该接口导入 NFmin/GammaOpt/Rn 或生成噪声协方差；需要带噪分析时必须
另行指定噪声模型。Touchstone 2、逐端口参考阻抗等原有边界仍然存在。

2026-09-29 接口验证：MSVC Debug/Release 全套各 43/43，安装后的独立 C/C++ 消费者
各 2/2；Python 36 项测试在安装后的 Release 动态库上通过。新增用例覆盖中文路径、
非互易复数插值、文件快照寿命、端点钳制、75→50 Ω 转换、内嵌噪声提示、非法格式、
JSON 相对路径、混频后频率查询及输入防覆盖。CLI 合成数据示例也已生成结果。
此次未新增 SystemVue 导出数据对照，跨平台 CI 结果尚未核验。
