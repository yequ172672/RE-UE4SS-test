# 构建与验证

## 当前权威路径

- CMake 是 README、启用中的 PR CI 和发布 workflow 使用的主构建系统。
- xmake 仍是仓库内和下游嵌入场景的兼容构建路径，但相关 GitHub workflow 已禁用。改变公共 mode、target、依赖或安装语义时仍需检查 xmake 是否必须同步。
- 目标平台是 Windows x64。Linux 文档描述的是交叉编译 Windows 二进制，不是 Linux 运行时支持。

## 前置条件

- Visual Studio 2022 17.13+ / MSVC toolset 14.43+，或受支持的 clang-cl 交叉路径。
- CMake 3.22+、Rust 1.73+、Ninja 或 Visual Studio generator。
- `deps/first/Unreal` 和 `deps/first/patternsleuth` 子模块已初始化；SSH 地址可能需要 GitHub/Epic 授权。
- 首次 CMake configure 还可能通过 FetchContent 访问外部仓库。

```powershell
git submodule status
git submodule update --init --recursive
```

不要默认加 `--remote`。

## 构建模式

格式固定为 `<Target>__<Config>__<Platform>`：

- Target：`Game`、`CasePreserving`、`LessEqual421`
- Config：`Debug`、`Dev`、`Shipping`、`Test`
- Platform：`Win64`

不要使用普通的 `Debug`/`Release` 代替项目模式。`Shipping` 是发布配置；`Test` 只是定义了 `UE_BUILD_TEST` 和 `STATS` 的编译配置。

## 推荐局部构建

在已加载 MSVC x64 开发环境、且 `cmake`/`ninja` 在 PATH 的终端：

```powershell
cmake -B build_cmake_msvc_Game__Dev__Win64 `
  -G Ninja `
  -DCMAKE_BUILD_TYPE=Game__Dev__Win64 `
  -DCMAKE_C_COMPILER=cl `
  -DCMAKE_CXX_COMPILER=cl

cmake --build build_cmake_msvc_Game__Dev__Win64 --target UE4SS --verbose
```

其他主要 target：`proxy`、`EventViewerMod`、`KismetDebuggerMod`、`UnrealVTableDumper`。局部验证显式指定 target，避免默认构建全部目标。

也可使用：

```powershell
.\tools\buildscripts\build.ps1 -Generator ninja -BuildConfig Game__Dev__Win64 -Target UE4SS -Verbose
```

包装脚本含若干遗留帮助/回退项；实际工具链支持以 README、`VersionChecks.cmake` 和存在的 toolchain 文件为准。

## 验证等级

| 等级 | 含义 | 可声称的结果 |
| --- | --- | --- |
| L0 静态审查 | 路径、调用方、构建元数据、格式和 diff 检查 | “已静态核对”，不能说已编译。 |
| L1 Configure | 新 build 目录成功生成 | “CMake 配置通过”，不能说目标可链接。 |
| L2 Compile | 指定模式/target 成功构建 | “目标编译通过”，不能说功能运行正确。 |
| L3 Runtime smoke | 在明确的游戏/UE 版本加载并执行代表路径 | “该样本运行验证通过”。 |
| L4 Compatibility | 覆盖贡献指南要求的 4.25 前后两个引擎时代 | 才能对跨版本回归给出较强结论。 |

当前仓库没有 `enable_testing()`/`add_test()`/CTest 单元测试注册，也没有统一 lint/format target。因此不要把 `Game__Test__Win64` 编译称为“自动测试通过”。

## 变更到最低验证

- 纯头/实现局部改动：格式化 + L2 直接 target。
- 公共 UE4SS 头或链接传播：UE4SS + 至少一个 `cppmods` 下游 target。
- mode/编译器/依赖：全新 build 目录 configure + 代表性 MSVC mode；必要时检查 clang-cl。
- Hook/线程/对象生命周期：L2 + 明确的 L3 计划；未实跑时列出游戏内待验证步骤。
- 布局/签名/版本兼容：至少两个引擎时代的 L3/L4；没有样本时停止在“编译验证”。
- 文档/发布：按 `docs-and-release.md`，不以 C++ 构建代替站点或打包检查。

## 当前本机不是仓库事实

环境探测结果会变化，只应在交付中作为当次证据记录。不要把某台机器的已安装路径、版本和 PATH 状态写成长期前置条件。
