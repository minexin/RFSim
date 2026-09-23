# 群时延后处理

`group_delay(FrequencyGrid, response)` 对相邻复数响应的相位差按 2π 取主值，计算 `-Δphase/(2π Δf)`。N 个样本返回 N-1 个区间结果，各项包含中点频率 Hz、区间宽度 Hz 和时延秒。支持非均匀网格。

要求真实相邻相位变化绝对值小于 π；采样不足造成的相位混叠无法由这些样本自动判断。恰好半周的差值、零响应、不递增网格和非有限数据明确报错。近零响应的相位易受噪声影响，当前没有幅度门限滤波。

本机 SystemVue2023/Help/systemvue.qch 的 users/function_groupdelay.html 说明其返回 ns，计算孔径为频率步长。RFModel 选择显式区间及 SI 秒契约（转 ns 乘 1e9）；还未验证与其边界取值、输出尺寸、相位不连续处理完全一致。

MSVC Debug/Release 全套 CTest 15/15 通过：2 ns 延迟跨相位分支、非均匀网格、二次相位的精确中点导数、零响应和错误输入。
