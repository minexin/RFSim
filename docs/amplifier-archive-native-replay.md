# 单音 / 双音历史档案的原生开发重放

本路线只恢复两类已退役 Python 数值入口的开发评分能力：三个单音 capture 和三个等功率双音 capture，共 42 项原有判定。它不是新 SystemVue 仿真，也不是完整 RFAMP、来源共享压缩或级联验收。产品 Python、ctypes/C ABI 绑定、通用 JSON 模型分发均未恢复。

## 范围和映射

固定旧绑定基线为 `e47ea901a2ded2528f7e26502d8635f4d599ab24`。其 `python/rfmodel/__init__.py` 中 `p1db_fundamental` 分别使用有 / 无总驱动的 C ABI；`intercept_amplifier` 使用正二次、负三次截点模型。当前开发 probe 直接调用相同的两个类型化接口：

- `P1dBFundamentalCompression::transmit_fundamental(incident)`，或显式 `total_incident_power_w` overload
- `MatchedPolynomialAmplifier::from_intercepts(...).transmit(PowerWaveSpectrum)`

独立 `RFMODEL_AMPLIFIER_REPLAY` 版本 1 协议只有 `p1db_fundamental` 和 `intercept_spectrum` 两种操作。它不是现有高阶协议的扩展或任意模型代理。单请求最大 65,536 字节、单 token 最大 128 字节；输入谱为 1–2 个唯一正频 bin，bin 不超过 `INT32_MAX/3`。完整响应有版本、操作、请求 ID、构建 ID、状态及有界有序数据，错误不回显输入。stdio 仅输出协议记录。Windows 下 probe 将本进程 stdin/stdout 切为 binary，保留原始 CRLF 字节计数并使 Ctrl-Z 不能充当文本 EOF；模式切换失败明确报错退出，不改系统设置。Python 开发传输复用成熟的 `native_replay.py` 超时和有界双管道 runner，没有库查找或静默回退。

原 checker 的 `compare(library, ...)` 原已支持显式 provider 注入；新独立入口直接传入 typed native provider。两个原 checker 文件保持逐字节不变，包括数据检查、1e-7 阈值、结果字段和限幅诊断。其整个文件同时受既有 RFAMP OP1dB pilot 的冻结源码合同约束，不能为新增 CLI 改写该合同。两个旧 CLI 仍明确报产品绑定退役；新聚合重放入口的 `--native-probe` 是本路线的唯一 opt-in。

## 单位和判定

单音按 Source 名义 dBm 转 W、再取 sqrt(W) 作为功率波，不能用实测输入 DCP 重新校准。保留 gain 和 output_w 两项，对照 CGAIN[1] 和 DCP[1]。RISO、NF、端接等仍作为归档参数核对条件；这不意味着标量压缩模型预测了反向反馈或噪声。

双音 spacing 为 100 MHz，bins 10/11，零相位。G=20 dB，IIP2/IIP3=20/10 dBm；截点不是输出截点。每个直接基波以总驱动 **2P** 压缩。实际 CW 两边界间隔为 1 Hz，n 阶产物为 n Hz；源设置中的 1 MHz BW 不是这些 CW 谱的实际宽度。按完整来源表达式和精确边界选一个平坦功率样本，不积分两边界，也不把同频三阶重叠项计入直接基波。

历史结果是 42 项中 22 通过、20 失败：三个单音全过；双音 case 状态为 true/false/false，后两组各十个多项式产物失败，六个直接基波全过。`per_tone`/`total_rf` 限幅假设仍是 `affects_compatibility_verdict=false` 的诊断，绝不替换名义判定。测试 gate 通过代表完整重放并检验预期事实；报告的 vendor_passed 仍可能为 false。

报告同时保存本次及历史的全部 42 项原判定和逐项差异。历史预测列仅用于对照，不作为当前模型输出或独立数值 oracle。若本次 native 真实变化，版本化报告如实追加差异；不硬编码旧失败、不放宽容差、不覆盖旧报告。

## 字节与 provenance

八个原始入库文件（四个 capture 文件、四个历史报告）保持原字节，并受 `.gitattributes -text` 保护。报告分别记录：

- 当前可读取的 Git/LF 文件 SHA-256
- 三个单音历史报告的 `capture_sha256`，它们是缺失原始 raw 文件的声明，不能冒充精简 capture 文件 hash
- 双音历史 `captures_sha256`，等于当前 LF 字节在内存转换为 CRLF 后的 SHA-256；原文件不被转换
- 六个 capture 中历史 raw hash 声明；未持有的原始 raw / workspace 不标为验证成功
- 实际 probe executable、build manifest、public headers、传输和 scorer 文件 hashes；编译器、flags、configuration、source revision、每次请求/响应 hash 与完整请求、实际命令行

构建 ID 对完整 manifest envelope 内容取 SHA-256 并编入可执行文件；provider 核对 manifest、响应 ID 和执行期文件未变。source revision 与源文件内容 hash 一起辨识未提交开发构建，不能把只含 base SHA 的临时构建称为该提交的干净产物。

## 开发命令和安装隔离

```sh
cmake -S . -B build-reference -DCMAKE_BUILD_TYPE=Debug \
  -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF \
  -DRFMODEL_BUILD_EXAMPLES=OFF -DRFMODEL_BUILD_REFERENCE_TESTS=ON
cmake --build build-reference --target rfmodel_reference_amplifier_probe \
  rfmodel_reference_amplifier_oracle rfmodel_reference_highorder_oracle --parallel 2
ctest --test-dir build-reference -R '^native_(compare_single_compression|compare_two_tone|amplifier_)' \
  --output-on-failure --parallel 1
python -I -S scripts/reference/replay_amplifier_archives.py \
  --native-probe build-reference/rfmodel_reference_amplifier_probe \
  --report build-reference/amplifier-archives-replay-v1.json
```

多配置生成器需选择对应 Debug/Release 子目录，Windows 可执行文件带 `.exe`。两个旧 checker CLI 继续退役；这里新增的是独立原生归档重放入口。聚合重放写完整报告后 exit 0，数值差异和 vendor 状态在报告内另列。所有 native 输出均必须为新路径，原子发布且不能覆盖任何已有输出或输入。

新增 installed gate 先关闭测试、C ABI、CLI、examples 构建纯 C++ SDK，确认未探测 Python 或 C 编译器，安装后重定位；把 probe 和真正第二 TU 复制到独立 consumer，通过 `find_package(RFModel CONFIG REQUIRED)` 仅链接安装后的 `RFModel::rfmodel`。无源 include 回退，不安装 probe、脚本、oracle 或 manifest。独立 oracle 不包含 SDK，按 P1dB 标量公式及有符号 Fourier 元组卷积计算含相位 / DC / 频率碰撞的锚点。

验证还覆盖协议故障、捕获缺项 / 重复 / 参数 / 新鲜度 / 边界错误、主动阻止产品导入、CLI retired-default 和文件覆盖保护、数值负控。Debug/Release 和远程多平台的结果必须分别绑定实际提交与构建，不能用本地 focused gate 替代远程 CI 或完整数值核心 full gates。
