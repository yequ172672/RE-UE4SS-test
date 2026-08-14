# EventViewerMod Agent 指南

## 责任

EventViewer 同时 Hook `ProcessEvent`、`ProcessInternal`、`ProcessLocalScriptFunction`，在任意调用线程产生轻量记录，由 ImGui 客户端消费、过滤和持久化。高层使用说明与当前模块图见 `README.md`。

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `include/Middleware.hpp` / `src/Middleware.cpp` | Hook 所有权、线程本地生产 token、深度和无锁队列。 |
| `include/Client.hpp` / `src/Client.cpp` | ImGui 状态、出队、线程视图、过滤和保存。 |
| `include/StringPool.hpp` / `src/StringPool.cpp` | 原始/小写字符串驻留及稳定 `string_view`。 |
| `include/Structs.hpp` / `src/Structs.cpp` | 采集记录与视图数据结构。 |
| `include/EntryCallStackRenderer.hpp` | 根调用链 modal 行为。 |
| `CMakeLists.txt` | `concurrentqueue` 固定提交与 Mod target。 |

## Local Invariants

- 三类 Hook 共用一套深度语义；过滤只隐藏显示项，不能重算原始深度。
- 所有 Hook 安装完成前不入队，避免捕获半初始化链。
- producer 路径必须保持轻量；UI、聚合、过滤和文件输出留在 consumer。
- `StringPool` 返回的 view 只在池不清空时稳定；若新增 clear，必须同时失效所有线程/视图缓存。
- Start/Stop、Pause、Enable 的语义不同，改状态机时同步 UI、捕获和出队行为。

## 验证

- 构建 `EventViewerMod`。
- 游戏内覆盖递归/嵌套三 Hook 调用、多个线程、开始/停止/暂停、白名单/黑名单、Stack/Frequency、保存和卸载。
- 高调用量下观察掉帧、队列增长、锁竞争和深度错乱；普通空闲场景不能代表热路径安全。

## Do Not

- 不在 Hook 回调内渲染 ImGui、写文件或执行昂贵字符串转换。
- 不因视图过滤修改原始捕获深度。
- 不清空字符串池而保留任何 `string_view` 消费者。
