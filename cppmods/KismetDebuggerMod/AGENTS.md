# KismetDebuggerMod Agent 指南

## 责任

KismetDebugger 通过替换 `GNatives_Internal` 项拦截脚本执行，实现断点和单步。它可能阻塞游戏线程并修改全局函数表，是本仓库最高风险的 Mod 区域之一。

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `include/KismetDebugger.hpp` | 断点、opcode、原函数、暂停同步状态与可序列化调试快照。 |
| `include/KismetDebuggerApi.hpp` | 供 UE4SS MCP 动态发现的版本化 C 导出接口。 |
| `src/KismetDebugger.cpp` | native 表替换、命中/暂停/单步、恢复与 UI。 |
| `src/dllmain.cpp` | Mod DLL 导出入口。 |
| `CMakeLists.txt` / `xmake.lua` | 两套 target 定义。 |

## Local Invariants

- 每个替换的 `GNatives_Internal` 条目都必须保存原函数，并在 disable/uninstall/析构路径完整恢复。
- 暂停发生在游戏线程；等待必须可解除，并能处理关闭 UI、禁用 Mod、卸载 DLL 和游戏退出。
- 状态跨 UI/游戏线程共享时使用明确同步；不要依赖固定延迟证明回调已经退出。
- Breakpoint、Step Into/Over/Out 的 opcode/栈语义必须以真实字节码执行路径验证。
- MCP 接口只能通过版本化导出函数访问；所有状态读写复用调试器自身锁与暂停控制，不允许 UE4SS 依赖 Mod 的 C++ 对象布局。

## 验证

- 构建 `KismetDebuggerMod`。
- 游戏内覆盖设置/命中/移除断点、各单步模式、连续运行、disable、卸载和退出。
- 命中暂停时关闭 UI/禁用 Mod，确认游戏线程一定恢复。
- 卸载后核对所有替换条目恢复，且没有回调进入已卸载代码。

## Do Not

- 不增加无界等待或仅靠 `Sleep`/固定毫秒数解决卸载竞态。
- 不在失败路径留下部分替换的 native 表。
- 不以单次断点命中成功代替恢复、卸载和退出验证。
