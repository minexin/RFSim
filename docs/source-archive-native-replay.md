# 共享来源谱历史档案的原生开发重放

本路线使用同一批 23 份历史 shared-compression capture，恢复两个独立的原生评分入口。它不是新增 23 份采集，也不是新 SystemVue 仿真、完整 RFAMP 验收或两级级联。产品 Python、ctypes/C ABI 绑定、通用 JSON 分发均未恢复。

## 两个评分范围必须分开

- A：原 `compare-shared-compression.py.compare(provider, captures)`。21 个正常 case，21 项输入驱动加 32 项 direct carrier，共 53 项判定；carrier 各比较两个边界，共 64 次复幅度比较。历史为 49 通过、4 失败，`direct_carrier_passed=true`、`drive_power_passed=false`、`passed=false`。
- B：原 `compare-coherent-amplifier.py.compare(provider, captures)`。相同 21 个正常 case，206 项完整二/三阶来源谱，每项联合要求复幅度及功率误差通过；另有 21 个 group 等价关系判定。历史均通过。
- 两个相消 case 均为 `diagnostic_only`、`affects_compatibility_verdict=false`，不贡献通过数量。B 不把 RFPwrIn 计入 verdict；B 的通过不能覆盖 A 的四个驱动失败。

两份原 checker 及其 `compare` 完全不改，仍使用原数据校验、精确 origin 标签、阈值和 verdict。新 `replay_source_archives.py` 显式传入只记录请求的代理，代理直接转交 `NativeSourceReplay`；不替换 checker globals，不调用退役 Library。原 shared/coherent/cascade 三个 CLI 仍然失败，且不接受新的 `--native-probe` 参数。`reference_records.py`、`native_replay.py` 和冻结级联 checker 均保持原字节。硬编码原始 LF hash 覆盖整个传递 scorer 依赖闭包，包括 `compare-amplifier-terms.py`、`compare-two-tone.py`、`compare-single-compression.py` 及其 `run-systemvue-reference.py` 数据审核器；冷进程测试逐个修改复制文件，确认启动前修改也被拒绝，不能把运行时 snapshot 当作原始冻结证据。

历史四个 drive 失败位于原数组第 7、9、12、16 份：−30/−30 dBm、噪声开启；对应无噪对照是第 18、19、20、22 份。每对观测 RFPwrIn 差约 1.6015544319643966e−11 W。此差额保留为观测，不拟合进模型，不改功率阈值。测试通过表示完整、正确地重放并报告这些已知差异，并不代表 vendor compatibility 为真。

## 有界 typed 映射

独立 `RFMODEL_SOURCE_ARCHIVE_REPLAY` v1 开发协议只支持：

1. `source_groups_v1` → `assign_source_coherence(vector<SourceCoherence>)`
2. `shared_fundamentals_v1` → `compress_coherent_fundamentals` 与 `SaturatingFundamentalCompression`
3. `cubic_sources_v1` → `CoherentLimitedAmplifier::evaluate`

每次请求最多两个 source-kind 输入；不接收 harmonic/intermod 输入、任意 JSON graph、stage 列表、二级失真数组或 `propagate_distortion=true`。组定义返回原顺序的 source/clock 标识与分组；共享压缩返回完整 reduced output、逐 bin 功率和总功率；cubic 返回 reduced inputs、完整 order/signed-indices/component terms、总驱动和 limited drive。现有 `RFMODEL_NATIVE_REPLAY` v1 的 source-only、至多两输入和 cascade 拒绝边界不变。

新传输复用未改 `native_replay.py` 的有界双管道进程 runner、manifest、请求记录、冷进程产品导入保护和原子报告发布。新操作白名单、magic、request/response decoder 与旧协议分离。协议包含版本、operation、request ID、build ID、状态和有界结构；限制请求/响应字节数、token 长度、组件/来源数、整数范围与有限数值；拒绝额外/缺失数据、错误频率/带宽/indices、重复或不完整 origin、stderr、超时及返回码不一致。错误不会被当成数值不匹配继续计分，也不会回退到动态库。

构建使用 classic locale、可往返的双精度文本。Windows probe 的 stdio 切至 binary，因此 CRLF 字节计数和 Ctrl-Z 都按实际字节处理；这仅改变本进程的 stdio 模式。源定义顺序、数字 group 和输出 identity 的验证属于协议/映射一致性，不是新的 vendor 覆盖。

## 名义输入、单位和来源等价类

归档固定 Source1=1 GHz；Source2 同频或 1.1 GHz，grid=100 MHz，bin=10/11。源名义输入为 `sqrt(10^((P_dBm−30)/10)) · exp(jφ)`，单位 sqrt(W)。不能拿实测 RFPwrIn 重新校准。G=20 dB、OP1dB=20 dBm、OPSAT=23 dBm、输入 IIP2/IIP3=20/10 dBm、参考 50 Ω。

