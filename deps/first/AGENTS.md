# First-party Dependencies Agent 指南

## 责任

本目录是 UE4SS 的共享基础库层。它既包含本仓库维护的 C++ 库和 Rust FFI 壳，也包含两个独立 Git 子模块；不能把整个目录统一当作 vendor 或普通业务代码。

## 子目录

| 类别 | 路径 | 职责 |
| --- | --- | --- |
| 系统/数据基础 | `File`、`String`、`Helpers`、`Constructs`、`ArgsParser` | 文件、字符串、通用构造和参数解析。 |
| 运行基础 | `DynamicOutput`、`MProgram`、`Input`、`Function`、`ASMHelper` | 日志、程序壳、输入、函数与汇编辅助。 |
| 解析/脚本 | `IniParser`、`JSON`、`ParserBase`、`LuaRaw`、`LuaMadeSimple` | 配置解析与 Lua C++ 封装。 |
| 扫描/性能 | `SinglePassSigScanner`、`Profiler`、`ScopedTimer` | 签名扫描、性能统计与计时。 |
| Unreal 适配 | `Unreal` | Git 子模块；UE 伪代码、反射、Hook 和版本适配。 |
| PatternSleuth | `patternsleuth`、`patternsleuth_bind` | Rust 子模块及本仓库 C++/Corrosion 绑定。 |

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `CMakeLists.txt` | 加载第一方库、子模块和 Rust 绑定。 |
| `xmake.lua` | xmake 第一方依赖图与 msvc-wine 特殊路径。 |
| `patternsleuth_bind/Cargo.toml` | Rust FFI crate 与子模块 crate 依赖。 |

## Dependencies

- 多数库通过 target 级 `PUBLIC` 链接成为 UE4SS 和外部 C++ Mod 的可见接口；改头文件、宏或链接可见性时扩大验证范围。
- `patternsleuth_bind` 的 CMake 路径由 Corrosion 导入 Rust crate，xmake 有独立 cargo 规则。
- `Unreal` 与 `patternsleuth` 是 gitlink；父仓库不能直接提交它们内部的普通文件改动。

## Common Patterns

- 保持依赖方向从通用基础库指向 Unreal/UE4SS 高层，避免基础库反向 include `UE4SS/`。
- 静态/动态构建宏是库接口的一部分；CMake 与 xmake 的宏名/可见性必须按实际消费者核对。
- 公共头变化优先通过直接库 target 验证，再构建 UE4SS、UVTD 或内置 Mods 中的代表性下游。

## For AI Agents

- 当前子模块未初始化时，只能审计 gitlink 和父仓构建适配，不能声称已核实子模块内部实现。
- 不在父仓任务中给 `Unreal`/`patternsleuth` 目录补丁；需要上游改动时单独说明仓库、提交和回灌 gitlink 的流程。
- 不用降低警告、关闭版本检查或扩大全局 include 路径解决单个基础库的接口问题。
