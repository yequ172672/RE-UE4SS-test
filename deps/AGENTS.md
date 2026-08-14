# Dependencies Agent 指南

## 责任

本目录汇集 UE4SS 的第一方基础库、第三方库与构建适配。默认把依赖视为外部接口，不在修复主项目问题时顺手重构依赖实现。

## 子目录

| 路径 | 职责 | 修改策略 |
| --- | --- | --- |
| `first/` | UE4SS 组织内基础库，以及 `Unreal`、`patternsleuth` 子模块 | 见 `first/AGENTS.md`；子模块改动必须单独说明提交来源。 |
| `third/` | 随仓库构建的第三方库适配/源码 | 仅在明确的依赖任务中修改，保留上游可追溯性。 |
| `third-repo/` | 第三方包/仓库获取配置 | 与 `third/` 和构建入口一起审查。 |
| `fonts/` | 字体依赖资源 | 保留许可证与生成来源。 |

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `CMakeLists.txt` | 加载 `third` 后加载 `first`。 |
| `xmake.lua` | xmake 依赖入口。 |
| `first/Unreal` | Git 子模块；UE 伪代码、反射类型和版本差异的核心来源。 |
| `first/patternsleuth` | Git 子模块；签名/模式搜索能力。 |

## 依赖规则

- 子模块固定提交记录在主仓库 Git tree 与 `.gitmodules`；初始化用 `git submodule update --init --recursive`，不要默认使用 `--remote`。
- 修改子模块前先确认任务是否真的要求修改上游仓库。主仓库只记录 gitlink，工作树内未提交修改不会自动成为可复现变更。
- 新增依赖同时更新 CMake 和需要支持的 xmake 路径，确认 license、版本固定方式、Windows x64/C++23 兼容性和发布分发要求。
- 避免把第三方 include/flags 以全局方式泄漏给所有 target；优先通过具体 target 的 `PRIVATE`/`PUBLIC` 链接关系传播。

## 验证

- 先用 `git submodule status` 区分 `-`（未初始化）、`+`（提交不匹配）和 dirty 状态。
- 依赖版本变化必须从全新 build 目录 configure，并构建直接使用者与至少一个下游 target。
- 头文件或编译定义传播变化要验证 UE4SS 作为下游 Mod 公共依赖时的场景。

## For AI Agents

- 不为修复主项目编译错误直接改 vendored 源码；先判断根因是调用方式、版本锁定还是构建适配。
- 不删除依赖目录来“修复”未初始化子模块。
- 不把子模块 SSH 访问失败误判为源代码缺失；记录认证/授权阻塞并停止需要联网的步骤。
