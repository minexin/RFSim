# Python 线性网络接口

`python/rfmodel` 使用标准库 ctypes 调用 C ABI，运行不依赖 NumPy；要求 Python 3.9 及以上。
这是 RFModel 自身的脚本接口，与用于采集参考数据的 SystemVue 自动化脚本分别使用。
原生动态库需由 CMake 单独构建或安装，Python 包不包含二进制，也不会自动下载或启动外部程序。

```powershell
python -m pip install ./python
```

通过 `Library` 显式提供原生库路径，Python 与库的架构必须一致。Windows Release 使用
`rfmodel_c.dll`、Debug 使用 `rfmodel_cd.dll`；Linux/macOS 使用对应的 `.so`/`.dylib`。
加载时检查 ABI 版本，当前要求 1。

```python
from rfmodel import Library

library = Library("build-msvc/Release/rfmodel_c.dll")
with library.network(reference_ohms=50.) as network:
    network.add([[0, -0.5j], [-0.5j, 0]])
    network.add([[0, 0.2], [0.2, 0]])
    network.connect(1, 2)
    s = network.external_s([0, 3])
    print(s[1][0])  # -0.1j
    network.terminate(0, source=2.)
    network.terminate(3)
    waves = network.solve()
    print(waves.outgoing[3])  # -0.2j
```

## 数据与生命周期

- `add` 接受方形嵌套序列，元素可为 Python 数值或 complex，返回器件首个全局端口编号。
  默认采用网络参考阻抗；显式指定不同阻抗会报错。输入会复制到原生库。
- `external_s` 返回按所选端口顺序排列的不可变复数行元组；只提取矩阵，不改变网络边界。
- `terminate` 的约定为 `a=source+reflection*b`；`solve` 返回 `Waves`，包含
  incident、outgoing 两个复数元组和 relative_residual。功率波单位为 sqrt(W)。
- 推荐使用 with 或显式 `close()`；close 可重复调用，垃圾回收仅作为释放兜底。
  已关闭的网络在进入原生库前即被拒绝；with 不吞掉调用者异常。
- 每个网络有独立锁，方法调用与 close 不会同时访问同一句柄；多个方法组成的事务
  若要求不被其他线程插入，仍需调用者同步。不同网络可以并行使用。
- 原生错误映射为 `RFModelError`，保留数值 status 和错误文本；Python 层形状、
  索引、类型校验使用 ValueError/TypeError。负索引、超大整数、浮点和 bool 索引不会截断成 size_t。

## 验证与范围

`python tests/test_python_api.py <native-library-path>` 使用真实动态库测试复数级联、
错误恢复、奇异网络、形状/非有限值/索引校验、显式及自动释放。CMake 找到 Python 3.9+
时会注册此测试；CI 设置 `RFMODEL_REQUIRE_PYTHON_TESTS=ON`，缺少解释器即配置失败。

当前封装覆盖已公开的线性网络 C ABI。器件参数模型、噪声、非线性、混频、扫频及模型文件
加载尚未作为 Python API 暴露；可以由脚本逐频点传入 S 矩阵，但这不代表完整系统分析接口已完成。

2026-09-29 本地验证：Python 3.10.6，六项 Python 测试通过；加入 CTest 后 MSVC
Debug/Release 各 42/42。通过 pip 默认隔离构建生成纯 Python wheel，并安装到
`build-python-package`；从该目录导入封装、分别加载安装后的 Debug/Release 原生库，
各六项测试再次通过。SystemVue 自带 setuptools 在禁用隔离构建时缺少 `_distutils_hack`，
因此未使用该模式，也未修改其 site-packages。安装后的测试可用 `--installed` 开关，
并通过环境变量 PYTHONPATH 指向包安装目录。本次跨平台结果待 CI 核验。
