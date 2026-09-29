# C ABI：线性网络接口

公共头文件为 `rfmodel/c_api.h`，动态库目标为 `RFModel::c_api`，Windows 文件名为
`rfmodel_c.dll`，Linux/macOS 使用平台共享库命名。默认构建；设置
`-DRFMODEL_BUILD_C_API=OFF` 可只构建原有 C++ 核心和所选工具。
ABI 版本由 `rfmodel_abi_version()` 返回，目前为 1。
Debug 库名追加 `d`，允许 Debug/Release 安装到同一前缀且不互相覆盖。

独立 C/C++ 项目通过安装包 `find_package(RFModel CONFIG REQUIRED)` 后，链接
`RFModel::c_api`。Windows 运行时需将 DLL 放在应用目录或可搜索目录；链接导入库
本身不完成 DLL 部署。C++ 头文件核心仍可单独使用 `RFModel::rfmodel`。

## 调用流程

1. `rfmodel_network_create` 创建指定正实参考阻抗的网络。
2. `rfmodel_network_add` 复制一块逐行排列的复数 S 矩阵，返回其第一个全局端口号。
3. `rfmodel_network_connect` 连接两个端口，或以 `rfmodel_network_terminate` 设置边界
   `a=source+reflection*b`。端口不能重复连接或终端设置。
4. `rfmodel_network_solve` 写入各全局端口的入射波、出射波及求解相对残差。
   所有端口必须已经分配边界；调用者通过 `rfmodel_network_port_count` 查询缓冲区长度。
5. `rfmodel_network_destroy` 释放句柄，允许 NULL。

另可用 `rfmodel_network_external_s` 提取选定外部端口的等效 S 矩阵，顺序由端口数组决定。
这些端口必须未分配边界，其余端口必须已连接或终端设置，且不得含独立源。

`rfmodel_complex` 是两个 double 字段 real/imag，不要求调用者使用 C99 complex 或
`std::complex` 的内部布局。输入 S 参数是无量纲波比，波幅以 sqrt(W) 归一化。
网络为单频点，各块参考阻抗必须一致，总端口上限沿用核心的 1024；扫频需逐频点构建网络。

## 错误与所有权

状态返回 0 表示成功，非零值分别区分非法参数、求解错误、内存分配失败和内部错误。
`rfmodel_last_error()` 返回当前线程的固定缓冲区文本，下一次返回状态的 API 调用会替换它；
若需保留请复制。C++ 异常均在 ABI 内捕获。无网络句柄的创建失败也可以读取错误。

创建失败将输出句柄设为 NULL。其他错误不写结果数组；添加器件采取成功后提交，
失败不会留半个器件。调用者拥有输入/输出数组，容量单位为复数元素数，不是字节。
缓冲区必须有效且满足声明的容量，输出区域不得相互重叠。库无法验证任意地址是否有效。
句柄不可重复释放或在释放后使用；创建输出指针应指向空变量，不能覆盖尚未释放的句柄。
同一句柄的访问由调用者同步，不同句柄可在不同线程使用。

## 验证与剩余接口

C 编译测试覆盖两块衰减器级联、等效 S 提取、端口波量、错误恢复、短缓冲区、
未终端网络、奇异网络、NULL、参考阻抗不一致和错误码/文本。
安装消费项目另由 C 编译器编译，调用安装后的共享库并核对复数传输值。
三平台 CI 会构建及运行这些测试。

2026-09-29 本地验证：MSVC Debug/Release 全套各 41/41，独立安装消费者各 2/2
（一个 C++、一个 C）；Debug/Release 使用各自的动态库名称完成加载。
本次提交的 Linux/macOS 执行结果仍待推送后 CI 核验。

当前 ABI 已暴露下述两类传输线及小信号放大器求值，其他器件模型、非线性、混频、参数扫描和模型文件加载尚未暴露；
[Python 封装](python-api.md)已覆盖当前线性网络接口。后续沿此接口扩展，
不将当前线性网络入口视为完整外部 API 验收。

## 噪声接口

`rfmodel_passive_noise` 根据完整 S 矩阵及开尔文温度计算被动热噪声协方差
`kT*(I-S*S^H)`，拒绝非无源矩阵。`rfmodel_network_external_noise` 将内生噪声
传播至指定外部端口，包含网络内部反射反馈。内生矩阵必须覆盖所有全局端口，
可包含器件间复数相关项，单位 W/Hz；库验证厄米性及半正定性。
两个接口均使用逐行排列的 rfmodel_complex，容量以元素数计，错误时不写输出。

外部端口须未分配边界，其他端口须连接或终端设置，所有独立源为零。
外部端口按匹配、无噪声处理；终端温度和源噪声不会自动加入。
本轮仅新增符号，ABI 版本仍为 1，已有调用保持兼容；使用噪声符号须部署包含该扩展的库。

2026-09-29 噪声扩展验证：Debug/Release 全套各 42/42，安装消费者各 2/2，
Python 八项测试在安装后的 Release 动态库上通过。新增覆盖级联热噪声、跨器件复相关、
非法协方差、输出短缓冲区及失败时保留输出。尚无本次提交的跨平台 CI 结果。

## 参数模型求值

`rfmodel_transmission_line_s` 和 `rfmodel_rlgc_line_s` 无需网络句柄，直接调用现有
C++ 传输线模型并写入四个逐行排列的 S 参数。参数顺序及 SI 单位见 c_api.h；
传播损耗单独使用 dB。输入错误或容量不足时不写输出。求值结果可以传给 network_add，
也可以直接计算被动热噪声。这是 ABI 1 的新增符号，需部署含该扩展的动态库。

`rfmodel_linear_amplifier_s` 接受频率、正向增益/相位、反向隔离/相位、复数输入/输出
阻抗及参考阻抗，输出四项 S 参数；仅小信号双向模型，不自动添加放大器噪声。
奇异阻抗（如输入阻抗为负参考阻抗）对应 domain_error，现统一转换为
RFMODEL_INVALID_ARGUMENT，不再落入内部未知错误；输出矩阵保持不变。

新增 cubic_amplifier_transmit 与 ideal_mixer_transmit 两个频谱传输入口，见
[非线性与混频频谱接口](spectrum-api.md)。这是匹配单向器件调用，不改变线性网络求解器语义。
