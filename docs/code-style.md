# C++ 代码风格

代码以便于人工阅读和审查为优先，不把类、函数或多个控制流程压缩到一行。仓库根目录的 `.clang-format` 是统一格式规则：

- 4 空格缩进，不使用制表符，目标行宽 100 列。
- 类定义、函数体和 lambda 体展开为多行，定义之间保留空行。
- if/else 和循环使用花括号，控制语句不与执行体挤在一行。
- 长参数列表换行，保持现有 include 顺序及注释内容。

当前使用 Visual Studio 附带的 clang-format 19.1.5 验证。Windows 下脚本先查 PATH，再查 Visual Studio 安装位置，也可显式传入可执行文件路径。

```powershell
./scripts/format-cpp.ps1
./scripts/format-cpp.ps1 -Check
./scripts/format-cpp.ps1 -Check -ClangFormat 'C:/path/to/clang-format.exe'
```

范围为 include、src、tests 下所有 .hpp 和 .cpp 文件，包括独立安装包消费者。`-Check` 只检查，不改文件；发现格式偏差时以错误结束。其他平台可使用 clang-format 19 按仓库配置处理相同文件。功能改动完成后执行格式检查和相应回归，避免重新出现压缩成单行的实现。
