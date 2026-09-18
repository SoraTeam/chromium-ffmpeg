# Windows x86 build

唯一预设为 **`windows-x86-clang13`**：CMake + Ninja、Chromium 官方 Clang 13
指定提交、Windows x86（32 位），使用包含 H.264/AAC 的 `Chrome` 配置，
始终启用 `CHROMIUM_NO_LOGGING` 和 ThinLTO。不需要 PowerShell 构建脚本。

## 首次构建

安装 CMake 3.30 或更新版本和 Ninja，将它们加入 PATH。
通过 VS 2019 Installer 安装以下组件，作为 Windows 头文件和库：

- `Microsoft.VisualStudio.Component.VC.14.26.x86.x64`
- `Microsoft.VisualStudio.Component.Windows10SDK.19041`

在 CMD 中初始化环境，然后执行 workflow（按实际安装位置修改 VS 路径）：

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 -vcvars_ver=14.26.28801 -winsdk=10.0.19041.0
cmake --workflow --preset windows-x86-clang13
```

CMake 自动配置、编译、测试和安装；任一步失败就停止。重复同一命令即可增量构建。
这里使用最小组件安装提供的 `VsDevCmd.bat`，不依赖默认工具集的 `vcvarsall.bat` 包装脚本。
宿主机使用 64 位 Windows，32 位测试在 WoW64 下运行。

无需安装系统 LLVM：toolchain 自动下载并校验 Chromium 官方 Windows 包
`llvmorg-13-init-3462-gfe5c2c3c-2`，检查 clang-cl 和 lld-link 的完整 revision
均为 `fe5c2c3ca682b140dd5e640e75948363b6b25ef9`。工具缓存位于 `out/toolchains/`。
配置要求 MSVC 目录版本 `14.26.28801` 和 SDK `10.0.19041.0`，不匹配直接失败。
头文件与库路径写入构建命令，防止混入本机其他版本。

GN、NASM 2.16.03、Opus 1.6.1 同样固定版本并校验 SHA-256，缓存位于
`out/windows-x86-clang13/_deps/`。不需要完整 Chromium checkout、depot_tools、
MSYS2 或 Python。

产物位于 `out/package/windows-x86-clang13/`：

- `ffmpeg.dll`：静态 CRT 和静态 Opus，不附带 Opus/VC runtime DLL。
- `ffmpeg.lib`、`ffmpeg.dll.pdb`：导入库和匹配的调试符号。
- `ffmpeg.def`、`licenses/`：导出清单和许可文本。
- `build-info.txt`、`SHA256SUMS`：源码/工具链信息、sysroot 来源及产物校验值。

也可以在相同开发环境中单独执行各阶段：

```console
cmake --preset windows-x86-clang13
cmake --build --preset windows-x86-clang13
ctest --preset windows-x86-clang13
cmake --install out/windows-x86-clang13
```

CI 仅运行这个 preset，先安装固定 bootstrapper/channel 中的 VS 2019 组件，
再执行同一 CMake workflow。成功后上传产物和符号，失败时上传诊断日志。

## 复现范围

构建检查与已提供原版 DLL 一致的 PE 字段：x86、Linker 14.0、OS/subsystem 5.1、
console subsystem、CFG、Large Address Aware 和短 PDB 名。
**PE 里的 5.1 只是头字段，不代表在 Windows XP 上经过运行验证。**

复现范围有三个明确边界：

- 仓库 `config.h` 的 `CC_IDENT` 记录的是**生成配置时**的编译器。
  [Electron 13.0.0 的 DEPS](https://github.com/electron/electron/blob/v13.0.0/DEPS)
  指向 Chromium 91.0.4472.69，而该版本的
  [Clang 构建 pin](https://chromium.googlesource.com/chromium/src/+/91.0.4472.69/tools/clang/scripts/update.py)
  是 `llvmorg-13-init-6429-g0e92cbd6-2`。不能从配置头或 PE Linker 字段证明
  提供的 DLL 最终使用的是 `fe5c2c3c`；本 preset 明确复现的是配置头里的 revision。
- [Chromium 的原 sysroot](https://chromium.googlesource.com/chromium/src/+/91.0.4472.69/build/vs_toolchain.py)
  是 `20d5f2553f`，公开下载目前返回 403。这里使用 Microsoft 公开的 VS 2019
  组件重建同代 sysroot，并非逐文件复制 Google 的归档。目前公开包包含
  14.26.28808 的头文件/工具及 14.26.28805 的 CRT，安装目录仍叫 14.26.28801；
  `build-info.txt` 记录来源和关键头文件/库的 SHA-256，避免混淆目录版本与文件内容。
- 仍沿用本独立构建的 Opus 1.6.1、NASM 2.16.03、CMake 编译参数和 ThinLTO。
  复刻原 DLL 的每个字节还需要最终 Chromium/Electron 构建参数、全部依赖提交、
  原始 sysroot，以及时间戳/PDB 等构建输入。本配置不声称与原版 DLL 二进制相同。

## 构建契约与维护

- 配置直接使用 `chromium/config/<branding>/win/ia32`；源码清单直接使用
  `ffmpeg_generated.gni`。`cmake/ChromiumSources.cmake` 在构建目录建立最小 GN
  环境，让 **GN 自己**解释未经修改的源码清单，输出 JSON 供 CMake 使用。
  两个替代 import 只提供 Windows 的架构和 branding 参数，不运行 Chromium
  整个构建系统，也没有第二份手写源码列表或自制 GN 解析器。
- Windows 导出表由 `chromium/ffmpeg.sigs` 生成，与仓库原 `BUILD.gn` 的组件库
  约定一致；不会导出 FFmpeg 的所有内部函数。该 DLL 只面向匹配此版本头文件及
  ABI 的使用方，不保证替换任意版本 Chromium 的 DLL。
- 所有 FFmpeg C 编译单元始终定义官方 `CHROMIUM_NO_LOGGING`，使用 `/O2` 和
  `-flto=thin`；Opus 同样使用 ThinLTO，最终由 LLD 链接。NASM 保留原生汇编优化。
  没有自制日志空实现、未解析符号忽略或关闭汇编来绕过链接错误。
- CTest 检查完整导出集合、日志入口缺失、branding、内存 WAV 解封装及 PCM 精确解码、
  Opus 解码，以及 FFmpeg/Opus C 对象的 LLVM bitcode 标识。使用运行时检查，
  不会被 Release 的 `NDEBUG` 去除。
- 上游更新后运行 `windows-x86-clang13` workflow。源文件、配置、签名变更会触发重新配置/编译。
  更新依赖时同时修改 URL 和 SHA-256，再运行相同测试。断网构建可通过 CMake 的
  `FETCHCONTENT_SOURCE_DIR_GN`、`FETCHCONTENT_SOURCE_DIR_NASM`、
  `FETCHCONTENT_SOURCE_DIR_OPUS` 指定预先校验并解压的对应版本目录。
- CI 使用相同 CMake workflow，PR/push 只构建并测试 `windows-x86-clang13`，成功后上传 DLL、导入库、PDB
  和许可文件；不自动提交、打标签或发布 Release。

范围仅限 Windows x86 Release；测试不是完整媒体兼容性或恶意媒体安全审计。
本仓库源码基于 2021 年 Chromium FFmpeg，现代编译工具不会更新其编解码器实现。

工具链参考：[CMake Presets / workflow](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)、
[CMake linker selection](https://cmake.org/cmake/help/latest/variable/CMAKE_LINKER_TYPE.html)、
[GN 固定版本](https://chrome-infra-packages.appspot.com/p/gn/gn/windows-amd64/+/5wPLBOHcmFQPLf_10R6kDW0yLN2GCkTUF8B2QyEHFyAC)、
[Opus 上游下载与校验值](https://www.opus-codec.org/downloads/)。
