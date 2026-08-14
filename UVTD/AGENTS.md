# UVTD Agent 指南

## 责任

UVTD 是独立的 `UnrealVTableDumper` 工具，从 PDB 与 JSON 配置生成 VTable、成员变量布局、Sol bindings 和 Unreal wrapper 辅助文件。其输出可能回灌 `assets/`、`UE4SS/` 和 Unreal 子模块，属于代码生成链而非普通日志。

## 子目录

| 路径 | 职责 |
| --- | --- |
| `Config/` | 后缀和待生成对象/PDB 等 JSON 配置。当前仓库只跟踪部分配置。 |
| `include/UVTD/` | 配置、PDB 类型、Dumper/Generator 接口与输出路径。 |
| `src/` | 交互入口、PDB 解析、类型容器和各生成器实现。 |

## 关键文件

| 文件 | 作用 |
| --- | --- |
| `src/main.cpp` | 菜单、配置初始化和任务选择。 |
| `src/UVTD.cpp` | 遍历 PDB、调度各 Dumper/Generator 和共享类型容器。 |
| `src/Config.cpp` | JSON 配置加载与校验。 |
| `include/UVTD/Helpers.hpp` | `UVTD_Generated_Output` 下的目标目录映射。 |
| `src/MemberVarsWrapperGenerator.cpp` | 生成 Unreal wrapper 与 `UE4SS/generated_include/MacroSetter.hpp`。 |
| `CMakeLists.txt` | `UnrealVTableDumper` target，链接 `raw_pdb` 与 glaze。 |

## Dependencies

- 输入依赖 PDB 和完整配置；缺少 `pdbs_to_dump.json` 时当前调度会直接返回。
- 输出会镜像 `deps/first/Unreal`、`assets/VTableLayoutTemplates`、`assets/MemberVarLayoutTemplates` 和 `UE4SS/generated_include` 的结构。
- `README.md` 仍描述旧的 DIA SDK/硬编码流程，与当前 `raw_pdb` + JSON 实现不一致；以源码/CMake 为事实并在相关任务中修正文档。

## Common Patterns

- 先解析所有目标版本到共享 `TypeContainer`，再生成跨版本 wrapper；不要让单个 PDB 的临时状态泄漏到其他版本。
- PDB 名、suffix 配置和生成文件名共同构成版本标识接口。
- 所有生成先留在 `UVTD_Generated_Output`，审查 diff 后再选择性同步到源树。

## 验证

- 构建 `UnrealVTableDumper`；CI 产物路径目前只在 `Game__Shipping__Win64` 收集 UVTD。
- 使用已知 PDB + 完整 Config 运行受影响生成项，比较文件集合、符号/布局内容和确定性。
- 回灌前分别审查 Unreal 子模块、assets 和 UE4SS 的 diff；不要跨仓库批量覆盖。

## For AI Agents

- 不直接把隔离输出全量复制到源树。
- 不手改生成的 `MacroSetter.hpp` 或模板来掩盖 generator 错误。
- 不按旧 README 中不存在的 `UVTD_DIA.cpp` 路径改代码。
- 无 PDB/完整配置时只声称编译或静态验证，不能声称生成链通过。
