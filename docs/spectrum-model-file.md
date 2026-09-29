# 匹配单向频谱链路 JSON v1

`rfmodel.spectrum-chain` 保存公共频率网格、输入功率波及按顺序执行的器件。
它与线性网络文件分开，避免把含频率转换的处理误当作单频 S 参数网络。
所有器件传输均调用 C++ 核心；Python 只做解析、校验和顺序编排。

```powershell
python -m rfmodel examples/two-tone-mixer.json `
  --library build-msvc/Release/rfmodel_c.dll `
  --output build-reference/two-tone-mixer-result.json
```

需安装当前 Python 包或将仓库 python 目录加入 PYTHONPATH，输出目录须已存在。
执行规则与线性 CLI 相同：所有计算和 JSON 序列化成功后才写文件，失败返回 1。

## 字段与单位

顶层必填 format（固定 `rfmodel.spectrum-chain`）、version（整数 1）、spacing_hz、
input、stages；可选 reference_ohms，默认 50。频率间隔及实参考阻抗均为正有限数。
input 是最多 2048 项的数组，每项含 bin 和 amplitude：bin 为 0 到 2147483647
的整数，amplitude 为实数或 `[实部, 虚部]`，单位 sqrt(W)。bin=0 时幅度必须为实数。
同一 bin 不得重复；空输入允许。实际频率为 bin*spacing_hz，负频率由核心共轭补全。

stages 按数组顺序执行，数量 1–512，每级 id 为唯一非空字符串。支持：

| type | 必填参数 | 可选参数 |
|---|---|---|
| cubic_amplifier | power_gain_db、input_ip3_dbm | 无 |
| ideal_mixer | lo_bin（正整数） | conversion_gain_db=0、lo_phase_radians=0 |

全部级共用频率间隔及参考阻抗，不自动插值、滤波或改变参考阻抗。未知字段、未知器件、
重复键、重复 ID、非有限数以及错误参数均拒绝处理。没有脚本表达式执行。

## 输出与示例

结果 format 为 `rfmodel.spectrum-results`，包含 version、spacing_hz、reference_ohms、
规范化后的 input 及 stages。每一级保存 id、type、spectrum；每条谱线包含 bin、
frequency_hz、amplitude（实虚数组）、power_w。按 bin 升序输出，零项可能省略。
不把不同来源但落在同一 bin 的功率单独相加：相干复数波叠加由核心完成。

示例每个输入单音为 1e-5 W，频率 10 和 13 MHz；20 dB、IIP3=10 dBm 的三阶放大器
产生 7 MHz IM3，功率 1e-9 W。8 MHz LO 将它折叠至 1 MHz；两级完整输出频谱均保留。
这既可作为脚本批处理输入，也可用于后续 SystemVue 比对的 RFModel 侧重放。

## 范围与验证

仅支持匹配单向链路：没有失配反馈、非线性网络连接、谐波平衡、变频噪声、LO 端口
加载和多阶 LO 杂散。三阶模型不能替代深压缩 PA 饱和模型；理想实混频器保留上下边带，
不会自动选取 IF。限制继承 [频谱接口](spectrum-api.md)。完整非线性网表仍待实现。

回归覆盖逐级 IM3 和差频功率、重复输入频点、非法 DC、重复器件 ID、未知参数、
版本和频率网格校验，以及命令行结果生成。SystemVue 仿真对照尚未执行。

2026-09-29 验证：Debug/Release 动态库上的 Python 专项各二十项通过；生成并安装
wheel 到项目内独立目录后，使用安装后的 Release 动态库再次通过二十项测试。
本轮未修改 C++ 数值核心，没有重复全套 C++ 回归；跨平台结果尚未核验。
