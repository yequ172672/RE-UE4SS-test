# C++ Mods Agent 指南

## 责任

本目录包含随仓库构建的 C++ Mods。它们是 UE4SS 公共接口的真实下游，也是游戏进程内代码：崩溃、阻塞、Hook 泄漏会直接影响宿主游戏。

## 子目录

| 路径 | 职责 | 局部指南 |
| --- | --- | --- |
| `EventViewerMod/` | 多 Hook 的高频调用采集与 ImGui 查看器 | `EventViewerMod/AGENTS.md` |
| `KismetDebuggerMod/` | Kismet 断点/单步调试 | `KismetDebuggerMod/AGENTS.md` |
| `MeshTreeMod/` | 主角组件树覆层 UI（Hide/Show 控制，配合 `[Debug] OverlayMode`） | — |

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `CMakeLists.txt` | 注册当前三个 Mod 子目录。 |
| `xmake.lua` | xmake Mod 入口；当前只包含 KismetDebugger，存在已知构建不对称。 |
| `<Mod>/src/dllmain.cpp` | Mod DLL 导出入口。 |
| `<Mod>/CMakeLists.txt` | shared library target、UE4SS/ImGui 及 Mod 私有依赖。 |

## Dependencies

- 所有 Mod 链接 `UE4SS` 公共接口，通常也使用 Unreal 与 ImGui。
- UE4SS 通过导出符号加载/卸载 C++ Mod；改变入口名、生命周期回调或公共类型时属于兼容性改动。
- CI 会发现 `cppmods/` 下带 `CMakeLists.txt` 的直接子目录并尝试构建，但根 `cppmods/CMakeLists.txt` 仍需显式注册。

## Common Patterns

- `dllmain.cpp` 只负责导出和实例化，行为留在 Mod 类/深模块内。
- Hook/全局表替换必须保存原状态并在 disable/uninstall/析构路径恢复。
- 高频回调内避免分配、格式化、锁竞争和 UI 工作；用队列/池把展示移到消费侧。
- 新 Mod 同步 CMake 注册；若声明支持 xmake，再同步 `cppmods/xmake.lua` 和依赖规则。允许不对称时写明原因。

## 验证

- 至少构建具体 Mod target；公共接口变化同时构建 UE4SS 和两个现有 Mods。
- 游戏内覆盖 load、enable、核心动作、disable/uninstall、热重载（若支持）和退出。
- 对 Hook Mod 额外验证多线程、高频场景及卸载后不再收到回调。

## For AI Agents

- 不把“DLL 成功加载”当作 Mod 功能验收。
- 不在游戏线程做无界等待或每帧重型处理。
- 不新增全局 Hook 而没有所有者、原函数保存和恢复路径。