Source 的 BW 参数回读是 1 MHz，实际 CW direct 边界宽为 1 Hz；二/三阶 origin 宽度按因子数累加为 2/3 Hz。先核对平坦边界、`|V_peak|²/(2 Re Z)=P` 与 50 Ω 实阻抗，再用 `V_peak/sqrt(2 Re Z)` 得到功率波。不能把 V 当 RMS，不能把两个边界功率相加或积分整个 P 数组。

分组表示 source/reference-clock 的关系；实际相量约简还要求 bin、kind、bandwidth 一致。独立时钟同频 180°不会相消；锁定同频 0°/90°按相量合并。归档矩阵不是笛卡尔积：唯一异频 case 为独立时钟，没有锁定异频的 vendor case，不能把这里的“locked 时四项”扩成一般定律。

报告显式记录每一原始 source definition、clock、native group、名义功率/相位、实际送入 native 的复波、reduced-input key、参与 source IDs，以及当次观测 representative。原数组第 15 份为 −26/−6 dBm、0/90°、锁定同频：四个厂商 origin 均写 Source2，native reduced input 仍只有一个、indices 仍为 1。这是 Source1 与 Source2 等价类的观察 alias，不代表 Source1 的贡献消失，也不是“选择较强源”的推断规则。

附加 alias 校验使用报告 mapper 的 representative 重建完整观察表达式，并在原 capture 中查找精确 origin。测试保持第 15 份所有 capture 数值及 native 预测不变，仅把 mapper 改为 Source1，必须因来源标签找不到而失败。等价类的 reduced key/value 和成员还与实际 native input 及两份原始 source definition 交叉核验，错误 key、遗漏成员或错误 reduced wave 也不能通过。另一个独立负控删除较弱 Source1 输入，即使标签仍为 Source2，也必须由实测复幅度/功率失败检测到。两种错误不混为同一测试。

同频不同 origin 不能按 bin 折叠。报告保存 signed primitive 等价类因子的完整重数，例如 `−x+x+x` 保持三个因子，不约为 `x`；direct carrier 与同频 cubic 的 kind、bandwidth、完整 origin 各自独立。native 数字 group 是本次局部标签，历史比较只比较来源/测量身份和 group 等价关系，不把整数当 SourceN。

## 两类相消诊断

第 21 份无噪相消：F2/P2/ID2/V2/Z2 五向量全部缺失。它是没有输出测量，不是测得输出零。第 23 份开噪相消：五向量存在，但活跃内容只有 noise/total，无确定性 direct carrier。报告分别写为 `all_five_output_vectors_absent` 和 `spectrum_present_without_deterministic_carriers`，两者 `measured_zero=false`。

只有既存、带规范警告的无噪相消允许五向量全缺。部分缺失、普通 case 全缺、缺少规范警告或未知警告仍拒绝。空集合、只有诊断的集合、重复 case、遗漏或新增配置都不能获得 vacuous pass。

## 版本化报告与字节证据

新 report schema 为 `rfmodel.source-archives-replay`、版本 1，包含 recipe/scorer/mapping 版本。A/B 分别保存当前与历史完整报告，以及按 raw capture 身份和完整 measurement identity 连接的逐项 current/historical/delta。shared carrier 的完整身份还从实际活跃 IDName 中绑定厂商 `{group}` 前缀，而非猜测 native group。排序变化不制造身份变化；缺项、重复、kind/bin/BW/signed-indices/origin 漂移或观察值变化必须拒绝。每行保留 changed fields、数值差额和状态变化；21 项 partition 有自己的历史对照。历史 `rfmodel` 列只用于比较，不参与当前预测。

三份实际使用的入库文件保持原字节：

- shared-compression-captures：当前 LF SHA-256 `4a02f9213bf4a1324471e545dde8e8fd24c0ac2c078bb6c193799196ddcff2ec`
- shared-compression 历史报告：`73b743874ce6fbcf9f5a49bfdaa5cf4245c1ac80a74790f2bcb229df42c39978`
- coherent-amplifier 历史报告：`e4fd87f29dd8bf81c6de79f05a1c8820168a6894162bea53ee170484a13a2507`

两份历史报告声明同一个 capture hash `394b79ea44512b22b17585d7041474453bf80a3d50bff83afe1f8f6b6a6a6b6f`。只在内存把实际 LF 字节换为 CRLF，即得到此 hash；JSON 中已转义的脚本文本不受影响。这是可验证的换行关系，不是持有历史原始文件的证明。新重放不修改 capture 行尾或重写旧报告。

23 个 raw capture hash、一个共享 workspace hash、两份历史 library hash 均单列为 `historical_declared_unverified`。workspace 原 scope 为 `on_disk_source_copy_at_archive_time`，不是仿真时内存状态快照。档案中 1490 节点只保存 path/data/dimensions/timestamp/evaluation_error；可以检查部件名、受控脚本与 SI 回读，不能声称验证了原生 Model/Netlist、DataEntry 或数字 Unit IDs。

