# 十一阶多项式与 ABI 兼容

通用实电压多项式现在支持到十一阶，覆盖 SystemVue 2023 RFAMP 阶数选择器
的上限。此阶段完成数值内核和接口扩展；没有新增 SystemVue 十一阶实测，
不能据此宣称 RFAMP 十一阶模型已通过兼容验收。

## 接口范围

| 接口 | 本级最高阶数 | 输出来源布局 |
| --- | ---: | --- |
| C++ CoherentPolynomial | 11 | std::array<int, 11> |
| 原 C rfmodel_coherent_polynomial_evaluate | 9 | 原结构的 9 个下标，布局不变 |
| 新 C rfmodel_coherent_polynomial_evaluate_v2 | 11 | 新结构 rfmodel_coherent_polynomial_term_v2，11 个下标 |
| Python Library.coherent_polynomial | 11 | 调用 v2，有效下标元组 |
| MemorylessPolynomial / polynomial_amplifier | 11 | 动态频谱数组，保留 DC |
| polynomial_coefficients_from_intercepts | IP2..IP11 | 最多十条截点定义、十二个电压系数 |

C ABI 版本仍为 1，新增函数符号不改变旧结构和旧函数的九阶限制。
C++ 公共项结构增加两个下标，使用这些头文件的 C++ 消费者应重新编译。
更新后的 Python 包要求配套带 v2 符号的共享库。

递归来源展开及来源表达式乘积允许 1..11 个本地父索引。
相干系统图与 spectrum-chain 格式都接受最多十二个电压系数。
已有 JSON 格式版本保持不变。

## 数值与资源约定

本地阶数与原始源阶数仍是不同概念。一个经过理想混频的 RF 项包含 RF 和 LO
两个根因子；对它作十一阶运算产生 22 个根因子。系统图的 max_source_order
按实际根因子数量裁剪，不是本地系数数组长度，也不等同于 SystemVue 全局阶数。

本级相干输出仍最多 4096 项；非线性输入最多 64 个，枚举最多一千万次访问。
来源表达式仍受原有贡献项、存储因子和分配乘积限制，单项最多 256 个根因子。
提高阶数不会放宽这些资源限制，失败不会返回部分结果。

## 可重跑验证

- 原生 coherent_polynomial_test 以另一套稀疏傅里叶卷积校验一至十一阶、
  50/75 Ω、复相位、共轭、排列数和完整 RF bin。
- nonlinear_spectrum_test 将带 DC 偏置的十/十一阶频谱还原为实波形，
  与直接实数幂逐点比较，并覆盖最高阶系数访问和齐次分量提取。
- polynomial_intercepts_test 用独立实波形采样和傅里叶投影检查 IP2..IP11。
- c_api_test 验证旧结构尺寸和成员偏移、数组步长、缓冲区哨兵、新旧九阶结果一致、
  十一阶 H11 幅度，以及容量失败时两个缓冲区和计数保持不变。
- Python API 覆盖十/十一阶、十条截点和来源展开；系统图同时校验纯多项式与
  混频后十一阶的完整复波及来源，检查全局阶数裁剪和十二阶输入拒绝。
- 安装后的独立 C/C++ 消费项目调用新增符号；wheel 在独立 Python 3.12 中验证。

## 本地工程结果（2026-10-09）

- MSVC Debug / Release 清理重建后完整 CTest 各 79/79。
- 安装后的 C/C++ 消费者两种配置各 2/2。
- 独立 Python 3.12 wheel：API 73 项、系统图 46 项通过。
- 112 个 C/C++ 文件格式检查通过。
- [机器可读工程记录](../validation/eleventh-order-polynomial.json)保存命令、
  日志哈希和运行库哈希。跨平台 CI 由本次提交关联的 GitHub Actions 记录提供。

既有四、五阶 SystemVue 原始采集与诊断报告保持原样；RFAMP 自动五/七/九/十一阶系数、总谱语义及更广参数范围
仍需后续受控实测。
