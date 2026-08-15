# Assets Agent 指南

## 责任

本目录是运行时默认数据与发布包素材的权威源，包括设置、变更日志、签名、兼容配置、布局模板、Lua Mods 和附加下载内容。数据改动可能与 C++ 改动同样影响跨版本安全。

## 子目录

| 路径 | 职责 |
| --- | --- |
| `CustomGameConfigs/` | 特定游戏覆盖；发布为独立压缩包。 |
| `CustomGameConfigs/NINJA GAIDEN 2 BLACK/` | NG2B UE 5.4 compatibility selector and overrides. |
| `UE4SS_Signatures/` | 扫描脚本示例/覆盖。 |
| `VTableLayoutTemplates/` | 各 UE 版本/CasePreserving VTable 模板，可由 UVTD 生成。 |
| `MemberVarLayoutTemplates/` | 各 UE 版本成员布局模板，可由 UVTD 生成。 |
| `Default_UVTD_Configs/` | UVTD 默认生成配置。 |
| `Mods/` | 随包提供的 Lua Mods 和共享代码。 |
| `MapGenBP/` | 单独发布的 Blueprint 资源。 |

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `UE4SS-settings.ini` | 开发/默认运行设置；发布脚本会改写 Shipping 差异。 |
| `Changelog.md` | 发布记录与 PR 必查项。 |
| `MemberVarLayoutTemplates/MemberVariableLayout_*.ini` | 版本化成员偏移。 |
| `VTableLayoutTemplates/VTableLayout_*.ini` | 版本化虚函数顺序。 |

## Dependencies

- `tools/buildscripts/release.py` 复制/改写本目录并生成普通、zDEV 与附加资源包。
- UE4SS 启动/扫描读取设置、签名和布局数据。
- UVTD 可在隔离输出中生成布局模板，再经人工审查回灌。

## Common Patterns

- 特定游戏修复放在最窄的 CustomGameConfig/签名/布局层，不修改全局默认来迁就单个样本。
- 新版本模板保持命名、section/order 和 CasePreserving 变体的一致性。
- 设置键变更必须同步 SettingsManager、开发默认值、发布时改写和用户文档。
- Changelog 面向用户描述结果，不写内部 Agent 过程。

## 验证

- 使用实际解析器/运行时加载受影响 INI/JSON/Lua；纯文本可读不等于语义有效。
- 模板/签名变化在目标 UE/游戏样本验证，并确认失败只局部降级。
- 发布资源变化核对 `release.py` 清单及压缩包内容。

## For AI Agents

- 不手改生成模板来掩盖 UVTD 根因；保留生成证据并审查 diff。
- 不把单一游戏的偏移或签名提升为全局默认。
- 不在配置、日志示例或文档中写入游戏路径、token 或其他本机敏感信息。
