# 有界参数优化：C++、C ABI 与 Python 契约

A09 的第一项原生实现提供 `include/rfmodel/optimization.hpp` 中的 `minimize_bounded(parameters, objective, options, cancelled)`。目标是最小化调用者给定的一个有限实标量；可用它调用 RFModel 网络、噪声或器件原语并组合设计误差。现已提供 C ABI 和 Python 同步回调入口；现已增加参数绑定和通用 JSON 优化任务；另已提供[原生 Monte Carlo](monte-carlo.md)，已接入 C/Python 和声明式任务，SystemVue 对照尚未完成。

## 参数及约束

参数向量按给定顺序传入目标函数，最多 64 项。每项 OptimizationParameter 包含 lower、upper、initial、kind，均使用实际物理值；优化器不会把初始点静默截断到边界。上下界相等表示固定参数，仍传入目标函数。

| kind | 契约 |
|---|---|
| Real | 有限实上下界，范围 upper-lower 也须有限；按范围归一到 [0,1] 搜索 |
| LogReal | 有限正上下界，在自然对数坐标归一到 [0,1]；目标函数仍接收实际正值 |
| Integer | 上下界及初值须为精确整数，绝对值不超过 2^53-1；每次传给目标函数的值也是整数 |

只实现盒形边界，不隐式加入线性/非线性约束、权重或惩罚函数。目标函数定义与量纲由调用者负责。所有参数、选项及目标函数是否存在，在首次评估前校验；无效请求抛 invalid_argument。

## 搜索和确定性

这是一个串行、确定性的完整坐标轮询：先评估初始点。每轮固定中心，按参数索引、正方向、负方向的次序访问候选；每个候选只改变一个参数，边界处裁剪到可行区。固定参数以及与中心完全相同的候选不调用目标函数。整轮选取严格改善目标值的最佳候选；相同目标值保持较早的最佳点。若未改善，则按 contraction 缩小归一化步长，直到 minimum_step。

Real 和 LogReal 的步长都相对于各自坐标范围；Integer 的实际步幅为 `max(1, ceil(step*(upper-lower)))`。整数候选独立地向正、负方向移动并裁剪，因此最小步幅为一。改善后的下一轮保留当前步长，没有自动扩张、多起点或随机扰动。候选不跨轮缓存，目标函数会看到重复评估点。

默认 initial_step=0.25、minimum_step=1e-7、contraction=0.5，分别要求 0<minimum_step≤initial_step≤1、0<contraction<1。max_evaluations 默认 10000，max_iterations 默认 2000，两者允许 1..100000。目标函数调用数包括异常和非有限返回，永不超出评估预算。iterations 表示开始过的轮询轮次，包含因目标达标、取消或评估预算而中途结束的一轮。

