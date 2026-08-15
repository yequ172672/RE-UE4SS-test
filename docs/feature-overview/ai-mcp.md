# AI MCP

UE4SS provides two local Model Context Protocol (MCP) Streamable HTTP endpoints:

- The embedded runtime endpoint runs inside the game process. It exposes the current UE4SS session, reflection search and watches, asset loading, built-in dumpers, Lua/mod control, hooks, logs, and runtime debugging.
- The optional host endpoint runs as a Python process. It adds scoped UE4SS settings profiling and patching, file operations, process lifecycle, explicit-PID memory tools, mod lifecycle, C++ builds, Unreal Editor automation, native debugging, tasks, and repeatable validation scenarios. It can proxy the embedded runtime tools through one client connection.

Both endpoints accept IPv4 loopback addresses only. They are disabled until explicitly configured and have not passed the separate security review required for remote or public exposure.

## Enable the embedded runtime

Edit `UE4SS-settings.ini` before launching the game:

```ini
[MCP]
Enabled = 1
DevelopmentMode = 1
BindAddress = 127.0.0.1
Port = 8182
OperationTimeoutMs = 5000
```

Connect an MCP client to:

```text
http://127.0.0.1:8182/mcp
```

`DevelopmentMode = 1` enables tools that execute arbitrary Lua or mutate runtime state. Enable it only for a game and mod directory selected for local development. The listener rejects non-loopback bind addresses.

After the endpoint starts successfully, UE4SS pins `UE4SS.dll` until the game process exits. This prevents an explicit DLL unload from leaving the listener or queued runtime work in unmapped code. Restart the game to unload this MCP-enabled UE4SS instance.

Set `Port = 0` to select an available ephemeral port. The selected endpoint and session identifier are written to `UE4SS.log`:

```text
MCP: Listening at http://127.0.0.1:<port>/mcp (session <id>).
```

The transport starts before Unreal discovery finishes. Health and protocol metadata remain available during slow or degraded startup. Tools that require the UE4SS event loop or game thread return `lifecycle.runtime_not_ready` until their executor is available.

For a build, installation, smoke-test, host-test, reversible settings, and troubleshooting walkthrough, see [Testing AI MCP locally](../guides/testing-ai-mcp.md).

## Embedded runtime tools

| Area | Tools and behavior |
| --- | --- |
| Session | `ue4ss_session_snapshot`, `ue4ss_capabilities`, `ue4ss_mods_list`, and `ue4ss_logs_read` report the real process, UE4SS/Unreal versions, World generation, mod state, capability evidence, and bounded log pages. |
| Reflection | `ue4ss_objects_query` combines literal or regular-expression name, full-name, path, class, outer, address, instance-kind, property-name, and property-type filters. `cursor` and `maxScanObjects` bound each game-thread scan; `nextCursor` continues an incomplete scan. Returned handles are bound to the session, World generation, object index, and serial number. |
| Objects and assets | Inspect and property read/write tools use lifecycle-safe handles. Property watch tools sample on the game thread into bounded cursor histories. `ue4ss_asset_load` resolves and loads one exact asset registry object path and returns a handle. |
| Calls | `ue4ss_console_execute` and `ue4ss_function_call` run on the UE4SS game-thread executor and return structured outputs. |
| Hooks | Hook registration, removal, listing, and event reads expose bounded callback records with reflected parameter snapshots captured while ProcessEvent parameters remain valid. Client code never runs inside the native hook callback. |
| Lua | `ue4ss_lua_execute`, `ue4ss_lua_mod_control`, reload, attach, breakpoint, status, control, error, and detach tools support evaluation, runtime lifecycle, and debugging in a named Lua mod state. Runtime lifecycle changes do not edit `mods.txt`. |
| Dumpers | Profile and start tools expose the seven built-in object, C++ header, UHT header, Lua type, USMAP, actor, and static-mesh dumpers as observable asynchronous tasks with post-run file evidence. Cancellation is guaranteed only before a dumper enters its non-cooperative C++ body. |
| Kismet | Profile, breakpoint, status, control, and removal tools use the loaded `KismetDebuggerMod` interface when its MCP interface version is compatible. |

The endpoint also exposes session, capability, mod, and log resources. Logs and event streams use cursors instead of unbounded responses.

## Run the optional host endpoint

Copy `tools/ue4ss_mcp_host.example.json` and set explicit directories and tool paths:

```json
{
  "bindAddress": "127.0.0.1",
  "port": 0,
  "runtimeEndpoint": "http://127.0.0.1:8182/mcp",
  "stateDirectory": "../.ue4ss-mcp",
  "maxWorkers": 4,
  "roots": {
    "repo": "..",
    "mods": "../mcp-workspace/mods",
    "artifacts": "../mcp-workspace/artifacts",
    "game": "D:/Games/ExampleGame/Binaries/Win64"
  },
  "tools": {
    "cmake": "cmake",
    "unrealEditor": "C:/Program Files/Epic Games/UE_5.4/Engine/Binaries/Win64/UnrealEditor-Cmd.exe",
    "debugger": "C:/Tools/lldb-dap.exe"
  }
}
```

