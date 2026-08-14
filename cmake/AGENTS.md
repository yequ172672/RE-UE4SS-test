# CMake Agent 指南

## 责任

本目录定义 CMake 的项目级配置、编译模式、编译器/架构检测、IDE 组织和跨平台 Windows 交叉编译工具链。根 `CMakeLists.txt` 是调用方，本目录提供其内部构建模块。

## 子目录

| 路径 | 职责 |
| --- | --- |
| `modules/` | 项目配置、通用函数、版本/编译器/架构检查和 target 级 flags。 |
| `modules/CompilerOptions/` | 按编译器族定义编译与链接选项。 |
| `toolchains/` | xwin、Wine/MSVC、clang-cl 的 CMake toolchain 文件。 |

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `modules/ProjectConfig.cmake` | `UE4SS_PROJECTS`、Target/Config/Platform 列表和功能开关；CI 会文本解析部分单行声明。 |
| `modules/Utilities.cmake` | 组合三段式配置及应用编译选项的核心函数。 |
| `modules/CompilerDetection.cmake` | 编译器识别与报告。 |
| `modules/VersionChecks.cmake` | 工具链版本下限。 |
| `modules/ArchitectureDetection.cmake` | 架构能力与 CPU flags。 |
| `modules/IDEOrganization.cmake` | target 设置传播与 IDE 分组。 |

## 依赖

- 被根 `CMakeLists.txt` 通过 `CMAKE_MODULE_PATH` 和 `include(...)` 加载。
- target 定义位于 `deps/`、`cppmods/`、`UE4SS/`、`UVTD/`，不要在此目录硬编码它们的源文件列表。
- xmake 的对应模式位于 `tools/xmakescripts/build_configs.lua` 和 rules 中；改变公共模式语义时检查两套构建路径。

## Common Patterns

- 所有可传播的编译选项应通过 target 级设置应用，避免目录级 `add_compile_options`/`add_link_options` 污染外部消费者。
- 新增模式必须保持 `<Target>__<Config>__<Platform>` 组合规则，并检查 CI matrix 解析、输出目录和 xmake mode。
- `ProjectConfig.cmake` 标为供 CI 解析的 `set(...)` 列表必须保持单行和当前格式。
- 新 toolchain 应只描述工具链差异；项目功能开关留在 `ProjectConfig.cmake`，目标依赖留在目标自身 `CMakeLists.txt`。

## 验证

1. 对受影响模式删除或使用新的专用 build 目录重新 configure，避免旧 cache 给出假阳性。
2. 至少构建一个直接受影响 target；公共 flags/依赖变化至少构建 `UE4SS` 和一个下游 `cppmods` target。
3. 修改模式列表时检查 `.github/workflows/cmake-ci.yml` 的 matrix 计算和 `tools/xmakescripts/` 对应定义。
4. 工具链变化必须在目标宿主验证；未具备 xwin/Wine/MSVC 环境时明确记录未验证项。

## For AI Agents

- CMake configure 通过不代表所有配置成立；模式相关改动要覆盖代表性组合。
- 不通过全局关闭版本检查、警告或异常处理来让单个工具链通过。
- 不手改 build 目录中的 `CMakeCache.txt`、生成项目或 `compile_commands.json`。
