# 独立 RFAMP 压缩参考

2026-09-29 使用 SystemVue 2023 自带 `RF Design Kit/Amplifiers/Amplifier Compression.wsv`
的项目内副本 `build-reference/RFModel_AmplifierCompression.wsv`。原厂文件未修改、
未提交。副本初始 SHA256 为 `9175db32246e0e25c46f42377fc7254f385203fe30e38ba420ceb5dd45171f85`。
原示例只有 Source、RFAmp、Out 三个器件，与此前四级天线链路独立。

## 受控条件与调用

新增后台采集 case `compression`。每次分析显式设置 G=20 dB、NF=3 dB、
RISO=100 dB、放大器输入输出和源/负载均为 50 ohm。保留示例 OP1dB=20 dBm、
OPSAT=23 dBm、OIP2=40 dBm、OIP3=30 dBm；源频率 1 GHz、单音、相位噪声关闭。
比较脚本回读核验上述参数及源功率，共 16 项，拒绝默认空值、参数差异、
额外器件、陈旧数据和任何 manager 错误/告警。其他内部模型默认值尚未完整审计。

```powershell
& 'C:/Program Files/Keysight/SystemVue2023/Python/python/python.exe' `
  scripts/reference/run-systemvue-reference.py compression `
  build-reference/RFModel_AmplifierCompression.wsv `
  build-reference/compression-plus09-next --source-power-dbm 0.9
```

输出目录必须是新目录。已有实例需只打开这个专用工作区；首次加载到空实例
可用 `--open-copy`。此 case 使用单次 System1 分析，不读取文件自带的旧 Sweep1 数据。

## 实测结果与边界

+1 dBm 是名义输入 P1dB。首次运行触发厂商“已超过输入压缩点、测量精度降低”
警告，采集器按原规则失败；未将该次结果用于通过声明，也没有放宽告警策略。
原日志保存在 `build-reference/compression-plus1-001/stderr.log`。

改用 +0.9 dBm 后获得无告警新结果，输出 0.098267324805 W。原生 C++ 经 C ABI
预测 0.098267327169 W。固定相对阈值 1e-7 下，增益误差 −9.4571e-10，输出功率
误差 +2.40543e-8，2/2 通过。证据为：

- `validation/systemvue-2023-single-compression-plus09.json`：参数、时间、比较和原始采集/DLL散列。
- `validation/systemvue-2023-single-compression-plus09-capture.json`：去掉脚本方法清单和无关数据的可重放采集，保留原始采集散列。

用 `compare-single-compression.py LIBRARY CAPTURE OUTPUT --source-power-dbm 0.9`
可重新验证原始或精简采集。报告 capture_sha256 对应实际传入文件；精简采集中的
raw_capture_sha256 对应原始文件，两者不可混同。比较只涉及基波增益和功率。

该单点支持独立器件的 P1dB 以下公式，但不能消除四级链路已有的 1–2 ppm 差异。
两实验的增益、输出标定功率、反向隔离和其他参数不同，尚不能唯一归因。
下一步应在独立拓扑中复现天线末级参数，再单独改变反向隔离等条件。

## 会话保留与回归

切换前运行 `preserve-close-antenna-reference.ps1`，仅接受唯一的
RFModel_AntennaNoise 工作区，先另存新项目内备份，确认文件存在且非空后关闭。
本次备份为 `build-reference/RFModel_AntennaBackup-before-compression.wsv`，SHA256
`9d959e479382d8555ffe6d56de5928f0a2cd0f50de8191aadd6fcfd860e075c5`。
当前参考实例保留独立放大器工作区（源 +0.9 dBm），没有关闭 SystemVue。

新增 CTest `single_compression_reference` 两项测试，涵盖实测重放和告警/参数/
时间/拓扑拒绝；本机 Debug/Release 均通过。原生链路比较五项测试、后台驱动
三项测试也通过。现有跨平台 CI 会运行新增 CTest，此提交的远端结果尚未核验。

## 反向隔离单变量对照

随后新增 `--compression-riso-db 50|100` 采集参数（仅 compression case），以及
比较脚本的 `--reverse-isolation-db 50|100`。默认仍为 100；比较必须显式声明
50 才接受相应采集，防止错用基线参数。PowerShell 入口同样限制取值和用例。

在 +0.9 dBm 源功率下只将 RISO 从 100 改为 50 dB，其余 15 项已核验参数
完全相同。50 dB 用例输出 0.0982673248103 W，相对基线变化约 +5.4233e-11；
增益也仅变化约 +5.4233e-11。对 RFModel 的增益残差约 −9.9994e-10、输出功率
残差约 +2.40001e-8，2/2 通过固定 1e-7 阈值。

该结果排除了“在本独立用例条件下，只改变 RISO 就会产生原链路 ppm 级误差”
这一解释；不能排除更高增益、反馈拓扑或参数组合的影响。还需对齐天线末级
30 dB 增益、60 dBm OP1dB 等参数，再验证同一相对压缩水平。

报告和可重放精简采集分别为 `validation/systemvue-2023-single-compression-plus09-riso50.json`
与 `validation/systemvue-2023-single-compression-plus09-riso50-capture.json`。
恢复 100 dB 后的原始采集位于 `build-reference/compression-plus09-riso100-restored-001`，
CF、CGAIN、DCP 共六个数值与原基线完全一致。当前工作区仍为独立放大器，
源 +0.9 dBm、RISO=100 dB。本次没有改动数值核心。

新增回归验证两组实测的差异范围、只有 RISO 改变，以及未声明 50 dB 时拒绝
错误参数。单器件测试现共三项，MSVC Debug/Release 均通过；后台驱动三项也通过。
