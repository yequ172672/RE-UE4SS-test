# Testing AI MCP locally

This guide verifies the UE4SS AI MCP on Windows in three layers:

1. Host unit tests without a game.
2. The embedded MCP against a running UE4SS game.
3. The optional host endpoint proxying that embedded runtime.

The examples use PowerShell and a CMake `Game__Dev__Win64` build. Replace every path in angle brackets with a path on your machine.

## Safety and prerequisites

Use an offline, single-player test installation without anti-cheat. Do not expose either MCP endpoint beyond the local machine. `DevelopmentMode = 1` enables arbitrary Lua execution and runtime mutation.

Before testing:

- Back up the game's `Binaries/Win64/ue4ss` directory and `UE4SS-settings.ini`.
- Confirm that the game already starts correctly with UE4SS installed.
- Install CMake 3.22 or newer, Ninja, Python 3.11 or newer, Rust 1.73 or newer, and Visual Studio 2022 17.13 or newer.
- Initialize repository submodules with `git submodule update --init --recursive`.
- Close the game before replacing `UE4SS.dll` or a C++ mod DLL.

## Build and install the runtime

From a Visual Studio x64 developer PowerShell:

```powershell
cmake -B build_cmake_Game__Dev__Win64 -G Ninja -DCMAKE_BUILD_TYPE=Game__Dev__Win64
cmake --build build_cmake_Game__Dev__Win64 --target UE4SS KismetDebuggerMod
```

Copy the resulting files into an existing UE4SS game installation:

```powershell
$GameWin64 = "<game>\Binaries\Win64"
$BuildBin = ".\build_cmake_Game__Dev__Win64\Game__Dev__Win64\bin"

Copy-Item "$BuildBin\UE4SS.dll" "$GameWin64\ue4ss\UE4SS.dll" -Force
New-Item "$GameWin64\ue4ss\Mods\KismetDebuggerMod\dlls" -ItemType Directory -Force | Out-Null
Copy-Item "$BuildBin\KismetDebuggerMod.dll" "$GameWin64\ue4ss\Mods\KismetDebuggerMod\dlls\main.dll" -Force
```

`KismetDebuggerMod` is optional unless Kismet debugging is being tested. Its `enabled.txt` marker must contain `1` if it should load at startup.

## Enable the embedded endpoint

Add or update this section in `UE4SS-settings.ini` before launching the game:

```ini
[MCP]
Enabled = 1
DevelopmentMode = 1
BindAddress = 127.0.0.1
Port = 8182
OperationTimeoutMs = 5000
```

Start the game and wait until UE4SS initialization finishes. Check `UE4SS.log` for:

```text
MCP: Listening at http://127.0.0.1:8182/mcp (session <id>).
```

Check transport health independently:

```powershell
Invoke-RestMethod http://127.0.0.1:8182/health
```

A healthy endpoint returns `status: ok`, its protocol version, and a non-empty session identifier. Health can become available before Unreal discovery completes, so it does not by itself prove that runtime tools are ready.

## Run the embedded smoke test

With the game still running, execute:

```powershell
py tools/mcp_smoke_test.py `
  --endpoint http://127.0.0.1:8182/mcp `
  --expect-runtime-ready
