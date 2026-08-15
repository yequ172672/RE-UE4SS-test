# AI MCP

UE4SS can expose the current local game runtime as a Model Context Protocol (MCP) Streamable HTTP endpoint. The endpoint is disabled by default and only accepts IPv4 loopback addresses.

## Enable the endpoint

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

Use `DevelopmentMode = 1` only for a game and mod directory selected for local development. It enables tools that can execute arbitrary Lua and change runtime state. The listener rejects non-loopback bind addresses.

After the endpoint starts successfully, UE4SS pins `UE4SS.dll` until the game process exits. This prevents an explicit DLL unload from leaving the MCP listener or queued runtime work executing in unmapped code. Restart the game to unload this MCP-enabled UE4SS instance.

Set `Port = 0` to select an available ephemeral port. The selected URL and session identifier are written to `UE4SS.log` as:

```text
MCP: Listening at http://127.0.0.1:<port>/mcp (session <id>).
```

The transport starts before Unreal discovery finishes, so clients can inspect health and protocol metadata during a slow or degraded startup. Tools that require the UE4SS event loop or game thread return a structured `lifecycle.runtime_not_ready` error until those executors are available.

## Runtime tools

| Tool | Behavior |
| --- | --- |
| `ue4ss_session_snapshot` | Returns the process, UE4SS build, detected Unreal version, event-loop thread, current World/map and mod summary. |
| `ue4ss_capabilities` | Refreshes evidence-based `available`, `degraded` and `unavailable` capability states. |
| `ue4ss_mods_list` | Lists discovered Lua and C++ mods with paths and install/start state. |
| `ue4ss_logs_read` | Reads a bounded page of `UE4SS.log` using a byte cursor. |
| `ue4ss_objects_find` | Finds live instances by reflected class name and returns session/World/index/serial object handles. |
| `ue4ss_lua_execute` | Executes Lua in a named mod's game-thread Lua state. Requires development mode. |
| `ue4ss_lua_mod_reload` | Uninstalls and reinstalls a named Lua mod, then reads back its actual state. Requires development mode. |

The server also exposes `ue4ss://session`, `ue4ss://capabilities`, `ue4ss://mods`, and `ue4ss://logs` resources. Large log reads use `cursor`, `nextCursor`, and `maxBytes` rather than returning an unbounded response.

## Result states

Tool results include an operation identifier, session identifier, target, current state, evidence, restart requirement, structured errors, and suggested actions. Status is one of:

- `succeeded`: the operation completed and its current state was read back.
- `failed`: the operation did not complete; inspect the structured error code.
- `indeterminate`: execution began but did not finish before the timeout. Do not blindly repeat a non-idempotent action.

Queued work that has not started is cancelled when its timeout expires. Work that already started reports `operation.indeterminate` instead of claiming it was cancelled.

## Object lifetime

Object handles contain the MCP session, World generation, UObject index, and allocated serial number. A new process creates a new session. A World change increments the generation. Clients must stop using handles from an old session or World and discover the object again.

## Current capability limits

Capability discovery reports missing conditions explicitly. Structured Blueprint creation/Cook/package operations, C++ builds, native debugging, and raw memory operations are currently reported as unavailable or degraded rather than being inferred from a game-version whitelist.

The endpoint is a local development interface. It has not passed the separate security review required for remote access or public distribution.
