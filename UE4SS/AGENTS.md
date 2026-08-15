# UE4SS Runtime Agent 指南

## 责任

本目录实现注入游戏进程的 `UE4SS.dll`、对 Lua/C++ Mods 的公共接口、调试 GUI、运行时 Dumpers/Generators 与代理生成器。修改生命周期或公共头前先读 `../agent-docs/runtime-architecture.md`。

## 子目录

| 路径 | 职责 |
| --- | --- |
| `include/` | 公共/内部头；与 `src/` 多数功能区镜像。 |
| `src/` | 主运行时实现、GUI、Lua 类型桥、Mod、Dumpers 和 Generators。 |
| `proxy_generator/` | 代理导出扫描、代码生成与 proxy DLL；见局部 `AGENTS.md`。 |
| `generated_include/` | 受控生成头；`MacroSetter.hpp` 由 UVTD 生成。 |
| `generated_src/` | 版本缓存等发布/构建输入。 |

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `src/main_ue4ss_rewritten.cpp` | `DllMain`、APC/手工注入分支、启动线程和静态清理。 |
| `include/UE4SSProgram.hpp` / `src/UE4SSProgram.cpp` | 路径/设置、扫描、事件循环、Mod 和 GUI 生命周期核心。 |
| `include/UE4SSRuntime.hpp` / `src/UE4SSRuntime.cpp` | 向 Mods/Lua 报告部分 Hook 能力。 |
| `include/SettingsManager.hpp` / `src/SettingsManager.cpp` | `UE4SS-settings.ini` 的运行时映射。 |
| `include/McpServer.hpp` / `src/McpServer.cpp` | 本地 MCP Streamable HTTP 传输、结构化结果与运行时线程调度。 |
| `include/Signatures.hpp` / `src/Signatures.cpp` | 扫描签名和 Lua 扫描覆盖。 |
| `include/Mod/CppUserModBase.hpp` | 外部 C++ Mod 的关键公共接口。 |
| `CMakeLists.txt` / `xmake.lua` | UE4SS target、公共 include/definitions、依赖和版本信息。 |

## Dependencies

- 公共依赖包含 `deps/first/Unreal`、LuaMadeSimple、Input、scanner、patternsleuth 绑定及 GUI/Hook 第三方库。
- MCP JSON 传输使用 Glaze；`deps/third/CMakeLists.txt` 的 CMake 版本与 `UE4SS/xmake.lua` 的 xmake 版本必须保持一致。
- `UE4SS/include/` 通过 CMake `FILE_SET` 与 `PUBLIC` include 暴露给下游 Mods；不要把内部实现细节无意变成公共接口。
- `generated_src/version.cache` 由版本工具/发布流程维护；版本定义会 `PUBLIC` 传播给下游。

## Common Patterns

- `include/<Area>` 与 `src/<Area>` 是同一个模块的接口与实现，搜索和修改必须成对检查。
- `UE4SSProgram` 构造函数已有大量副作用；初始化顺序分析必须覆盖 constructor、`init()`、`update()`、`on_program_start()` 和 cleanup。
- 新 Lua 类型按现有注册/封送/error 模式实现，并同步公开 Lua 文档。
- Hook 回调必须明确线程、可重入性、所有者和卸载/热重载失效路径。
- 兼容性检查应产出能力级可用/不可用结果，不把单个签名、偏移或 Hook 失败扩大为全局拒绝。
- 自动游戏兼容配置只覆盖声明的设置键；本地 MCP、GUI、热键和 Mod 配置保留在安装根设置中，布局/签名按选中目录局部覆盖。
- MCP 传输线程不得直接访问 UObject 或模组 Lua 状态；运行时命令分别调度到 UE4SS event loop 或有预算的 EngineTick game-thread 队列。
- MCP 超时必须区分“尚未执行且已取消”和“已经执行、结果不确定”；非幂等操作不能在超时后静默迟到执行。

## 验证

- 实现改动至少构建 `UE4SS`；公共头/导出变化再构建 `EventViewerMod` 与 `KismetDebuggerMod`。
- DllMain/代理/扫描/线程/Hook 变化必须有游戏内 smoke 计划，纯编译不够。
- 版本适配至少覆盖受影响的 `Game`、`LessEqual421`、`CasePreserving` 代表模式，并按贡献指南安排两个 UE 时代运行验证。
- 生成器变化用小样本比较输出，避免仅检查“生成了文件”。

## For AI Agents

- 不在 `DllMain` 增加重型逻辑或同步等待。
- 不手改 `generated_include/MacroSetter.hpp`；从 UVTD 的生成实现和隔离输出更新。
- 不直接改 `generated_src/version.cache` 完成普通功能任务。
- 不把裸 UE 对象指针跨线程/跨帧缓存而没有失效策略。
- 不改变公共类型布局、导出函数或生命周期回调而漏掉 C++ Mod 兼容性与 upgrade guide。
