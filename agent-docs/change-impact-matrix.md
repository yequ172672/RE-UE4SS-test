# 变更影响矩阵

修改前选中最接近的行；如果一个改动跨多行，合并所有检查项。

| 改动区域 | 主要消费者 | 必查同步项 | 最低验证 |
| --- | --- | --- | --- |
| `UE4SSProgram` 启动/初始化 | 所有运行时能力 | DllMain 路径、失败/清理、设置、线程顺序 | UE4SS 编译 + 游戏加载 smoke |
| Hook/Unreal 反射 | Lua/C++ Mods、GUI、Dumpers | 注册/卸载、对象失效、版本布局/签名、局部降级 | 受影响模式编译 + 两个 UE 时代运行验证 |
| `LuaType`/`LuaLibrary` | Lua Mod 作者 | Lua 错误语义、game-thread 调度、`docs/lua-api*`、`SUMMARY.md` | 编译 + 代表性 Lua 脚本 |
| `include/Mod`/公共导出 | 外部 C++ Mods、内置 Mods | ABI/回调顺序、版本宏、C++ API、upgrade guide | UE4SS + 内置 Mod 编译；破坏性变更迁移示例 |
| GUI | 调试控制台/LiveView/Mods | 三种 render mode、锁、输入、持久设置 | UE4SS 编译 + 至少一个目标渲染模式运行验证 |
| SDK/USMap/Object Dumper | 生成文件消费者 | 输出格式兼容、路径、公开指南 | 编译 + 小样本输出 diff/解析 |
| UVTD | VTable/成员布局模板 | `UVTD/Config`、assets 模板、下游 Unreal wrapper | `UnrealVTableDumper` 编译 + 已知 PDB 输出对比 |
| `cppmods` | 游戏内 C++ Mod | UE4SS 公共接口、依赖、热路径/卸载 | Mod target 编译 + 游戏加载/卸载 |
| `ProjectConfig.cmake` | 根构建、CI、xmake | CI 单行解析、mode 组合、输出目录、xmake 对应定义 | 新目录 configure + matrix 静态核对 + target 编译 |
| 依赖版本/传播 | 所有下游 target | Gitlink/固定版本、license、PUBLIC/PRIVATE、CMake/xmake | 全新 configure + 直接/间接消费者编译 |
| `assets/` | 运行时与发布包 | 默认/Shipping 设置差异、release.py、Changelog | 格式/解析 + 打包清单或临时试打包 |
| README/`docs/` | 用户与 Mod 作者 | README 会被复制为站点首页、`SUMMARY.md`、版本导出 | 链接检查 + 可用时 mdBook 全流程 |
| workflow/发布脚本 | PR、发布和 Pages | inputs、matrix、target/产物名、secrets 权限 | YAML/参数静态检查 + 非破坏性 dry path |

## 破坏性变更闸门

以下任一成立时，默认按公开破坏性变更审查：

- 删除/重命名 Lua 全局函数、类型成员或 C++ 导出接口。
- 改变参数/返回值、回调顺序、线程要求、错误模式或配置键语义。
- 改变生成文件格式、布局模板含义、Mod 目录结构或发布包路径。
- 移除已支持的构建模式、引擎时代或代理行为。

需要同步 `docs/upgrade-guide.md`、`assets/Changelog.md`、相关 API/指南和可执行迁移示例。不要只保留兼容别名却不声明弃用周期，也不要用无条件兼容层掩盖结构已经无法安全支持的能力。