完整轮询和步长停止的一般概念可参见 [MathWorks 官方轮询说明](https://www.mathworks.com/help/gads/how-pattern-search-polling-works.html)。这里的顺序、尺度、整数行为和停止状态以上述 RFModel 契约为准；不据此声称 MATLAB 或 SystemVue 的算法等价。

同一二进制/浮点环境、参数、选项和确定性目标函数会产生可重放的评估顺序；record_history 不改变搜索。没有随机种子，因为此算法不调用随机数。不同编译器的浮点末位可能影响严格改善判定，跨平台以预定数值容差核对解析解，不承诺轨迹字节相同。带状态或随机性的目标函数须由调用者自行控制。

## 结果与停止原因

OptimizationResult 包含最佳已评估 parameters/value、has_finite_point、stop、evaluations、rejected_evaluations、iterations、final_step、last_failed_parameters、last_error，以及可选 history。没有有限点时 has_finite_point=false，value 为 NaN；parameters 保留初始点，不能当作求解成功。history 默认记录每次实际调用的参数、值、轮次、Finite/NonFinite/Exception 状态及错误消息，可用 record_history=false 关闭；失败诊断仍保留。

| stop | 含义 |
|---|---|
| target_reached | 最佳有限目标值 ≤ 调用者明确指定的 target；可在轮询中间停止 |
| step_tolerance_reached | 在 minimum_step 上完成一轮轮询，没有找到严格改善点 |
| all_parameters_fixed | 只需一次有限评估，没有可变参数 |
| evaluation_limit | 下一次评估会超出调用预算；保留目前最佳点 |
| iteration_limit | 已用完允许开始的轮询轮次；保留目前最佳点 |
| objective_failure | 默认策略遇到目标函数异常或非有限返回，立即停止 |
| no_finite_point | 步长/可表示候选/固定参数限制下仍没有找到有限目标值 |
| numerical_resolution | 候选或步长收缩无法在浮点中形成新值；不是最优性证明 |
| cancelled | 调用者的取消回调请求停止；可在首次目标函数调用前发生 |

步长或目标阈值停止不证明全局最优，也不证明非光滑或混合整数问题的全局可行性。特别是极窄浮点区间可能需要增大初始步长才能触及端点；NumericalResolution 独立报告这种情况。取消在评估前及轮询开始时检查，不中断正在执行的目标函数。

## 目标函数错误

默认 invalid_evaluation=Stop。非有限返回及标准/非标准 C++ 异常记录为失败评估，保留此前最佳有限点；std::bad_alloc 继续抛出，不伪装成可跳过的器件点。标准异常文本最多保存 512 字节。取消回调自身的异常同样直接传播。

显式指定 Reject 才能把无效候选排除并继续轮询；它不替换为任意大惩罚数。如果初始点无效，会围绕该点轮询，找到有限候选后才迁移中心。这不保证跨越整个无效区域去寻找另一可行区域。历史中的无效 value 可为 NaN/Infinity，消费方须先检查 status，不能将其当作有限目标值序列。

## C++ 使用

~~~cpp
#include <rfmodel/optimization.hpp>
rfmodel::OptimizationOptions options;
options.target = 1e-12;
auto result = rfmodel::minimize_bounded(
    {{0., 10., 1., rfmodel::OptimizationParameterKind::Integer},
     {1e-12, 1e-9, 1e-11, rfmodel::OptimizationParameterKind::LogReal}},
    [](const std::vector<double>& x) {
        return (x[0]-7.)*(x[0]-7.) + std::pow(std::log(x[1]/1e-10), 2);
    }, options);
// Inspect result.stop and result.has_finite_point before consuming result.value.
~~~

公共头文件随既有 CMake 安装导出；安装后的独立 C++ consumer 直接调用优化器。它不依赖共享库或 Windows API。

## RF LC 匹配设计示例

`examples/optimize_lc_match.cpp` 构建为 rfmodel_match_design，stdout 输出 `rfmodel.optimization-example` version=1 的设计输入、参数顺序、选项、目标值、停止原因和完整评估轨迹。这是固定可重跑案例的机读输出，不是通用优化任务输入格式。

网络在 1 GHz 使用输入端并联电容 C、随后串联电感 L，将 25 Ω 负载接到 50 Ω 输入参考。目标函数逐次使用 IdealRLCModel 与 LinearNetwork 组网，最小化 |S11|²。L 在 [0.1 nH,30 nH]、C 在 [0.1 pF,100 pF] 以对数尺度搜索，初始值 2 nH、10 pF，目标 |S11|²≤1e-14。

独立电路推导：串联支路阻抗为 25+jωL；令 ωL=25，得到其导纳 .02-j.02 S，再令 ωC=.02，输入导纳为 .02 S，即 50 Ω。因此解析值 L=25/ω≈3.978873577 nH、C=.02/ω≈3.183098862 pF。用实际搜索结果重建物理阻抗，与网络 S11 求解独立核对。

~~~sh
cmake --build build --config Release --target rfmodel_match_design
# Windows: build/Release/rfmodel_match_design.exe
# Linux:   build/rfmodel_match_design
~~~

测试另覆盖连续/对数/整数/固定参数混合的已知最优点、边界最优、非光滑目标、完整轮询选择、逐点评估重放、关闭历史后相同搜索、预算截断、异常/非有限值、显式拒绝策略、取消、数值分辨率以及无效参数不调用目标函数。

A09 仍需 Monte Carlo 分布/种子及统计参考，以及厂商优化与统计行为对照；已增加的声明式任务和系统参数绑定见下文，整项验收仍开放。

## 原生 C++ 阶段工程验证（2026-10-12）

[机读验收记录](../validation/optimization-native-validation-20261012.json)包含四组日志和源码哈希；[完整 RF 示例轨迹](../validation/optimization-lc-match-example-20261012.json)随源码归档。Windows MSVC、Ubuntu WSL2 GCC 的 Debug/Release 均 clean-first 构建，全量 CTest 各 134/134，安装后的 C/C++ 消费者各 2/2。160 个 C/C++ 文件格式检查通过。

四组 RF 示例均在 174 次评估、44 个已开始轮次内达到 target：L≈3.978873680 nH、C≈3.183098337 pF，|S11|²≈6.9625e-15。逐一用独立物理阻抗公式重算全部评估点，目标值最大绝对差约 2.34e-15；两种元件参数相对解析解误差均小于 1e-6。同一平台和配置的重复输出逐字节相同。

Linux 原生 /tmp 构建目录在完整测试和安装消费结束后的一次访问中已不存在。Release 示例因此在新原生临时目录单独重建，并在同一次进程中保存输出和重放；原全量测试日志保留，机读记录明确列出补采的配置、构建日志和可执行文件哈希。

原有八份未提交文件的哈希保持不变，完整 CTest 包含参考脚本草稿，不能宣称纯净检出验证。C ABI/Python 源码未改，本阶段未重新构建或单独安装 wheel；已有绑定套件包含在全量 CTest 中。未新增 SystemVue 分析、推送、macOS 或远程 CI 验证。

## C ABI 回调与结果所有权

`rfmodel_optimize_bounded` 复制参数和选项后调用同一个原生优化器。参数 kind 使用 RFMODEL_OPT_REAL、RFMODEL_OPT_LOG_REAL、RFMODEL_OPT_INTEGER；options=NULL 使用原生默认值。显式 options 必须填写全部字段；has_target、reject_invalid、record_history 只接受 0/1，未启用 target 时对应数值须为零。

objective 是同步 cdecl 回调，接收只在本次回调有效的只读物理参数、user_data、目标值输出和 513 字节错误缓冲区。返回零表示目标值已写入；NaN/Infinity 属于 NonFinite 评估。非零返回属于 Exception 评估，错误消息最多保留 512 字节，空消息使用默认诊断。回调不得把语言异常抛过 C 边界，可重入其他 RFModel 函数。取消回调返回零继续、正值取消、负值表示回调故障；故障是 API 错误而非优化器停止结果。

out 指向的句柄必须初始为 NULL，回调不得修改它。RFMODEL_OK 表示 API 执行成功；ObjectiveFailure、预算耗尽、取消等仍返回有效结果，调用者须另外检查 summary.stop 和 has_finite_point。参数校验/分配/取消钩子故障返回 API 错误并保留 out 原值。结果由调用者通过 `rfmodel_optimization_destroy` 释放，destroy(NULL) 安全。

结果是不可变快照。get_summary 提供参数数目、轨迹数目及错误字节数；get_parameters、get_last_failure、get_evaluation 复制结果，绝不重新调用目标函数。get_evaluation 可仅提供 info，其他输出全部 NULL/0 查询元数据；完整读取需要 parameter_count 个 double 和 error_bytes+1 个 char。get_last_failure 对无失败结果接受 NULL/0 参数缓冲区，但仍需一个字节写空消息。容量单位分别是元素数和字节数。输出缓冲区不得相互重叠或覆盖句柄；失败不修改输出，成功不修改容量中未使用的尾部。空历史不能查询 index=0。

## Python 调用

```python
from rfmodel import Library, OptimizationParameter
lib = Library('/path/to/rfmodel_c.dll')  # Linux: librfmodel_c.so
result = lib.minimize_bounded(
    [OptimizationParameter(0., 1., .5)],
    lambda point: (point[0] - .25)**2,
    target=0.)
assert result.stop == 'target_reached'
assert result.parameters == (.25,)
```

参数可用 OptimizationParameter 或只有 lower/upper/initial/可选 kind 的 dict；kind 为 real/log/integer。关键字选项与原生一致，invalid_evaluation 为 stop/reject，record_history 为 bool，cancelled 是可选返回 bool 的函数。目标函数接收 tuple，返回实数；布尔值和数字字符串不作为目标值接受。Python 仅封装一次原生搜索调用，没有另一套搜索循环。

OptimizationResult 和 OptimizationEvaluation 是 NamedTuple，参数和历史都是拥有独立数据的 tuple。返回前已释放 C 句柄。没有有限结果或某次评估无效时，相应 value 为 None，便于 JSON 编码；停止原因、轨迹状态和失败文本仍单独保留。普通目标函数 Exception 记录为失败评估；MemoryError、KeyboardInterrupt、SystemExit，以及取消回调异常，在返回 Python 后重新抛出，确保先释放任何结果句柄。错误文本按完整 UTF-8 字符截断至 512 字节。

`examples/optimize_lc_match.py <native-library>` 使用相同的 LC 匹配参数，每个候选通过 Python 调用 C ABI 网络求解，再对全部轨迹进行独立阻抗公式验证，输出设计输入及完整 JSON 轨迹。使用已安装 wheel 或显式 PYTHONPATH=python 运行。此例是固定设计程序；声明式 JSON 优化任务见下文。

本次新增六个 C 导出符号，旧 ABI 版本和结构布局保持不变。新 wheel 需要包含这些符号的配套原生库；仅替换 Python 包而保留早期原生 DLL 不受支持。


## 跨语言阶段工程验证（2026-10-12）

[跨语言机读验收记录](../validation/optimization-api-validation-20261012.json)记录 Windows MSVC 与 Ubuntu WSL2 GCC 的 Debug/Release 四组 clean-first 构建，全量 CTest 各 136/136，安装后 C/C++ 消费者各 2/2。新 wheel 在四组安装原生库上均通过 25 套件、398 项测试，零跳过；其中 6 项是新增优化器 Python 回归。161 个 C/C++ 文件格式检查通过。

四组已安装 Python 包均运行 LC 示例并核验重复轨迹逐字节一致。[完整 Python 示例归档](../validation/optimization-python-lc-match-example-20261012.json)包含全部选项和 174 次评估。L≈3.978873680 nH、C≈3.183098337 pF，|S11|²≈6.9625e-15；每个候选的网络求解与独立阻抗公式核对，最大目标值绝对差小于 2.5e-15，参数相对解析值误差小于 1e-6。

原有八份未提交文件保持原哈希，全量回归仍包含既有参考脚本草稿，未声称纯净检出验证。未新增 SystemVue 分析、推送、macOS 或远程 CI 验证；不据本阶段关闭 A09 整项。


## JSON 优化任务与系统参数绑定

`rfmodel.optimization` version=1 包装一份既有分析文档。`analysis` 可使用线性网络、频率转换、转换网络、混频线性化、谱链、放大器分量、相干归约、相干网络或相干系统九种分析格式。其拓扑、器件和结果仍遵循原格式；任务不接受嵌套优化任务或执行代码的表达式。

必需字段是 format、version、analysis、parameters、objectives，可选 options。parameters 为 1..64 项，含唯一 id、lower、upper、initial、bindings，可选 kind（real/log/integer，默认 real）和说明性 unit。参数与原生优化器边界、尺度和整数契约一致。每项参数可绑定 1..256 个目标，总计最多 4096 个。bindings 的每项含 path，可选 scale=1、offset=0；写入数值为 `scale*parameter+offset`。scale 必须非零，整个端点范围必须有限。Integer 参数且 scale/offset 均为整数时，用精确整数运算写入 Python int；系数及结果须在 ±(2^53-1) 内。

path 是 1..64 段的数组：字符串查对象键，非负整数查数组索引，单字段对象按值唯一选择数组中的对象。例如 `['devices', {'id':'L'}, 'model', 'inductance_h']` 对应名为 L 的器件。选择器接受字符串或有限数值，数值 1 与 1.0 相等，布尔值不属于数值。绑定路径在模板中必须指向已经存在的有限数值叶子；不新增字段，不绑定 version，不接受两项绑定指向同一叶子（即使路径写法不同）。绑定在请求开始时解析为固定索引，所以可以一项参数驱动多个元件，也可以优化离散阶数或频率编号；具体候选仍须通过对应分析格式的校验。

每次评估先复制 analysis，再应用全部绑定。原始输入保持不变。目标路径每次在结果中重新解析，可用 `['streams', {'id':'result'}, 'power_by_bin', {'bin':18}, 'power_w']` 按输出流和频率编号选择测量。选择器零匹配或多匹配均失败，不静默选择第一项。结果数组会随参数改变时，宜使用稳定选择器；位置索引始终表示当次结果中的该位置。

objectives 为 1..256 项，每项含唯一 id、path、goal，可选 component、weight=1、scale=1 及 goal 所需的 target。weight 与 scale 须有限且严格为正。component 默认 value，要求有限实标量；real、imag、magnitude、power、magnitude_db、phase_radians 接受实数或原格式的 `[real,imag]`。power 为复数模平方，magnitude_db 为 20log10|z|，phase_radians 为 atan2(imag,real)。零幅度的 dB 测量失败，没有隐式噪声底截断。

令 m 为测量值、t 为目标、s 为 scale、w 为 weight，各项目标贡献为：

| goal | 贡献 |
|---|---|
| minimize | w·m/s |
| maximize | −w·m/s |
| target | w·((m−t)/s)² |
| at_most | w·(max(m−t,0)/s)² |
| at_least | w·(min(m−t,0)/s)² |

只有后三种 goal 要求 target，前两种不接受 target。总目标使用准确浮点求和；非有限测量、贡献或总值被记录为失败评估。上下限是带权惩罚项，不是另一个硬约束求解器；混合目标和单位权衡由使用者显式给定。目标标量是越小越好，不能把负值直接解释为误差。

options 支持 Python 原生封装的八个纯数据选项：max_evaluations、max_iterations、initial_step、minimum_step、contraction、target、invalid_evaluation、record_history；未写字段取公开默认值。Python 函数 `analyze_optimization(library, document, base_directory=..., cancelled=...)` 另支持取消回调，JSON 中不保存可执行回调。原生预算统计每次分析尝试，包括失败；保存或导出结果不额外运行分析。

结果格式为 `rfmodel.optimization-results` version=1，包含归一化参数、绑定、目标和选项、parameter_order、input_analysis、外部依赖路径/哈希、原生 result，以及 best_model、best_analysis、best_objectives。只缓存严格改善时的完整分析快照，原生历史仍记录全部参数、标量值、状态和错误；关闭历史不改变搜索或最佳快照。没有有限点时三个 best 字段均为 null。可使用原任务目录作为 base_directory 再次运行 best_model；相对输入路径仍以该目录为准，任务本身不会做这次额外求值。

Touchstone 相对路径以任务文件所在目录解析；Python API 用 base_directory 指定。输入文件须在整个运行期间保持不变，任务比较开始与结束的 SHA-256 指纹，不一致则拒绝返回结果；这不是对中间时刻文件内容的原子快照。CLI 同时保护任务文件、原生库与内嵌 Touchstone 路径，包括已有硬链接别名，防止输出覆盖输入。CLI 返回 0 表示成功写出完整报告，预算/目标函数失败等停止状态仍保留在 result.stop 中；不代表设计达标。请求格式或文件故障返回非零并保留原有输出。

两个完整输入可直接运行：

```sh
python -m rfmodel examples/optimize-lc-match.json --library <native-library> --output lc-result.json
python -m rfmodel examples/optimize-image-rejection.json --library <native-library> --output image-result.json
```

LC 任务与前述两元件解析解一致。镜像抑制任务在共享 LO 时钟、正交 RF 分支及合路网络中优化一个 LO 相位，同时最小化上边带镜像功率并要求下边带功率至少 0.99 W。输入为 1 W，独立解析关系为 `P_image=(1−sin φ)/2`、`P_desired=(1+sin φ)/2`；最优相位 π/2。回归逐点比较组合目标，不用搜索输出倒推参考公式。

此任务层通过 Python/CLI 编排已有 C++ 数值原语；JSON 解析不进入 C++ API，也未增加原生 ABI。任意表达式、拓扑增删、随机分布/Monte Carlo 及厂商优化算法等价性仍不属于当前已验收范围。


## JSON 任务阶段工程验证（2026-10-12）

[任务层机读验收](../validation/optimization-task-validation-20261012.json)记录 Windows/Linux Debug/Release 四组完整 CTest 各 137/137，安装消费者各 2/2，新 wheel 各 26 套件、410 项，零跳过。12 项新任务测试覆盖上述契约及九类分析入口。Windows 构建全部目标，原生源码未改；Linux 使用新的原生文件系统构建目录。

四组安装包均通过 CLI 执行并重放两个完整任务，同配置输出逐字节一致。[LC 完整结果](../validation/optimization-task-lc-match-example-20261012.json)保存 174 次评估，达到 |S11|²≈6.9625e-15；[镜像抑制完整结果](../validation/optimization-task-image-rejection-example-20261012.json)保存 66 次评估，φ≈1.570796466 rad，镜像功率≈4.8404e-15 W，目标边带功率≈1 W。每一评估点都对照独立电路公式；两类任务的最大目标值绝对差均小于 1e-12。

此前跨语言阶段提交 `0dc3bc4` 已按恢复的阶段推送授权发布到 GitHub，并通过 Windows/Ubuntu/macOS Debug/Release 六组干净检出 CI：各 136/136 核心测试、2/2 安装消费者和 398/398 wheel 回归。[CI 机读记录](../validation/optimization-api-ci-validation-20261012.json)精确对应该提交，不能替代随后新增 JSON 任务的远程验证；任务层报告生成时尚待提交推送。历史报告的未推送标记保持其记录时刻的含义。

此前声明式优化任务提交 `b0a7158` 已通过 Windows/Ubuntu/macOS Debug/Release 六组干净检出 CI，每组为 137/137 核心测试、2/2 安装消费端及 410/410 wheel 回归。[该提交的 CI 机读记录](../validation/optimization-task-ci-validation-20261012.json)现已归档，补充任务层验证的发布状态，不覆盖后续 Monte Carlo 实现。
