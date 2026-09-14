# 线性网络求解 API

`rfmodel/network.hpp` 的 `LinearNetwork` 接收单频 S 矩阵块。`add` 返回块的全局首端口编号；`connect(p,q)` 连接两个端口；`terminate(p,gamma,source)` 设置边界 `a[p]=source+gamma*b[p]`。未连接端口必须显式终端化。

每块满足 `b=S*a`，连接矩阵与激励满足 `a=C*b+e`，因此求解 `(I-C*S)*a=e`。内部组装块对角 S 矩阵，使用带部分选主元的复数高斯消元，返回全部入射波、出射波和归一化残差。阈值检查拒绝奇异或数值上难以可靠求解的网络。

```cpp
rfmodel::LinearNetwork network;
network.add(rfmodel::SMatrix{2,{0.,1.,1.,0.}});
network.terminate(0,0.25,1.0);
network.terminate(1,0.5);
auto waves=network.solve(); // a0=8/7, b0=4/7, b1=8/7
```

API 波量约定为相同正实数参考阻抗下的功率波，模平方单位为 W；dBm 仅为后续显示换算单位。`add` 的参考阻抗必须与构造参数一致，当前不会做自动重归一化。默认值均为 50 欧姆。

实现上限为 1024 个总端口，使用稠密矩阵。当前尚无条件数估计、稀疏求解或大网络性能验证。频率扫描、器件能力适配、等效外端口 S 矩阵提取和复参考阻抗仍需继续实现。

验证：匹配级联、源/负载失配的反馈解析值、三端口理想结点、闭环奇异矩阵、重复端口占用、参考不一致和未终端化端口；MSVC Debug/Release 全套 CTest 各 5/5。尚未与 SystemVue 2023 对照。
