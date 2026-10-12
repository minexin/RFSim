# 有界参数优化：C++、C ABI 与 Python 契约

A09 的第一项原生实现提供 `include/rfmodel/optimization.hpp` 中的 `minimize_bounded(parameters, objective, options, cancelled)`。目标是最小化调用者给定的一个有限实标量；可用它调用 RFModel 网络、噪声或器件原语并组合设计误差。现已提供 C ABI 和 Python 同步回调入口；通用 JSON 任务输入、Monte Carlo 与 SystemVue 优化器对照尚未完成。

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

A09 仍需通用任务格式、系统级参数绑定、Monte Carlo 分布/种子及统计参考，以及厂商优化与统计行为对照；本阶段不关闭整项验收。

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

`examples/optimize_lc_match.py <native-library>` 使用相同的 LC 匹配参数，每个候选通过 Python 调用 C ABI 网络求解，再对全部轨迹进行独立阻抗公式验证，输出设计输入及完整 JSON 轨迹。使用已安装 wheel 或显式 PYTHONPATH=python 运行。此例是固定设计程序，尚不提供通用 JSON 优化任务输入。

本次新增六个 C 导出符号，旧 ABI 版本和结构布局保持不变。新 wheel 需要包含这些符号的配套原生库；仅替换 Python 包而保留早期原生 DLL 不受支持。


## 跨语言阶段工程验证（2026-10-12）

[跨语言机读验收记录](../validation/optimization-api-validation-20261012.json)记录 Windows MSVC 与 Ubuntu WSL2 GCC 的 Debug/Release 四组 clean-first 构建，全量 CTest 各 136/136，安装后 C/C++ 消费者各 2/2。新 wheel 在四组安装原生库上均通过 25 套件、398 项测试，零跳过；其中 6 项是新增优化器 Python 回归。161 个 C/C++ 文件格式检查通过。

四组已安装 Python 包均运行 LC 示例并核验重复轨迹逐字节一致。[完整 Python 示例归档](../validation/optimization-python-lc-match-example-20261012.json)包含全部选项和 174 次评估。L≈3.978873680 nH、C≈3.183098337 pF，|S11|²≈6.9625e-15；每个候选的网络求解与独立阻抗公式核对，最大目标值绝对差小于 2.5e-15，参数相对解析值误差小于 1e-6。

原有八份未提交文件保持原哈希，全量回归仍包含既有参考脚本草稿，未声称纯净检出验证。未新增 SystemVue 分析、推送、macOS 或远程 CI 验证；不据本阶段关闭 A09 整项。
