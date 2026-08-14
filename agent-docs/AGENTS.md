# Internal Agent Docs 指南

## Scope

本目录只存放跨模块、面向开发 Agent 的内部知识。公共用户文档属于 `../docs/`；本目录不得加入 `../docs/SUMMARY.md`。

## Key Files

| 文件 | 作用 |
| --- | --- |
| `README.md` | 文档层级、主题导航和维护原则。 |
| `runtime-architecture.md` | DLL 启动、运行时模块、线程/Hook/生命周期不变量。 |
| `build-and-validation.md` | CMake 主路径、构建模式、target 与验证等级。 |
| `change-impact-matrix.md` | 代码区域到消费者、同步项和最低验证的映射。 |
| `docs-and-release.md` | README/docs 导出、mdBook、Changelog 和发布安全。 |
| `ue4ss-ai-mcp-requirements.md` | UE4SS AI MCP 的开发能力、功能范围、错误语义、验收标准与发布审计门槛。 |

## Common Patterns

- 主题文档只记录跨目录且相对稳定的知识；模块局部内容放最近的目录级 `AGENTS.md`。
- 命令必须标明前置条件、写入范围和能证明的验证等级。
- 代码事实变化时同步本目录；一次性调试记录、个人环境和未确认推断不沉淀为长期规则。
- 新增主题前先确认现有文档无法自然承载，避免同一事实多处复制。

## For AI Agents

- 修改 Key Files 或新增/删除主题时同步本表和 `README.md` 导航。
- 不把这里的内部过程复制到面向最终用户的公开正文。
- 引用源码事实时重新核对当前工作树，尤其是 CI、依赖和版本信息。
