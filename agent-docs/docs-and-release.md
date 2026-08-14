# 文档与发布流程

## 公共文档数据流

```text
README.md + docs/
  -> docs-export/export.py
  -> ignored docs-repo/versions/{dev,release,v*}/src
  -> docs-repo-template/build.py + mdBook
  -> docs-repo/docs
  -> GitHub Pages
```

- `docs/SUMMARY.md` 是公开导航；新页面不加入这里就不会成为正常 mdBook 章节。
- `README.md` 会复制到每个版本作为首页，并重写 docs.ue4ss.com 的内部链接。
- `docs-export/export.py` 会删除并重建 `docs-repo/`。`docs-repo/` 是忽略的生成物，绝不手改。
- 版本导出读取 `main:docs`、最新 `v*` 和所有版本标签。验证脚本时要考虑没有标签与旧标签文档结构。
- Agent 内部知识只放在根/目录 `AGENTS.md` 与 `agent-docs/`，不加入 `docs/SUMMARY.md`。

## 文档验证

快速静态检查：

- 新增/重命名页面后检查 `docs/SUMMARY.md` 链接。
- 搜索被改接口在 README、Lua/C++ API、指南和 upgrade guide 中的所有引用。
- 公开正文只面向用户/Mod 作者，不包含 Agent 路由、作者备注、制作流程或内部验收语言。

完整构建需要 Python、mdBook、`mdbook-mermaid`、`mdbook-alerts`，步骤以 `.github/workflows/pushdocs.yml` 为准：

```powershell
python docs-export/export.py
python docs-repo/build.py
```

第一条命令会重建 `docs-repo/`；执行前确认该目录没有需要保留的非生成内容。

## 发布数据流

- `tools/buildscripts/release.py` 读取构建产物与 `assets/`，生成普通包、zDEV 包和附加资源压缩包。
- `assets/UE4SS-settings.ini` 是开发默认设置；发布脚本会为 Shipping 包改写部分设置。
- `assets/Changelog.md` 是贡献指南/PR 模板要求维护的发布记录。
- `.github/workflows/cmake-release.yml` 负责编排版本提交、构建、打包、推送标签和创建 release。

## 发布安全

- 日常验证不运行 `release_commit`、推送、打标签或创建 release。
- 设置键、产物名、资源目录或压缩包名变化必须从构建 target 一直追踪到收集、打包和 workflow 上传。
- 需要试打包时使用可丢弃输出，检查文件清单与设置差异；不得把“脚本执行结束”当作产物正确。
- action/runner/外部工具版本可能变化，修改 workflow 前用官方文档核实，不凭旧记忆升级。
