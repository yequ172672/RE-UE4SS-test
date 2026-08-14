# Proxy Generator Agent 指南

## 责任

本目录包含 `proxy_generator` 可执行程序和 `proxy` DLL target。生成器从真实 DLL 或受控 `.exports` 快照生成 `.def`、汇编跳板和加载代码；proxy DLL 保留原 DLL 导出并引导 UE4SS。

## 子目录

| 路径 | 职责 |
| --- | --- |
| `exports/` | 跟踪的导出名/ordinal 快照，当前包含 dwmapi 与 xinput1_3。 |
| `proxy/` | proxy target、资源文件与 CMake/xmake 生成规则。 |

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `main.cpp` | 解析 PE 导出并生成 `.def`、`.asm`、`dllmain.cpp`。 |
| `CMakeLists.txt` | `proxy_generator` target。 |
| `proxy/CMakeLists.txt` | 选择真实/回退 exports、执行生成器并构建 proxy。 |
| `proxy/xmake.lua` | xmake 对应生成/构建路径。 |
| `exports/*.exports` | 名称/ordinal 接口快照，不是随意编辑的普通列表。 |

## Common Patterns

- 默认代理以系统 `dwmapi.dll` 为基准，可通过 `UE4SS_PROXY_PATH` 指定其他 DLL。
- 名称导出与仅 ordinal 导出都必须保持；转发数组索引、`.def` ordinal、汇编跳板和 `GetProcAddress` 查询是一条原子生成链。
- 构建目录生成的 `dllmain.cpp`、`.asm`、`.def` 是输出，不是编辑源。
- proxy 自身的 `DllMain` 同样受 loader lock 约束，加载 UE4SS 的顺序不能凭单个游戏样本简化。

## 验证

1. 构建 `proxy_generator` 和 `proxy`。
2. 对生成前后的名称、ordinal、导出数量和原 DLL逐项比较。
3. 检查找不到原 DLL/UE4SS、相对路径和 `--ue4ss-path` 场景的失败行为。
4. 最终在目标游戏中做启动 smoke；仅成功产出 DLL 不能证明转发正确。

## For AI Agents

- 不手工修 build 目录生成代码来绕过生成器缺陷。
- 修改 `.exports` 前保留来源和可复现的导出核对证据。
- 不删除“看起来未使用”的 ordinal 跳板；它可能是原 DLL ABI 的唯一入口。