manifest 另含当前 executable/build-manifest 字节 hash、source revision/content/build ID、compiler/flags/configuration、实际请求/响应 hash、canonical request、全部本轮可读取档案和 scorer 字节 hash、实际命令行。源 revision 不能单独代表一个带未提交改动的构建；须结合 content hash。运行前后重新核对文件；任何状态变化都按实际结果保留，不回填历史失败或人为扩大阈值。

报告只能写入新路径，原子发布；不能覆盖 capture、历史报告、probe、manifest、脚本或任何已有文件。CLI 完整生成报告后返回 0；vendor 数值 verdict 在 A/B 的 summary 字段中独立表达。

## 开发运行与验证边界

```sh
cmake -S . -B build-source-Debug -DCMAKE_BUILD_TYPE=Debug \
  -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF \
  -DRFMODEL_BUILD_EXAMPLES=OFF -DRFMODEL_BUILD_REFERENCE_TESTS=ON
cmake --build build-source-Debug --target rfmodel_reference_source_probe \
  rfmodel_reference_source_oracle rfmodel_reference_highorder_oracle --parallel 2
ctest --test-dir build-source-Debug -R '^native_source_' \
  --output-on-failure --parallel 1
python -I -S scripts/reference/replay_source_archives.py \
  --native-probe build-source-Debug/rfmodel_reference_source_probe \
  --report build-source-Debug/source-archives-replay-v1.json
```

多配置生成器需选择 Debug/Release 子目录，Windows 可执行文件带 `.exe`。Python/scorer/CMake 内容纳入构建 manifest；修改这些文件后要重新构建 probe，使编入的 build ID 与 manifest 一致。

`test_source_archive_replay.py` 覆盖所有真实 capture、53/206/21 计数、完整逐身份历史比较、四个噪声对照、两类诊断、Source2 alias 与弱源删除、来源漏项/额外/重复/kind/BW/group/相位/零幅度、V/P/Z 和时间戳/参数/shape/非有限数、联合功率阈值、只读旧 CLI、原子覆盖保护及冷进程 `-I -S`。冷启动在没有产品树的复制目录中主动禁止 `rfmodel`、`ctypes`、`ctypes.util` 和 `_ctypes` 导入。协议与独立数学 oracle 负控另在 `test_source_replay_protocol.py`。

安装 gate 从关闭 BUILD_TESTING/C_API/CLI/EXAMPLES/PYTHON_TESTS/REFERENCE_TESTS 的纯 C++ SDK 构建开始，检查未发现 Python 或 C 编译器。安装后重定位；独立 consumer 只通过 `find_package(RFModel CONFIG REQUIRED)` 和 `RFModel::rfmodel` 使用安装 headers。probe 与真正第二 TU 链入同一 executable，不能用两个单 TU executable 冒充 ODR 验证。安装 SDK 不应含 probe、oracle、Python、manifest 或实验素材。相同 archive/transport 测试再用于该安装 probe。

数学 oracle、typed API 交叉检查和 vendor archive 三者分别解释：独立公式和 ordered signed Fourier 卷积是数学 oracle；第二次调用同一 kernel 是传输/API 一致性；vendor 观察值才是本次历史比较依据。Debug、Release、installed SDK、全 native 回归和远程平台 CI 必须分别报告。未运行的 MSVC/macOS、未持有的 raw/workspace 或新 vendor 仿真不能由本地通过推论。

本批明确不含两级 cascade、谱约简缺省假设的物理验收、五阶 secondary、unity-gain 拟合、噪声注入、完整 NodeTotal、反向驱动或多端口完整模型。

### 初次 Debug 历史差异观察

初次 Linux/GCC Debug 原生重放得到 A 的 53=49 通过/4 失败，B 的 206 项与 21 个 partition 全过，64 次边界比较及两个排除诊断完整；所有 pass/fail 状态与历史相同。A 的逐项数值无差异；B 有 10 行数值差异，位于原数组第 5、6、15、17 份。涉及 3 行预测复幅度、5 行预测功率、8 行复幅度误差与 5 行功率误差（同一行可有多个字段变化）。

最大预测复幅度绝对差为 2.0816681711721685e−17 sqrt(W)，最大相对差为 9.174580513195498e−16；最大预测功率绝对差为 1.0408340855860843e−17 W，最大相对差为 1.8195895936463925e−15。复幅度误差字段和功率误差字段的最大绝对变化分别为 9.174580755943831e−16、1.7763568394002505e−15。第 15/17 份 direct carrier 的两个幅度分量逐值相同而导出功率不同，可把这些差异定位在后续浮点模长/功率计算；仅第 5 份的三个 native 幅度本身有约舍入量级变化。旧 library/compiler/runtime 无原始字节，因此“与浮点舍入一致”是有界解释，不能宣称已证明具体历史平台原因或逐位相等。所有差异完整保留，不修改 1e−7 阈值。这一初次 Debug 观察不能代替最终 Debug/Release/安装 SDK 结果。
