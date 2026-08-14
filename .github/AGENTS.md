# GitHub 自动化 Agent 指南

## 责任

本目录定义 PR/Issue 输入模板、构建矩阵、发布流程、文档部署和子模块更新自动化。

## 子目录

| 路径 | 职责 |
| --- | --- |
| `workflows/` | GitHub Actions；只有扩展名为 `.yml` 且启用触发器的文件属于当前自动化。 |
| `ISSUE_TEMPLATE/` | Bug、文档、功能和升级问题模板。 |

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `workflows/cmake-ci.yml` | PR 构建矩阵与路径过滤。 |
| `workflows/cmake_build_ue4ss.yml` | 可复用 Windows CMake 构建、target 选择与产物收集。 |
| `workflows/cmake-release.yml` | 稳定发布编排。 |
| `workflows/cmake-experimental.yml` | 实验发布/构建路径。 |
| `workflows/pushdocs.yml` | mdBook 依赖安装、文档导出和 Pages 部署。 |
| `pull_request_template.md` | PR 验证与变更日志清单。 |

## Dependencies

- CI matrix 读取 `cmake/modules/ProjectConfig.cmake` 的模式列表。
- 构建 workflow 依赖 CMake target 名称、build 目录布局和产物名。
- 发布 workflow 调用 `tools/buildscripts/release.py` 并消费其压缩包命名。
- 文档 workflow 调用 `docs-export/export.py`、生成的 `docs-repo/` 与 `docs-repo-template/build.py`。

## Common Patterns

- 可复用 workflow 的 input 是调用方接口；新增/重命名 input 时同步所有 `uses: ./.github/workflows/...` 调用点。
- 构建矩阵、target 发现、产物收集是同一条链，不能只改其中一步。
- `.disabled` workflow 不代表当前验收要求；只在迁移或恢复该流程时参考。
- action 版本和 runner 变更属于可能随时间漂移的信息，修改前核对官方当前支持状态。

## 验证

- 用 YAML 解析器检查语法，并逐个搜索 reusable workflow input/output 消费方。
- 改 matrix 时把计算结果与 `ProjectConfig.cmake` 当前列表对照。
- 改产物规则时同时验证 target 是否构建、文件是否收集、required-files 检查和发布下载名。
- 无法本地复现 GitHub runner 时，明确区分静态验证与 Actions 实跑。

## For AI Agents

- 不通过降低 required-files、跳过失败或扩大 secrets 权限来掩盖构建/发布根因。
- 不运行或推送发布流程，除非用户明确授权创建版本、标签或 release。
- 变更 `paths-ignore` 前评估是否会让应验证的源码/构建元数据绕过 CI。
