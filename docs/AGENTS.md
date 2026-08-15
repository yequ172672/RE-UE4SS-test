# Public Documentation Agent 指南

## 责任

`docs/` 是面向 UE4SS 用户和 Mod 作者的公开 mdBook 源。正文只解释产品行为、接口、步骤、风险和迁移，不出现 Agent 工作流、作者备注、内部验收或制作过程。

## 子目录

| 路径 | 职责 |
| --- | --- |
| `lua-api/` | Lua 类、全局函数、表定义和示例。 |
| `cpp-api/` | C++ 接口说明与示例。 |
| `guides/` | 安装、开发、调试、兼容性和生成器指南。 |
| `feature-overview/` | 用户可感知功能介绍。 |
| `devlogs/` | 技术文章/开发日志。 |

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `SUMMARY.md` | mdBook 导航与章节权威源。 |
| `guides/testing-ai-mcp.md` | AI MCP 本地构建、实机 Smoke、Host、可逆配置与故障排查指南。 |
| `contributing.md` | PR、格式、兼容性验证、Changelog 和 breaking change 规则。 |
| `upgrade-guide.md` | 公开破坏性变更迁移。 |
| `lua-api.md` / `cpp-api.md` | 两类接口入口。 |

## Dependencies

- 根 `README.md` 会被 `docs-export/export.py` 复制为每个版本的首页；首页变化不只影响 GitHub。
- `docs-export/export.py` 从 Git refs 导出 `docs/`，生成被忽略的 `docs-repo/`。
- `docs-repo-template/build.py` 调用 mdBook 构建 release 根站点和版本子目录。
- `.github/workflows/pushdocs.yml` 是完整工具安装与 Pages 部署事实源。

## Common Patterns

- 新增/重命名页面同步 `SUMMARY.md` 和所有引用链接。
- Lua/C++ 接口描述与当前实现、错误模式、线程要求保持一致；不要只复制函数名。
- 破坏性变化在 `upgrade-guide.md` 给出旧/新示例和迁移步骤。
- 版本相关事实说明适用范围，不把一个游戏/引擎样本泛化为所有版本。
- 内部 Agent 文档放 `../agent-docs/`，不加入 `SUMMARY.md`。

## 验证

- 搜索旧接口名与失效链接，检查代码块能否按当前接口使用。
- 完整流程见 `../agent-docs/docs-and-release.md`；具备依赖时运行 export + mdBook build。
- 页面修改后从最终读者视角复读，删除开发者/作者元叙事和内部提示。

## For AI Agents

- 不手改 `docs-repo/`；它会被导出脚本删除并重建。
- 不在公开正文记录本机路径、内部计划、Agent 分工或验收清单。
- 不因为当前 PR CI 忽略 docs 路径就跳过本地链接/站点验证。
