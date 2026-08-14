# RE-UE4SS Agent 开发指南

## 适用范围

本文件约束整个仓库。进入子目录工作时，继续读取离目标文件最近的 `AGENTS.md`；子目录文件只补充局部规则，不重复根规则。发生冲突时，以更具体的目录规则为准，但不得绕过安全、验证和第三方代码约束。

## 快速开始

1. 运行 `git status --short` 与 `git submodule status`，先区分用户改动、未初始化子模块和本次任务范围。
2. 阅读 `README.md`、本文件、目标目录的 `AGENTS.md`，再读目标的 `CMakeLists.txt`/`xmake.lua` 和入口代码。
3. 修改前沿调用链确认接口、线程、生命周期、生成关系和版本分支；不要仅根据文件名猜测。
4. 做最小必要改动，并同步受影响的构建元数据、资源、公开文档、升级说明和变更日志。
5. 至少完成目标级编译或静态验证；需要游戏运行时才能验证的结论必须明确标为未验证。

## 事实源优先级

| 主题 | 首要事实源 | 说明 |
| --- | --- | --- |
| 当前行为与接口 | `UE4SS/`、`UVTD/`、`cppmods/` 源码 | 文档与实现冲突时先确认是否存在文档漂移，不要静默选边。 |
| 构建模式与选项 | `cmake/modules/ProjectConfig.cmake` | CI 会解析其中单行的模式列表，修改格式也可能破坏 CI。 |
| CI 验收 | `.github/workflows/cmake-ci.yml`、`cmake_build_ue4ss.yml` | 启用的 workflow 才是当前自动化事实；`.disabled` 文件仅供历史参考。 |
| 用户与贡献者文档 | `README.md`、`docs/`、`docs/SUMMARY.md` | `docs/` 会进入版本化 mdBook，不放 Agent 内部过程。 |
| 发布内容 | `tools/buildscripts/release.py`、`assets/` | 发布脚本决定复制、改写、压缩哪些文件。 |
| xmake 兼容路径 | 根 `xmake.lua` 与 `tools/xmakescripts/` | CMake 是当前 CI 主路径；改构建逻辑时检查两套定义是否需要同步。 |

## 子目录

| 路径 | 职责 | 局部指南 |
| --- | --- | --- |
| `UE4SS/` | 主运行时 DLL、Lua/C++ Mod 接口、GUI、Dumpers、代理生成 | `UE4SS/AGENTS.md` |
| `UVTD/` | 从 PDB 生成 VTable、成员布局和绑定辅助输出 | `UVTD/AGENTS.md` |
| `cppmods/` | 仓库内置 C++ Mod 示例/工具 | `cppmods/AGENTS.md` |
| `cmake/` | CMake 配置、编译器、架构与工具链规则 | `cmake/AGENTS.md` |
| `tools/` | 构建、发布和 xmake 辅助脚本 | `tools/AGENTS.md` |
| `deps/` | 第一方、第三方和 Git 子模块依赖 | `deps/AGENTS.md` |
| `assets/` | 打包资源、默认配置、签名、布局模板和 Lua Mods | `assets/AGENTS.md` |
| `agent-docs/` | Agent 专用的架构、验证、影响矩阵和文档/发布流程 | `agent-docs/AGENTS.md` |
| `docs/` | 面向用户/Mod 作者的公开文档源 | `docs/AGENTS.md` |
| `docs-export/`、`docs-repo-template/` | 文档导出与站点模板 | 受根规则及 `docs/AGENTS.md` 的跨目录说明约束。 |
| `.github/` | CI、发布、文档部署与贡献模板 | `.github/AGENTS.md` |

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `CMakeLists.txt` | 单仓库 CMake 入口，加载依赖、内置 Mods、UE4SS 与 UVTD。 |
| `xmake.lua` | xmake 入口和全局 mode/平台限制。 |
| `cmake/modules/ProjectConfig.cmake` | 项目列表、三段式构建模式、功能开关及全局定义。 |
| `UE4SS/src/main_ue4ss_rewritten.cpp` | DLL 入口与脱离 loader lock 后的启动线程。 |
| `UE4SS/src/UE4SSProgram.cpp` | 主运行时初始化、扫描、Mod 生命周期和事件循环。 |
| `tools/buildscripts/build.ps1` | Windows CMake 构建包装脚本。 |
| `tools/buildscripts/release.py` | 发布版本、打包内容和发布时设置改写。 |
| `assets/Changelog.md` | PR 要求维护的发布变更日志。 |

## 依赖与构建

