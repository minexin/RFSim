# 两路 PLL：显式相关相噪和有限带宽 jitter（C++17）

这是可直接复制到宿主软件的双翻译单元示例，不新增求解器或噪声算法。
`pll_noise.hpp` 的 owning `Result` 保存每个 offset 的两路 PLL transfer/budget、
完整 6×6 C/P 和两路单输出 jitter；`main.cpp` 仅选择输入 grid、调用和打印。
所有运行时 oracle 用异常检查，Release/NDEBUG 下也生效。

## 仅使用已安装 SDK 构建

先用包含 locked PI PLL、complex phase response 和 phase jitter 接口的版本安装 SDK：

```sh
cmake -S . -B build-header-sdk -DBUILD_TESTING=OFF \
  -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF \
  -DRFMODEL_BUILD_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX=/absolute/path/sdk
cmake --install build-header-sdk
cmake -S examples/pll-phase-jitter-sdk -B build-pll-example \
  -DCMAKE_PREFIX_PATH=/absolute/path/sdk -DCMAKE_BUILD_TYPE=Release
cmake --build build-pll-example --config Release
ctest --test-dir build-pll-example -C Release --output-on-failure
```

也可把本目录单独复制到 SDK 外任何位置再配置。CMake 只使用
`find_package(RFModel 0.1.0 EXACT CONFIG REQUIRED)` 和 `RFModel::rfmodel`，
没有源码 include fallback、Python 产品接口或额外第三方运行库。
Windows/MSVC 同样使用 CMake 3.20+、C++17；多配置生成器运行
`build-pll-example/Release/pll_phase_jitter.exe`。
版本号相同不代表旧 SDK 已含这三个新增接口，请使用对应新版本的完整安装。

```sh
./build-pll-example/pll_phase_jitter
./build-pll-example/pll_phase_jitter --equal-loops
```

程序只写 stdout/stderr，不创建或覆盖任何文件。默认打印固定 81 点复 H（rad/rad）、budget CSV
片段及两路 jitter 汇总；两个配置和非法参数均有 CTest。完整矩阵保留在
`Result::offsets[i].incident_noise`，可由宿主自行选择格式导出。

## 真实 API 链和显式物理假设

1. 用户明确指定两路输出载波均为 **100 MHz**、反馈分频 N=10、共同参考为
   10 MHz。载波是示例声明，不由锁定模型自动推断；loop 参数改变不改变该声明。
2. 对每个严格正 offset 调用 `analyze_locked_pi_pll`。Kpd 为 A/rad、Kvco 为
   rad/s/V、R 为 Ω、C 为 F；不得把 Hz/V 直接当 rad/s/V。
   输入一侧 reference PSD=2e-14 rad²/Hz 和各路 VCO PSD=5e-13 rad²/Hz。
   其它噪声项显式置零。默认第二路 R/C 不同，`--equal-loops` 使用相同参数。
3. `with_phase_response_noise` 接收三个用户显式定义的独立 group：一个包含两路
   `H_reference(+f)` 的 common reference，以及各自独立的 `H_vco(+f)`。
   函数内部使用 `phase_response_group` 生成完整相关 C/P，通过
   `additional_source_noise` 加入请求。不会从 total PSD 反推共同源，标签本身
   不是持久过程身份，也不意味着自动识别了 physical clock。仅同组成员共享源。
4. `analyze_conversion_simulation` 在两只互不耦合的 50 Ω matched load 上传播。
   `loaded_noise=true` 是取得 incident C/P 的必要设置。PLL 输出定义在 incident
   source 波；outgoing 是零。载波 wave A=0.1、B=0.06+j0.08，单位 sqrt(W)，
   两者功率均 0.01 W。这是纯 PM、无 AM 的最小显式网络。
5. `measure_phase_jitter` 直接接收各路 `noise.total_rad2_per_hz`。C/P 是 **W/Hz**，
   不是 rad²/Hz，不能直接送入 jitter。仅在本例已声明的纯 PM/matched 条件下，
   以 `C_upper,upper=|a|² S_phi/2` 作交叉核对；这不是通用相噪提取算法。

## 独立 oracle 和积分合同

每个正 offset 以三个独立 proper latent source 构建解析 Fourier 系数：
upper 为 `j*a*H*sqrt(S/2)`，lower 为 `j*a*conj(H)*sqrt(S/2)`。
独立构造 `C=UUᴴ+VVᴴ`、`P=UVᵀ+VUᵀ`，检查所有 36 个 C 和 P 元素，
包括 lower conjugation 和 upper/lower complementary correlation。
矩阵比较以该矩阵最大元素（W/Hz）为零元素的舍入误差尺度，容差 2e-11；
避免把 roundoff 产生的约 1e-30 W/Hz 当作真实额外噪声。
两输出相位 cross PSD 为 `H_A*conj(H_B)*S_reference`：

- 相同 H 时，差分 common 项 `|H_A-H_B|²*S_reference` 精确为零。
- 不同环路的固定 10 kHz fixture 独立核对 cross PSD 为
  `(2.0373096846266146e-12 + j*3.911269270949205e-15) rad²/Hz`，
  实虚部分别检查符号和相对误差，不能只保留 |H|。
- 两路独立 VCO 差分贡献相加，始终非零，不因 common reference 而抵消。

这些差分值仅为逐 offset 的**示例解析 oracle**；没有差分 jitter 产品 API，
也没有积分差分 jitter。实际测量是每一路单输出有限带宽 RMS phase/time jitter，
不是 period jitter、cycle-to-cycle jitter、TIE 序列或无限带宽 jitter。

`simulate(offsets_hz, equal_loops)` 接收用户指定、严格递增的正整数 Hz grid；
1 Hz conversion spacing 使 carrier±offset 可精确表示，offset 必须小于 carrier。
默认 100 Hz 到 1 MHz，81 个近似 log-spaced 样点（四舍五入到整数 Hz），无 DC。
有限积分边界显式取首尾样点，策略显式为 `linear_rad2_per_hz`：在**线性 PSD**
上插值，梯形面积按 **Hz 宽度**加权，而不是 log(Hz)、dB 或 dBc/Hz。
独立 oracle 用 `|H_ref|²*S_ref+|H_vco|²*S_vco` 重算每段面积，核对 phase variance、
其平方根和 `rms_phase/(2*pi*carrier_hz)` 秒数。

离散积分只对所提供的分段线性谱精确，不声称等于连续 PLL 谱的解析积分。
采样密度、共振附近加密和收敛检查由调用者负责；改变 grid 后应评估积分变化。
不要自动外推到 DC、无穷频率或样本覆盖以外。此 PLL 是 already-locked、
linearized、continuous-time、zero-delay PI 模型；不包含捕获、非线性 PFD/CP
（PNCP）、fractional-N spur、厂商器件兼容承诺或 SystemVue 等价认证。

## 持续回归入口

主项目启用 BUILD_TESTING 后注册 `pll_phase_jitter_sdk_installed`。
现有跨平台 Debug/Release CI 的顶层 CTest 会执行它：独立 pure C++ header 安装、
重定位 SDK、把示例复制到独立目录，再构建并运行本目录全部三个 CTest。
可只运行此 focused gate（不要求构建其余测试）：

```sh
cmake -S . -B build-focused -DBUILD_TESTING=ON -DRFMODEL_BUILD_C_API=OFF \
  -DRFMODEL_BUILD_CLI=OFF -DRFMODEL_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release
ctest --test-dir build-focused -C Release -R '^pll_phase_jitter_sdk_installed$' --output-on-failure
```
