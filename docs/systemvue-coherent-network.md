# SystemVue 2023 双源相干合路参考

## 当前证据

2026-10-09 从本机 SystemVue 2023.0.0.11903 官方
RF Design Kit/Phase/Spectrum Phase.wsv 复制到项目 build-reference，
通过既有后台 COM/RunScript 接口执行。原安装文件未改动。
上一专用放大器工作区先 SaveAs 到新备份文件，再关闭；切换时确认实例中仅有指定副本。

电路保留示例原有连接：两个 MultiSource 各经过一条 TLE，在同一节点连接，
再经过 5 dB ATTN_Linear 到 50 ohm 输出。三端节点按等参考阻抗理想 tee 建模，
对角 S=-1/3、非对角 S=2/3。这里没有 SPLIT2 或 HYBRID 器件，
不能据此验收其有限隔离、默认相位或不平衡参数。

本轮成功记录只有一个配置：
- 两路均为 1 GHz、0 dBm CW，源相位均为 0；
- 两路 RefClk 都为 RFModelClock，参考结果中的相干编号相同；
- 两条线均为 50 ohm、无损，电长度 **30 rad**，标定频率 1 GHz；
- 衰减器输入输出及源/负载阻抗均为 50 ohm。

30 rad 是采集后由参数回读确认的实际配置，不是 30 度。官方例子中的 TLE 长度
为可调参数，Set 后直接存储原生弧度；普通源 Phase.Set 则以显示角度解析。
检查器强制比对原生参数值，不把意图当成生效参数。
运行器新增 --coherent-length-rad 明确这一单位，默认 pi/6，重放本记录要传 30。

## 比较范围与结果

compare-coherent-network.py 使用 RFModel 的 Network.transmit_coherent，
构造两条线、三端 tee 和衰减器的完整连接网络。全局端口刻意乱序选择，
每路单独激励以验证来源项的复幅度，再共同激励以验证合并功率。

两个来源分别导出 CW 上下边界的 V3/Z3/P3，检查输出阻抗接近实 50 ohm、
峰值电压与功率一致，然后转换为 sqrt(W) 功率波。RFModel 在中心频率建模；
边界距中心为 0.5 Hz，30 rad 传输线引起的小相位变化包含在原 1e-7 容差内。

- 四个边界复幅度比较最大相对差异 1.90636e-8；
- 合路后衰减器输入 RFPwrIn(Attn1) 为 0.0017777777412593342 W；
- RFModel 为 0.0017777777777777776 W，绝对差异 3.65185e-11 W，
  小于相对 1e-7 所对应的 1.77778e-10 W。

RFPwrIn 是参考软件独立导出的合并 RF 输入功率，未从待比较的预测值反推。
本轮没有启用节点总谱，未将两个路径功率直接相加冒充相干合并后的测量值。
输出功率波预测及输入功率检查分别保留在报告中。

精简采集 validation/systemvue-2023-coherent-network-captures.json 保存参数、
频率/功率/复电压/复阻抗/相干编号、数据集时间戳、原始采集 SHA256 和工作区
文件 SHA256。比较器拒绝过期或重复记录、错误版本、模式/相位/长度不匹配、
编码错误和时钟关系与实测相干编号不一致的输入。
报告 validation/systemvue-2023-coherent-network.json 另存采集文件与原生 DLL 散列。

    python scripts/reference/compare-coherent-network.py build-msvc/Release/rfmodel_c.dll validation/systemvue-2023-coherent-network-captures.json build-reference/coherent-network-report.json

## 后台重放与错误恢复

    python scripts/reference/run-systemvue-reference.py coherent build-reference/RFModel_PhaseCombiner.wsv build-reference/coherent-new-run --coherent-locked --coherent-phase-deg 0 --coherent-length-rad 30

输出目录必须全新；工作区应为同名官方示例副本，且为该实例唯一打开的工作区。
仅在实例没有工作区时添加 --open-copy。独立时钟省略 --coherent-locked；
源 2 相位由 --coherent-phase-deg 控制。不得在前一采集仍在运行时再次提交。

尝试修正长度并开启总谱时，新增 SetValue/SetProperty 调用触发了
“Error Running Script”模态提示；尚未读取到具体报错，不能确定两者中是哪一处。
该次运行目录 coherent-locked-phase0-003 为 timeout_unresolved，**不作为验收数据**。
代码已撤回这两种调用，改回成功使用过的 Set 字符串形式，并离线编译检查。
修正版尚需在错误提示解除后重新实际采集，不能只凭离线编译宣称恢复成功。
电脑操作运行时遇到 Windows sandbox setup refresh 错误，因此已请用户提供提示内容
并关闭错误提示；没有强制终止 SystemVue 或重启仍存活的采集任务。

## 待完成

1. 共时钟 90/180 度及独立时钟 0/90/180 度扫描；
2. 单源真实功分后经过不同支路再合路的实测；
3. SPLIT/HYBRID 全复数矩阵、有限隔离和阻抗参数；
4. 自动时钟/相干 ID 推导，以及第二非线性级和变频联合传播。

新增比较器 6 项回归覆盖真实数据、相位翻转仍保持功率、篡改合并功率、
错误相干编号、过期/重复数据以及元数据/复杂数组错误。
本阶段不是完整分支相干或 RF System Analysis 兼容验收。

2026-10-09 本地全套 CTest：Debug/Release 各 62/62；后台运行器 12 项回归通过，修正版 C# 采集器离线编译通过。原生 C++/C API 本轮未改动，沿用上一提交已验证的安装包；跨平台结果以本阶段 CI 为准。

## 源时钟分组扩展

后续比较器已改用 RFModel 原生源/参考时钟解析器生成输入组，不再把 SystemVue
相干编号作为预测输入。以实测编号核对源关系等价性，并继续比较同一批路径复幅度
和 RFPwrIn；详见 source-coherence.md。这是对已有一组数据的更强验证，
没有增加独立时钟或反相实测点。比较器现有 7 项回归，新增错误原生分组的失败测试。