- 主目标是 Windows x64、C++23。README 当前要求 CMake >= 3.22、Rust >= 1.73、MSVC toolset >= 14.43 / Visual Studio >= 17.13。
- 首次构建先运行 `git submodule update --init --recursive`。不要加 `--remote`；它会越过仓库锁定的依赖提交。
- 当前 CI 的主路径是 CMake + Ninja。Windows 本地可使用：

```powershell
cmake -B build_cmake_Game__Dev__Win64 -G Ninja -DCMAKE_BUILD_TYPE=Game__Dev__Win64
cmake --build build_cmake_Game__Dev__Win64 --target UE4SS
```

- 也可运行 `./tools/buildscripts/build.ps1 -Generator ninja -BuildConfig Game__Dev__Win64 -Target UE4SS`。脚本仍含 VS2019 回退，而当前 README 的编译器要求更高；不要把成功选中旧生成器误当作兼容性证明。
- 构建模式格式为 `<Target>__<Config>__<Platform>`。当前 Target 为 `Game`、`CasePreserving`、`LessEqual421`；Config 为 `Debug`、`Dev`、`Shipping`、`Test`；Platform 为 `Win64`。
- `Test` 目前是带 `UE_BUILD_TEST`/`STATS` 的编译配置，不等于已有自动测试套件。仓库当前未注册 CTest 测试。
- C++ 文件遵循根 `.clang-format`。只格式化本次触及的 `.cpp`/`.hpp`，避免制造无关大 diff。

## 架构与安全不变量

- `DllMain` 内只做 loader-lock 安全操作；重型初始化必须保持在 `process_initialized`/`thread_dll_start` 之后。
- UE 反射对象、Hook 回调和 GUI/游戏线程有严格生命周期与线程归属。修改前追踪注册、调用、卸载/失效的完整对称路径。
- 版本差异通过构建模式、签名和布局数据局部处理。不要用一个未知版本判断让整个运行时全局拒绝；只禁用无法安全验证的能力。
- 跨 Mod 的稳定接口集中在 `UE4SS/include/` 与导出宏处。改变公开类型、布局、回调顺序或错误语义时，按破坏性变更处理。
- `deps/first/Unreal` 是 Unreal 伪代码/反射基础，不等同普通可自由重构的本仓库实现。

## 修改与验证矩阵

| 改动 | 必查/必同步 | 最低验证 |
| --- | --- | --- |
| 主运行时 C++ | 对应头文件、CMake/xmake 源文件收集、相关设置/资源 | 目标模式编译；涉及 Hook/线程时补运行时验证计划。 |
| Lua 接口 | `UE4SS/src/LuaType`、`LuaLibrary`、`docs/lua-api*`、`docs/SUMMARY.md` | 编译 + 代表性 Lua 调用或明确未完成游戏验证。 |
| C++ Mod 接口 | `UE4SS/include/Mod`、内置 `cppmods/`、C++ API 文档 | UE4SS 与至少一个受影响 Mod 编译。 |
| 构建模式/依赖 | CMake 与 xmake 对应定义、启用的 CI workflow | 重新 configure + 受影响目标编译。 |
| 公开破坏性变更 | `docs/upgrade-guide.md`、`assets/Changelog.md` | 文档链接/示例检查 + 相关构建。 |
| 打包资源 | `tools/buildscripts/release.py`、默认/开发设置差异 | 检查打包清单；必要时在临时输出中试打包。 |
| 文档 | `docs/SUMMARY.md` 与导出脚本影响 | 链接检查；具备 mdBook 环境时执行完整导出构建。 |

贡献指南要求涉及运行时兼容性的 PR 至少覆盖一个 UE 4.11–4.25 版本和一个 4.25 之后（优先 UE 5.1+）版本。无法访问游戏样本时，不得声称完成该层验收。

## Agent 工作规则

- 先理解调用链再改：解释当前逻辑、设计原因、最小改动点、风险和验证，再实现。
- 把用户给出的方案当假设；优先满足目标和验收标准，指出最容易失败的隐含假设。
- 不编辑构建输出、文档导出目录、生成文件或第三方源码来掩盖根因。
- 不覆盖用户已有改动；发现重叠 dirty diff 时先缩小范围，无法隔离再请求用户决定。
- 新增/删除文件时更新所在目录 `AGENTS.md` 的 Key Files；新增/删除目录时更新父级 Subdirectories；依赖或核心模式结构性变化时同步 Dependencies、Common Patterns 或 For AI Agents。
- 上述文档同步只用于结构性变化，不为每个小实现改动制造文档噪声。

<!-- MANUAL -->
## 人工维护区

在此区加入只适用于本地工作流、且不应被自动整理覆盖的约束。
<!-- MANUAL -->
