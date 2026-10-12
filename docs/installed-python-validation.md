# 安装 wheel 与原生 SDK 的独立验证

A10 发布验收要求同时验证 C++ 安装包、共享 C ABI 和 Python wheel。构建出 wheel 不等于 wheel 能被独立导入，更不等于它加载的是刚安装的原生库。本阶段新增 `scripts/verify-installed-python.py`，供本地验证和三平台 CI 共用。

## 验证路径

```sh
python -m pip wheel ./python --no-deps --wheel-dir python-dist
cmake --install build --config Release --prefix /absolute/path/install
python scripts/verify-installed-python.py --wheel-dir python-dist \
  --install-prefix /absolute/path/install --configuration Release \
  --work-dir build-reference/installed-wheel-Release
```

work-dir 必须是新目录。验证器不会清理或复用已有目录，避免混入旧报告和过期模块。没有 `--package-root` 时，调用当前解释器的 pip，以 `--no-index --no-deps --target` 离线安装指定 wheel 到本次 work-dir/site。pip 本身必须可用；wheel 构建所需 setuptools 由前面的构建步骤提供。

已有独立虚拟环境或目标目录时，可传 `--package-root /path/to/site-packages` 跳过安装，仍执行相同的内容、来源和运行验证。这种模式的报告不冒充执行了 pip 安装，外部安装日志应另行归档。源码/数据夹具默认由脚本所在仓库确定，也可用 `--source-root` 显式指定。

验证器执行以下检查：

1. 所选目录只能有一份 RFModel wheel；Debug/Release 必须分别找到唯一的安装后原生库。Windows 使用 rfmodel_cd.dll/rfmodel_c.dll，Linux 使用 librfmodel_cd.so/librfmodel_c.so，macOS 使用相应 dylib。解析后的库路径须位于安装前缀内。
2. 安装目录不能是源码 python 树。wheel、当前源码与安装后每个 Python 模块的内容须逐字节一致；缺失、额外旧模块或不同源码构建的 wheel 均失败。安装后的 distribution METADATA 也必须与 wheel 一致。
3. 自动枚举 tests/test_*.py，任何新增绑定套件缺少 --installed 支持都会明确失败，不能被安静跳过。
4. 每组在新的工作目录中，以 `python -I` 启动独立进程；显式插入 wheel 安装目录，清除继承的 Python 路径/启动配置，并关闭用户 site。加载 RFModel 后核对包路径、ABI 1 和绝对原生库路径。
5. 运行套件后核对所有已导入 rfmodel 模块来自安装目录。RFModel CLI 子进程必须只使用安装目录的 PYTHONPATH；报告记录实际 CLI 命令和模块来源，防止测试悄悄退回源码。
6. 根据 unittest 实际执行的数量、失败、错误和跳过项判定结果；没有执行测试、子进程超时、缺少子报告或非零退出均失败。套件失败后继续记录其他套件结果，最终进程仍返回非零。

每组日志和结果保留在 work-dir/test_<suite>/；总报告为 installed-python-validation.json，包含解释器、平台、配置、wheel/原生库/验证器/测试源码 SHA256、安装内容、全部套件结果和耗时。报告保留绝对路径以便追查实际加载来源。新目录内采用临时文件替换来写 JSON，已有运行目录不会被覆盖。

## 修复旧安装模式的一处源码泄漏

`tests/test_python_api.py` 的 coherence CLI 用例此前无论是否 --installed，都会将 PYTHONPATH 指向仓库 python 目录。本阶段改为使用已经导入的 `rfmodel.__file__` 所在安装目录，并用验证器的 CLI 路径检查约束全部套件。

旧报告中“python_api 套件从 wheel 导入”的结论不能单独证明这一 CLI 子用例也走安装路径；历史报告保留，本阶段以新的隔离运行证据补齐。没有修改任何数值阈值。

## CI 接入

`.github/workflows/build.yml` 保留 Windows/Ubuntu/macOS × Debug/Release 六组合，新增显式 Python 3.12 环境，并把同一解释器传给 CMake 的 Python3_EXECUTABLE。每个组合依次执行原生构建、全量 CTest、安装、独立 C/C++ 消费者、wheel 独立安装及所有 Python 绑定套件。

工作流还运行验证器的失败导向自测，并在成功或失败后保存 JSON、各组日志和 CTest 日志。每个矩阵组合使用唯一 artifact 名称。解释器配置和报告归档使用官方 [setup-python](https://github.com/actions/setup-python) 与 [upload-artifact](https://github.com/actions/upload-artifact) 接口。只修改工作流不算远程执行成功；本阶段仍遵守不推送约束。

## 本地验证范围与限制

当前绑定套件为 23 组、379 项。验证器另有 10 项失败导向测试，覆盖错误配置/缺失或重复原生库、源码被当成安装目录、wheel/源码/安装内容不一致、缺失/多余模块、新套件未支持安装模式、CLI 源码路径泄漏、环境污染及失败报告/旧结果保护。

Linux 使用已有 Ubuntu 24.04.1 WSL2、GCC 13.3、CMake 3.28.3、Python 3.12.3。Windows 使用现有 MSVC 工具链，wheel 验证由 Python 3.12.14 执行。Windows 原生 C++ 源未变，采用增量全目标构建；Linux 从新的构建目录编译两种配置。两端测试同一个 py3-none-any wheel；它由 Windows Python 构建，Linux 将其离线安装到项目内新建的虚拟环境，没有改动系统 Python。

Linux 首轮在 /mnt/d 共享盘生成 Makefile 时出现 0.019 秒及后续依赖文件毫秒级“未来修改时间”告警。虽然测试通过，最终验收改在 Linux /tmp 的独立构建目录重新执行，源码仍取当前工作区，安装包和日志回写 build-reference。原始告警日志保留，不更改系统时钟。

本地记录见 [A10 机读验证](../validation/installed-python-a10-validation-20261012.json)。WSL Linux 结果不是 GitHub Ubuntu runner 结果；macOS 和本阶段远程六组合运行仍待验证。原工作区六份参考脚本草稿被保留，全量 CTest 不等同于纯净提交检出。既有八份未提交文件的哈希在阶段记录中单独核对。本阶段没有新增 SystemVue 分析或发布，整体兼容目标仍未完成。


## 本阶段实际结果（2026-10-12）

| 平台 / 配置 | 全量 CTest | 安装 C/C++ 消费者 | 隔离 wheel | CLI 路径核查 |
|---|---:|---:|---:|---:|
| Windows / Debug | 129/129 | 2/2 | 23 组 379 项、0 跳过 | 64 次 |
| Windows / Release | 129/129 | 2/2 | 23 组 379 项、0 跳过 | 64 次 |
| Ubuntu WSL2 / Debug | 129/129 | 2/2 | 23 组 379 项、0 跳过 | 64 次 |
| Ubuntu WSL2 / Release | 129/129 | 2/2 | 23 组 379 项、0 跳过 | 64 次 |

验证器失败导向自测在 Windows/Linux 各 10/10。最终四份安装报告中的验证器 SHA256 与提交源码一致，所有 worker 的 ABI 为 1、原生库 SHA256 与各自配置的安装库一致。最终 Linux 原生文件系统构建及消费者日志无时间戳告警。原有八份未提交文件 SHA256 未变。
