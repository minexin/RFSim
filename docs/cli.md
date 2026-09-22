# 批处理 CSV 导出

构建生成 `rfmodel_cli`，安装位于 bin。使用 `-DRFMODEL_BUILD_CLI=OFF` 可只构建库。

```powershell
./build-msvc/Release/rfmodel_cli.exe tests/sweep.s2p 1000000000 2000000000 3000000000 > build-msvc/sweep.csv
```

不传频率参数时导出原始采样频点；传入的 Hz 频率必须严格递增。带外频率报错，不静默外推。输出字段为 frequency_hz、row_port、column_port、reference_ohms、s_real、s_imag，CSV 端口编号从 1 开始，S 矩阵按行优先展开。输出使用独立于系统区域设置的小数点和 17 位精度。

成功退出码 0，参数个数错误为 2，模型/数值错误为 1；诊断写入 stderr，模型错误发生时 stdout 不包含部分 CSV。输出设备故障可能在部分写入后报告，调用者应同时检查退出码。

这是单模型 S 数据导出工具，尚未支持网络拓扑文件、噪声或非线性分析。为后续参考比较提供机器可读数据入口，不代表已完成 SystemVue 对照。

CTest 验证中间频点的完整 CSV 文本、带外/NaN/非法频率串失败行为；MSVC Debug/Release 全套各 12/12 通过。