```

The command exits with code `0` only when all required checks pass. Its JSON output should show:

- `runtime: "succeeded"`;
- a non-empty tool list;
- at least one successful bounded object query;
- a property watch event;
- structured `transport.invalid_arguments`, `asset.not_found`, and `mod.not_found` failure cases;
- HTTP `403` for a non-loopback Origin;
- HTTP `400` for an unsupported MCP protocol version;
- JSON-RPC `-32602` for an unknown tool; and
- HTTP `405` for GET requests to `/mcp`.

If `--expect-runtime-ready` fails with `lifecycle.runtime_not_ready`, leave the game running until UE4SS finishes Unreal discovery and run the command again. Do not treat repeated retries as a fix for a persistent initialization error; inspect `UE4SS.log` instead.

## Call tools manually from PowerShell

The following helper sends one `tools/call` request and returns its structured result:

```powershell
function Invoke-Ue4ssMcpTool {
    param(
        [Parameter(Mandatory)] [string] $Endpoint,
        [Parameter(Mandatory)] [string] $Name,
        [hashtable] $Arguments = @{}
    )

    $headers = @{
        Accept = "application/json, text/event-stream"
        "MCP-Protocol-Version" = "2025-06-18"
    }
    $body = @{
        jsonrpc = "2.0"
        id = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
        method = "tools/call"
        params = @{
            name = $Name
            arguments = $Arguments
        }
    } | ConvertTo-Json -Depth 12 -Compress

    $response = Invoke-RestMethod `
        -Method Post `
        -Uri $Endpoint `
        -Headers $headers `
        -ContentType "application/json" `
        -Body $body

    if ($null -ne $response.error) {
        throw ($response.error | ConvertTo-Json -Depth 12)
    }
    return $response.result.structuredContent
}
```

Read the current session and capabilities:

```powershell
$Runtime = "http://127.0.0.1:8182/mcp"
$Session = Invoke-Ue4ssMcpTool $Runtime ue4ss_session_snapshot
$Capabilities = Invoke-Ue4ssMcpTool $Runtime ue4ss_capabilities

$Session.status
$Session.currentState
$Capabilities.currentState
```

A runtime-dependent capability is usable only when its reported state and evidence say it is available. Do not infer availability from the presence of a tool name alone.

Run a bounded object query:

```powershell
$Query = Invoke-Ue4ssMcpTool $Runtime ue4ss_objects_query @{
    className = "GameViewportClient"
    includeDerived = $true
    instanceMode = "instances"
    limit = 5
    maxScanObjects = 200000
}

$Query.status
$Query.currentState.objects
$Query.currentState.nextCursor
```

If `nextCursor` is non-null, pass it as `cursor` in the next request. Handles returned by one game process or World generation must not be reused after restarting the game or changing maps.

To inspect an object, use a returned handle:

```powershell
$Handle = $Query.currentState.objects[0].handle
$Object = Invoke-Ue4ssMcpTool $Runtime ue4ss_object_inspect @{
    handle = $Handle
    limit = 100
}
$Object.currentState.properties
```

Choose a readable property from the inspection result before testing `ue4ss_property_read` or the property-watch tools. Property names and available objects vary by game and map.

## Run host-only automated tests

The host test suite uses temporary directories and a fake runtime, so it does not modify a game installation:

```powershell
py -m unittest tools/test_mcp_host.py
```

A successful run reports all tests as `OK`. These tests cover scoped paths, atomic file operations, mod manifests, settings round-trips, cross-process settings serialization, processes, tasks, cancellation, DAP routing, scenarios, and host/runtime scope dispatch.

## Configure and start the optional host

Copy `tools/ue4ss_mcp_host.example.json` to a private location outside the repository and edit it. A minimal Windows configuration is:

```json
{
  "bindAddress": "127.0.0.1",
  "port": 0,
  "runtimeEndpoint": "http://127.0.0.1:8182/mcp",
  "stateDirectory": "C:/Temp/ue4ss-mcp-state",
  "maxWorkers": 4,
  "roots": {
    "repo": "D:/Source/RE-UE4SS",
    "game": "D:/Games/ExampleGame/Binaries/Win64",
    "mods": "D:/Games/ExampleGame/Binaries/Win64/ue4ss/Mods",
    "artifacts": "C:/Temp/ue4ss-mcp-artifacts"
  },
  "tools": {
    "cmake": "cmake"
  }
}
```

Only paths below a configured root can be accessed by host tools. Add `unrealEditor` or `debugger` executables only when testing those toolchains.

Create each directory that a test will use before starting the host. A configured root can appear as unavailable in host status when its directory does not exist.

Start the host:

```powershell
py tools/ue4ss_mcp_host.py `
  --config C:\Temp\ue4ss-mcp-host.json `
  --print-endpoint
```

The process prints one JSON line similar to:

```json
{"status":"ready","endpoint":"http://127.0.0.1:51536/mcp","sessionId":"host-..."}
```

Keep this process running. Use the printed endpoint in the PowerShell helper:

```powershell
$HostEndpoint = "http://127.0.0.1:51536/mcp"
$HostStatus = Invoke-Ue4ssMcpTool $HostEndpoint ue4ss_host_status
$HostStatus.currentState
```

When `runtimeEndpoint` is configured and the game is running, the host also proxies embedded tools. Run the same smoke test against the printed host endpoint:

```powershell
py tools/mcp_smoke_test.py `
  --endpoint $HostEndpoint `
  --expect-runtime-ready
```

For the four colliding task/event tools, omit `scope` or use `scope = "host"` for host operations. Use `scope = "runtime"` with embedded runtime task identifiers.

## Perform a reversible settings test

This test changes one file value, verifies it, and restores the original value. The configured `game` root in this example points to `Binaries/Win64`, so the settings path is relative to that directory.

```powershell
$Profile = Invoke-Ue4ssMcpTool $HostEndpoint ue4ss_settings_profile @{
    root = "game"
    path = "ue4ss/UE4SS-settings.ini"
}

$OldValue = [int]$Profile.currentState.baseSections.MCP.OperationTimeoutMs
$TestValue = $OldValue + 1

$Patched = Invoke-Ue4ssMcpTool $HostEndpoint ue4ss_settings_patch @{
    root = "game"
    path = "ue4ss/UE4SS-settings.ini"
    expectedSha256 = $Profile.currentState.baseSha256
    changes = @(
        @{
            section = "MCP"
            key = "OperationTimeoutMs"
            value = $TestValue
        }
    )
}

if ($Patched.status -ne "succeeded" -or -not $Patched.currentState.verified) {
    throw "Settings patch did not verify"
}

$Restored = Invoke-Ue4ssMcpTool $HostEndpoint ue4ss_settings_patch @{
    root = "game"
    path = "ue4ss/UE4SS-settings.ini"
    expectedSha256 = $Patched.currentState.baseSha256
    changes = @(
        @{
            section = "MCP"
            key = "OperationTimeoutMs"
            value = $OldValue
        }
    )
}

$Restored.status
$Restored.currentState.verified
```

Both mutations report `restartRequired: true`; they do not modify the already running `SettingsManager`. Restore the original value even if no game restart is planned. If another writer changes the file between profile and patch, the patch returns `artifact.conflict`; profile the file again instead of bypassing the hash guard.

## Recommended live test sequence

For a repeatable manual acceptance pass:

1. Start the game and confirm `/health`.
2. Run `mcp_smoke_test.py --expect-runtime-ready` against the embedded endpoint.
3. Read the session snapshot, capabilities, loaded mods, and logs.
4. Query one class with a bounded scan and inspect one returned handle.
5. Read and watch one game-specific property without changing it.
6. Execute a harmless Lua expression such as `return 2 + 2` in a dedicated test mod state.
7. Trigger one intentional failure with a missing asset or missing mod and confirm its structured error code.
8. Start the optional host and run its unit tests and proxied smoke test.
9. Perform the reversible settings test and confirm the original SHA-256 or value is restored.
10. Stop the host with `Ctrl+C`, close the game normally, and remove temporary host state or test mods.

Do not use destructive file, process, memory-write, function-call, hook, or debugger controls merely to prove that they are discoverable. Test those tools only against an expendable local installation with an explicit expected state and a rollback plan.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Connection refused | Confirm the game is running, `[MCP] Enabled = 1`, the configured port is free, and the listening line exists in `UE4SS.log`. |
| `lifecycle.runtime_not_ready` | Wait for Unreal discovery; then inspect earlier initialization or signature errors in `UE4SS.log`. |
| `capability.unavailable` | Read `ue4ss_capabilities` or the relevant profile tool for the exact missing hook, mod, executable, symbol, or platform condition. |
| `lifecycle.stale_handle` | Query the object again after a process restart or World/map change. |
| `artifact.conflict` | Re-profile or re-read the file and apply the intended change against the new SHA-256. |
| `mod.not_found` | Check the exact mod directory name, manifest, enabled marker, and `ue4ss_mods_list` output. |
| Kismet tools unavailable | Build, install, and enable the interface-compatible `KismetDebuggerMod`, then restart the game. |
| Host path rejected | Use a relative path below the named configured root; do not use an absolute path or `..`. |

For the complete tool groups, lifecycle rules, and current limitations, see [AI MCP](../feature-overview/ai-mcp.md).
