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

当前 ABI 尚未暴露器件模型构造、噪声、非线性、混频、参数扫描和模型文件加载；
[Python 封装](python-api.md)已覆盖当前线性网络接口。后续沿此接口扩展，
不将当前线性网络入口视为完整外部 API 验收。
