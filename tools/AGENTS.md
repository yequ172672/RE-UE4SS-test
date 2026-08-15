# Tools Agent 指南

## 责任

本目录承载开发/发布自动化与 xmake 构建实现。脚本可能清理构建目录、生成版本提交或创建发布包；执行前先读参数和写入范围。

## 子目录

| 路径 | 职责 |
| --- | --- |
| `buildscripts/` | Windows/Linux CMake 包装、版本更新与发布打包。 |
| `xmakescripts/` | xmake mode、rules、依赖构建和 CI 查询插件。 |
| `mcp_host/` | 外部 MCP 主机的传输、受限工作区、任务、构建、调试和场景执行模块。 |

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `buildscripts/build.ps1` | Windows configure/build 包装；可选生成器、编译器、模式和 target。 |
| `buildscripts/build.sh` | Linux/交叉编译包装。 |
| `buildscripts/release.py` | 版本提交、发布包、设置改写、资源归档和 release notes。 |
| `mcp_smoke_test.py` | 对运行中的 UE4SS MCP 执行工具发现、日志读取、生命周期及传输失败场景验收。 |
| `ue4ss_mcp_host.py` / `ue4ss_mcp_host.example.json` | 启动 loopback MCP 主机并配置显式工作区、运行时端点和工具链。 |
| `test_mcp_host.py` | 外部主机传输、文件、模组、DAP 和场景回归测试。 |
| `xmakescripts/build_configs.lua` | xmake 三段式 mode 定义。 |
| `xmakescripts/rules/build_rules.lua` | mode 到编译定义/选项的映射。 |
| `xmakescripts/modules/mods/install.lua` | Mod 安装辅助。 |

## 依赖

- 构建脚本调用根 CMake 配置和 `cmake/toolchains/`。
- 发布脚本读取 `assets/`、构建产物及 Git 历史，并写入 `release/` 或版本元数据。
- xmake 脚本同时被仓库自身与把 UE4SS 作为子项目的下游工程消费，不能假设 `os.projectdir()` 总是仓库根。
- MCP 主机只访问配置中的命名根目录，并可选代理游戏内 MCP；CMake、Unreal Editor、UnrealPak 与 DAP 适配器均为显式工具链依赖。

## Common Patterns

- 脚本路径从脚本自身位置解析到项目根；不要依赖调用者当前目录。
- build 目录名称必须包含足够的编译器/toolchain/mode 信息，避免不兼容 cache 混用。
- 发布时的 Shipping 设置与 `assets/UE4SS-settings.ini` 开发默认值不同，改设置键时同步检查 `release.py` 的改写逻辑。
- 发布打包用 `--build-dir` 选择唯一完整构建树；发现同名二进制歧义时必须失败，不能按目录遍历顺序静默选取。
- CMake/xmake 模式、target 名称和产物名属于跨脚本接口；变更必须端到端搜索消费者。
- MCP 文件与模组操作返回哈希和实际变化；写入/删除使用期望哈希或清单检测并发漂移，长任务通过可取消任务与游标事件暴露进度。

## 验证

- 参数解析变化至少运行 `-Help`/`--help`，并用无发布副作用的最小构建路径验证命令拼装。
- 发布逻辑优先在临时输出或可丢弃分支验证；不要在用户工作分支直接运行会创建提交/标签的命令。
- 打包变化核对普通包、zDEV 包、CustomGameConfigs/MapGenBP 附包及 release workflow 的文件名。
- xmake 变化至少运行配置/target 枚举；若本机无 xmake，明确记录未验证。

## For AI Agents

- `--clean` 会递归删除脚本计算出的 build 目录；执行前解析并核对绝对路径。
- `release.py release_commit` 与发布 workflow 具有 Git 写操作，除非用户明确要求发布，否则只读审计。
- 不用脚本内的遗留兼容分支证明当前 README 所声明的工具链仍受支持。
