# Touchstone 数据接入

读取 legacy `.sNp` 全矩阵 S 参数，RI/MA/DB 转换，Hz/kHz/MHz/GHz，行内注释、前导空白、矩阵续行及尾部缺省选项。统一参考电阻保存在 `reference_impedance_ohms`，不隐式重归一化。

依据 [IBIS Touchstone 规范](https://www.ibis.org/touchstone_ver2.1/touchstone_ver2_1.pdf) 对旧版数据约定的说明，二端口 S11/S21/S12/S22 转为内部行优先存储，其余端口数使用行优先。

输入必须有选项行、有限数值、正参考电阻和非负严格递增频率。不支持的参数类型/单位、重复头、残缺记录、非法扩展名及版本 2 关键字明确报错。读取器资源上限为 1024 端口，并非格式标准上限。

测试包含非互易二端口传输方向、三端口续行顺序、75 欧姆参考、缺省 GHz/MA，以及 14 类非法输入；MSVC Debug/Release CTest 各 4/4 通过。

尚未支持：Touchstone 2 元数据、噪声区段、逐端口参考阻抗。尚未执行 SystemVue 导出数据的差异验证；不宣称完整 Touchstone 兼容。