Paths supplied to host tools are resolved beneath one named root. Absolute paths, `..` escapes, and unsupported symlinks are rejected.

Start the host:

```powershell
python tools/ue4ss_mcp_host.py --config path/to/ue4ss-mcp-host.json --print-endpoint
```

The command prints one JSON record containing its selected loopback endpoint and host session identifier. `--runtime-endpoint` overrides the configured embedded endpoint. `--port 0` selects an ephemeral host port.

## Host tool groups

| Area | Behavior |
| --- | --- |
| Status and tasks | Host status, task list/get/cancel, and cursor-based events expose progress, recent output, completion, failure, and cancellation. These four colliding tool names default to `scope: "host"`; use `scope: "runtime"` with runtime task identifiers to inspect or cancel embedded dumper and wait tasks without losing the embedded result payload. |
| Settings | `ue4ss_settings_profile` parses a bounded UTF-8 `UE4SS-settings.ini` plus an optional read-only override and reports hashes, effective values, and override provenance. `ue4ss_settings_patch` requires the profiled SHA-256, preserves unrelated text/comments/order/BOM/newlines, writes atomically, verifies readback, and reports `restartRequired: true`. |
| Files and artifacts | Bounded list/read/search, atomic write, compare, move, guarded delete, backup/restore, and hashed manifests operate only under configured roots. |
| Mods | Lua and C++ source creation, verified install, atomic enable/disable, and drift-checked removal support a clean mod lifecycle. Generated Lua mods use `Scripts/main.lua`; generated C++ mods install `dlls/main.dll`. |
| C++ | `ue4ss_cpp_build` runs cancellable CMake configure/build/install operations and returns compiler diagnostics, process evidence, and bounded hashes for build or install artifacts. |
| Blueprint toolchain | Editor profile, Python script execution, Cook, Automation Tool packaging, and UnrealPak packaging use explicitly configured executables and scoped inputs. |
| Process and memory | Profile/discover/start/stop/restart tools manage executables only through configured roots. Explicit-PID region/read/write/pattern-scan tools are bounded, capability-gated, and verify writes by readback. |
| Native debugging | DAP profile, attach, source breakpoints, stack, variables, evaluation, execution control, session events, adapter output forwarding, and detach use the configured adapter. |
| Validation | `ue4ss_scenario_run` composes runtime calls, file checks, waits, captures, and assertions into a cancellable scenario with per-step evidence. |

The host does not infer that an editor or debugger is compatible merely because an executable exists. Profile tools report the available operations or the exact missing configuration. A missing editor does not disable reflection or runtime operations against Blueprint objects already loaded by the game.

## Operation states and evidence

Tool results include an operation identifier, session identifier, target, current state, changes, evidence, structured errors, suggested actions, and restart/reload requirements. Status is one of:

- `accepted` or `running`: asynchronous work is queued or executing; query it with the task tools.
- `succeeded`: execution completed and the reported state was read back.
- `failed`: execution failed; inspect the structured error code, stage, context, and suggested actions.
- `cancelled`: queued or running cancellable work accepted cancellation and stopped.
- `indeterminate`: execution started but completion could not be established before timeout or disconnect. Do not blindly repeat a non-idempotent action.

Read-only calls, file hashes, process output, runtime readback, event records, and scenario assertions provide evidence appropriate to the operation. A request being accepted is not evidence that a mod loaded or behaved correctly.

## Object and session lifetime

Object handles contain the MCP runtime session, World generation, UObject index, and allocated serial number. A new process creates a new session. A World change increments the generation. Rediscover objects after either change; stale handles are rejected.

Host and runtime operation identifiers belong to different sessions. The host records its own task and file/build events while proxied runtime results retain the embedded runtime session identifier.

## Current limits

- Process discovery and lifecycle tools require explicit scoped roots; they do not search or execute outside those roots.
- Blueprint creation, Cook, and packaging require a matching Unreal Editor/project/toolchain. Existing Blueprint instances remain available through runtime reflection when those tools are absent.
- Native debugging requires a configured DAP executable and symbols appropriate to the target.
- Runtime Kismet debugging requires a loaded, interface-compatible `KismetDebuggerMod`.
- Raw memory tools require an explicit process ID, are bounded to the documented sizes, and currently report unavailable on unsupported hosts.
- Compatibility is capability-specific. One failed probe does not disable unrelated tools.
- Runtime object queries execute on the game thread. Keep `maxScanObjects` bounded and continue through `nextCursor` rather than requesting one unbounded full-object-array scan.
