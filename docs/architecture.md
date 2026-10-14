# 总体架构

```text
Host / vendor parameters -> ModelAdapter (identity + optional capabilities)
                          -> S + references + noise -> LinearNetwork
                          -> spectrum callback -> SpectrumAnalysis
                          -> response + Jacobian -> Conversion operating-point solver
                          -> coherent operation -> CoherentSystem / CoherentPipeline
```

器件负责行为数据和导数；求解器负责端口连接、网络方程、迭代和结果汇总。能力由实际回调声明，不强制所有器件实现所有分析，也不把缺失噪声解释为零噪声。具体契约、所有权和验收见 [模型能力与引擎适配](model-adapters.md)。

线性模型以 S 和逐端口功率波参考接入；非线性工作点以直接/共轭 Jacobian 接入。相干系统图提供 `stage()` 扩展入口，支持复用已有来源运算。一般频谱、来源追踪、频率规划与噪声是不同契约，不能相互自动替代。

`adapters/` 属于外层模型接入代码。SystemVue 版本参数与经验映射留在该层，底层数值相位组合按类型契约调用。既有模型和旧头文件入口继续保留；新代码可直接包含通用接口。各目录是职责划分，交付仍为 C++17 `RFModel::rfmodel` 头文件 SDK 及既有 C ABI。宿主管理文件格式、模型选择和 UI；Python/JSON 产品接口已退役。
