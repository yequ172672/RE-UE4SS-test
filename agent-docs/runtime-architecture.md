# 运行时架构与危险接缝

## 主链路

```text
proxy/manual injection
  -> UE4SS.dll DllMain
  -> dll_process_attached
  -> process_initialized
  -> UE4SSProgram constructor
  -> thread_dll_start / UE4SSProgram::init
  -> setup_unreal
  -> setup_unreal_properties
  -> update event loop
  -> on_program_start
  -> GUI / input / Lua Mods / C++ Mods / dumpers
```

这张图只描述当前高层顺序。具体条件、异常路径和版本分支必须回到调用点确认。

## 启动与 loader lock

- `UE4SS/src/main_ue4ss_rewritten.cpp` 是 DLL 入口。代理注入时可能通过主线程 APC 延后初始化，手工注入路径直接进入 `process_initialized`。
- `process_initialized` 构造 `UE4SSProgram`，再创建线程调用 `thread_dll_start`；真正的 `program->init()` 位于 `DllMain` 外。
- `DLL_PROCESS_DETACH` 调用 `UE4SSProgram::static_cleanup()`。改启动时必须同时审查 detach/失败清理，不能只保证成功路径。
- 不在 `DllMain` 添加文件 I/O、同步等待、复杂堆分配、加载 DLL 或依赖其他线程完成的逻辑。

## UE4SSProgram 生命周期

- 构造函数负责路径/设置、日志/控制台、早期 LoadLibrary hooks、Unreal 模块设置和初始 C++ Mod 安装/启动。
- `init()` 负责 `setup_unreal()`、扫描结果/模块报告、C++ Mod Unreal-init 通知、属性设置、Lua 共享函数与事件线程。
- `update()` 运行独立事件循环线程并调用 `on_program_start()`；后者注册 GUI tick、输入、ObjectDumper、热重载并启动 Lua Mods。
- 构造、init、event loop、render/game tick 不是同一线程。新增状态必须明确所有者、跨线程同步和销毁顺序。

## 主要模块与接口

| 模块 | 接口/接缝 | 实现重点 |
| --- | --- | --- |
| 运行时编排 | `UE4SSProgram` | 路径、设置、扫描、事件队列、Mod 和 UI 生命周期。 |
| Unreal 适配 | `deps/first/Unreal` + signatures/layout assets | 多引擎版本反射类型、扫描、Hook 和偏移。 |
| Lua 接口 | `LuaLibrary`、`LuaType/*`、`Mod/LuaMod` | Lua 状态、UE 类型封送、线程调度和错误转换。 |
| C++ Mod 接口 | `Mod/CppUserModBase`、`Mod/CppMod` | DLL 加载、版本信息和生命周期回调。 |
| GUI | `GUI/*` | ExternalThread、EngineTick 或 GameViewportClientTick 三种渲染模式。 |
| 生成器 | `SDKGenerator/*`、`USMapGenerator/*`、`ObjectDumper/*` | 从运行时反射对象生成外部文件。 |
| 代理 | `proxy_generator/` | 从 DLL/exports 清单生成转发定义、汇编和加载代码。 |

## 必守不变量

- Hook 注册、启用、暂停和卸载/失效必须成对；回调不能在依赖对象已经销毁后继续执行。
- 需要游戏线程的 UE 操作必须通过已有 game-thread 调度机制执行，不能因为当前样本“似乎可用”就从 worker/render 线程直调。
- Unreal 对象指针不天然稳定。缓存前确认 GC/销毁语义；跨帧或异步持有要有失效策略。
- Lua 与 C++ 异常不得穿过不支持异常的系统/Hook/DLL 接缝；沿已有 `TRY`、日志和错误对象路径处理。
- GUI 的 EngineTick/GameViewportClientTick 路径运行在游戏相关 tick，ExternalThread 路径另有线程；共享 UI/运行时状态必须审查锁和回调顺序。
- 版本兼容失败应局部降级并记录具体能力；不要用未知版本号作为全局拒绝运行的替代品。

## 修改前追踪清单

1. 谁注册/创建该对象或回调？
2. 谁调用它，在哪个线程，能否重入？
3. 设置、签名、布局或版本宏如何改变该路径？
4. 失败如何报告，是否保持其他能力可用？
5. 谁卸载/清理，游戏退出和热重载是否走同一路径？
6. 公开 Lua/C++ Mod 调用方会观察到哪些行为变化？
