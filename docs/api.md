# RFModel API 首阶段约束

- 语言标准：C++17。
- 构建系统：CMake 3.20+。
- 平台：Windows/MSVC 优先；禁止在公共头文件使用 Windows 专属类型，保持 Linux/macOS 可移植。
- 单位：内部频率统一 Hz，内部功率使用 W，dBm 用于显示换算，阻抗使用欧姆。
- 复数波按端口顺序存储，端口参考阻抗显式传递。
- 器件模型只描述行为；网络连接、求解、扫描和结果汇总由求解器负责。
- S 参数、噪声、非线性、频率转换采用独立能力接口。
- 对外 ABI 暂不承诺稳定；v1.0 前优先保证源码兼容和测试可复现。

双向小信号放大器参数与使用示例见 [linear-amplifier.md](linear-amplifier.md)。

失配二端口的换能、工作和可用功率增益见 [power-gain.md](power-gain.md)。

各级端口功率及累计增益预算见 [linear-path.md](linear-path.md)。

原生固定泵浦 PHD 系数切片（显式 RMS 功率波 F/S/T、受限扰动 Jacobian、
不提供通用网络或 Newton 适配器）见 [PHD 系数契约](phd-coefficient-contract.md)。
输出匹配、谐波反馈和非泵小扰动可使用拥有完整约束的
[固定泵确定性网络](phd-fixed-pump-network.md)，每个非泵通道必须显式给出有效入射扰动上限。
