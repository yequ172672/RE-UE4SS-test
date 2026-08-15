#define NOMINMAX

#include <McpServer.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <Unreal/FOutputDevice.hpp>
#include <limits>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <DynamicOutput/DynamicOutput.hpp>
#include <GUI/LuaDebugger.hpp>
#include <GUI/Dumpers.hpp>
#include <Helpers/String.hpp>
#include <Mod/CppMod.hpp>
#include <Mod/LuaMod.hpp>
#include <UE4SSRuntime.hpp>
#include <USMapGenerator/Generator.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UPackage.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UAssetRegistry.hpp>
#include <Unreal/UAssetRegistryHelpers.hpp>
#include <Unreal/World.hpp>
#include <Unreal/UGameViewportClient.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <Unreal/UnrealVersion.hpp>
#include <fmt/format.h>
#include <glaze/glaze.hpp>

struct KismetDebuggerApiV1
{
    uint32_t version;
    uint32_t size;
    bool (*enable)();
    void (*disable)();
    bool (*is_enabled)();
    bool (*pause_execution)();
    void (*continue_execution)();
    void (*step_into)();
    void (*step_over)();
    void (*step_out)();
    const char* (*snapshot_json)();
    const char* (*breakpoints_json)();
    bool (*add_breakpoint)(const char*, uint64_t);
    bool (*remove_breakpoint)(const char*, uint64_t);
    const char* (*last_error)();
};

using GetKismetDebuggerApi = const KismetDebuggerApiV1* (*)(uint32_t);
inline constexpr uint32_t kismet_debugger_api_version = 1;

namespace RC::Mcp
{
    namespace
    {
        using Json = glz::generic_json<glz::num_mode::u64>;
        using JsonArray = Json::array_t;
        using JsonObject = Json::object_t;

        constexpr std::string_view protocol_version{"2025-06-18"};
        constexpr size_t maximum_http_request_size = 1024 * 1024;

        enum class PendingState
        {
            Queued,
            Running,
            Completed,
            Cancelled,
        };

        class McpError : public std::runtime_error
        {
          public:
            McpError(std::string code, std::string message) : std::runtime_error(std::move(message)), m_code(std::move(code))
            {
            }

            [[nodiscard]] auto code() const -> const std::string&
            {
                return m_code;
            }

          private:
            std::string m_code;
        };

        auto json_object() -> Json
        {
            return JsonObject{};
        }

        auto json_array() -> Json
        {
            return JsonArray{};
        }

        auto serialize_json(const Json& value) -> std::string
        {
            auto serialized = glz::write_json(value);
            if (!serialized)
            {
                throw std::runtime_error{"Failed to serialize MCP JSON response"};
            }
            return std::move(serialized.value());
        }

        auto parse_json(std::string_view value) -> Json
        {
            Json parsed{};
            if (auto error = glz::read_json(parsed, value); error)
            {
                throw McpError{"transport.invalid_json", "Request body is not valid JSON"};
            }
            return parsed;
        }

        auto find_member(const Json& value, std::string_view name) -> const Json*
        {
            if (!value.is_object())
            {
                return nullptr;
            }
            const auto& object = value.get<JsonObject>();
            const auto member = object.find(std::string{name});
            return member == object.end() ? nullptr : &member->second;
        }

        auto required_string(const Json& value, std::string_view name) -> std::string
        {
            const auto* member = find_member(value, name);
            if (!member || !member->is_string())
            {
                throw McpError{"transport.invalid_arguments", fmt::format("Missing string argument '{}'", name)};
            }
            return member->get<std::string>();
        }

        auto optional_string(const Json& value, std::string_view name) -> std::optional<std::string>
        {
            const auto* member = find_member(value, name);
            if (!member)
            {
                return std::nullopt;
            }
            if (!member->is_string())
            {
                throw McpError{"transport.invalid_arguments", fmt::format("Argument '{}' must be a string", name)};
            }
            return member->get<std::string>();
        }

        auto optional_boolean(const Json& value, std::string_view name, bool fallback) -> bool
        {
            const auto* member = find_member(value, name);
            if (!member)
            {
                return fallback;
            }
            if (!member->is_boolean())
            {
                throw McpError{"transport.invalid_arguments", fmt::format("Argument '{}' must be a boolean", name)};
            }
            return member->get<bool>();
        }

        auto optional_integer(const Json& value, std::string_view name, int64_t fallback) -> int64_t
        {
            const auto* member = find_member(value, name);
            if (!member)
            {
                return fallback;
            }
            if (!member->is_number())
            {
                throw McpError{"transport.invalid_arguments", fmt::format("Argument '{}' must be an integer", name)};
            }
            if (const auto* signed_value = member->get_if<int64_t>())
            {
                return *signed_value;
            }
            if (const auto* unsigned_value = member->get_if<uint64_t>())
            {
                if (*unsigned_value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                {
                    return static_cast<int64_t>(*unsigned_value);
                }
                throw McpError{"transport.invalid_arguments", fmt::format("Argument '{}' must be an integer", name)};
            }

            const auto numeric_value = member->get<double>();
            constexpr auto int64_exclusive_upper_bound = 9223372036854775808.0;
            if (!std::isfinite(numeric_value) || std::trunc(numeric_value) != numeric_value ||
                numeric_value < static_cast<double>(std::numeric_limits<int64_t>::min()) || numeric_value >= int64_exclusive_upper_bound)
            {
                throw McpError{"transport.invalid_arguments", fmt::format("Argument '{}' must be an integer", name)};
            }
            return static_cast<int64_t>(numeric_value);
        }
        auto required_integer(const Json& value, std::string_view name, int64_t minimum, int64_t maximum) -> int64_t
        {
            if (!find_member(value, name))
            {
                throw McpError{"transport.invalid_arguments", fmt::format("Missing integer argument '{}'", name)};
            }
            const auto result = optional_integer(value, name, minimum);
            if (result < minimum || result > maximum)
            {
                throw McpError{"transport.invalid_arguments", fmt::format("Argument '{}' must be between {} and {}", name, minimum, maximum)};
            }
            return result;
        }

        auto process_creation_id() -> std::string
        {
            FILETIME creation{}, exit{}, kernel{}, user{};
            ULARGE_INTEGER creation_value{};
            if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
            {
                creation_value.LowPart = creation.dwLowDateTime;
                creation_value.HighPart = creation.dwHighDateTime;
            }
            return fmt::format("{}-{:016X}", GetCurrentProcessId(), creation_value.QuadPart);
        }

        auto executable_path() -> std::string
        {
            std::wstring buffer(32768, L'\0');
            const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            buffer.resize(length);
            return std::filesystem::path{buffer}.generic_string();
        }

        auto lowercase(std::string value) -> std::string
        {
            std::ranges::transform(value, value.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        }

        class BoundedOutputCapture final : public Unreal::FOutputDevice
        {
          public:
            explicit BoundedOutputCapture(size_t maximum_bytes) : m_maximum_bytes(maximum_bytes)
            {
                bSuppressEventTag = false;
                bAutoEmitLineTerminator = true;
            }

            void Virtual_Serialize(const TCHAR* value, Unreal::ELogVerbosity::Type, const Unreal::FName&) override
            {
                append(value);
            }

            void Virtual_Serialize(const TCHAR* value, Unreal::ELogVerbosity::Type, const Unreal::FName&, double) override
            {
                append(value);
            }

            [[nodiscard]] auto output() const -> const std::string&
            {
                return m_output;
            }

            [[nodiscard]] auto truncated() const -> bool
            {
                return m_truncated;
            }

          private:
            void append(const TCHAR* value)
            {
                if (!value)
                {
                    return;
                }
                const auto text = to_string(StringType{value});
                const auto remaining = m_maximum_bytes - m_output.size();
                if (text.size() > remaining)
                {
                    auto valid_prefix = remaining;
                    while (valid_prefix > 0 && (static_cast<unsigned char>(text[valid_prefix]) & 0xC0U) == 0x80U)
                    {
                        --valid_prefix;
                    }
                    m_output.append(text.data(), valid_prefix);
                    m_truncated = true;
                }
                else
                {
                    m_output.append(text);
                }
            }

            size_t m_maximum_bytes{};
            std::string m_output{};
            bool m_truncated{};
        };

        auto trim(std::string_view value) -> std::string_view
        {
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            {
                value.remove_prefix(1);
            }
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            {
                value.remove_suffix(1);
            }
            return value;
        }

        auto tool_definitions() -> Json
        {
            static const std::string definitions = [] {
                std::string result = R"json(
{
  "tools": [
    {
      "name": "ue4ss_session_snapshot",
      "description": "Read the connected UE4SS game session, process, Unreal version, World/map, threads and mod summary. Read-only. Requires UE4SS runtime initialization; World fields can be null before a map is ready.",
      "inputSchema": {"type":"object","properties":{},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_capabilities",
      "description": "Probe capabilities from current runtime evidence. Read-only. Returns available/degraded/unavailable with evidence and missing conditions instead of relying on a global game-version whitelist.",
      "inputSchema": {"type":"object","properties":{},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_mods_list",
      "description": "List discovered Lua and C++ mods with path, stable runtime id, installability, installed state and started state. Read-only. Requires the UE4SS event loop.",
      "inputSchema": {"type":"object","properties":{},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_logs_read",
      "description": "Read a bounded page of UE4SS.log by byte cursor. Read-only and idempotent for an unchanged log. The log can grow concurrently; use nextCursor to continue.",
      "inputSchema": {"type":"object","properties":{"cursor":{"type":"integer","minimum":0},"maxBytes":{"type":"integer","minimum":1,"maximum":65536}},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_objects_query",
      "description": "Query live UObjects with bounded, conjunctive filters and return lifecycle-safe session/world/index/serial handles without retaining UObject pointers. Literal text filters use substring matching; regex uses ECMAScript search; outer matches the outer path; hasProperties and propertyTypes require every listed value. cursor and maxScanObjects bound one game-thread scan page; nextCursor continues an incomplete page. Requires initialized GUObjectArray and at least one effective filter; malformed filters fail before scanning.",
      "inputSchema": {"type":"object","properties":{"name":{"type":"string","minLength":1},"fullName":{"type":"string","minLength":1},"path":{"type":"string","minLength":1},"className":{"type":"string","minLength":1},"outer":{"type":"string","minLength":1},"address":{"type":"string","minLength":1},"regex":{"type":"boolean"},"caseSensitive":{"type":"boolean"},"includeDerived":{"type":"boolean"},"instanceMode":{"type":"string","enum":["any","instances","nonInstances","classDefaultObjects"]},"hasProperties":{"type":"array","maxItems":128,"uniqueItems":true,"items":{"type":"string","minLength":1}},"propertyTypes":{"type":"array","maxItems":128,"uniqueItems":true,"items":{"type":"string","minLength":1}},"cursor":{"type":"integer","minimum":0},"maxScanObjects":{"type":"integer","minimum":1,"maximum":200000},"limit":{"type":"integer","minimum":1,"maximum":500}},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_asset_load",
      "description": "Resolve exactly one Unreal asset registry object path and load that asset on the game thread. Returns lifecycle-safe session/world/index/serial identity for subsequent inspect/read operations and never retains a raw UObject pointer. Side effect: the asset/package may become resident. Requires DevelopmentMode=1, the UEngine::Tick game-thread callback, GUObjectArray and AssetRegistry; failures distinguish thread.unavailable, asset.registry_unavailable, asset.not_found and asset.load_failed.",
      "inputSchema": {"type":"object","required":["objectPath"],"properties":{"objectPath":{"type":"string","minLength":1}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":true}
    },
)json";
                result += R"json(
    {
      "name": "ue4ss_console_execute",
      "description": "Execute an Unreal console command through the current GameViewportClient on the game thread. Captures at most 65536 bytes written by ProcessConsoleExec itself and separately reports UE4SS.log byte cursors before/after; the log is not presented as command output. Side effects depend on the command. Requires DevelopmentMode=1, a live viewport and the UEngine::Tick game-thread callback; capture truncation is explicit.",
      "inputSchema": {"type":"object","required":["command"],"properties":{"command":{"type":"string","minLength":1}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":true}
    },
    {
      "name": "ue4ss_hook_register",
      "description": "Register a read-only ProcessEvent observer filtered by exact UFunction full name. Events and bounded reflected parameter text values are copied while callback data remains valid; raw UObject or parameter pointers are never retained. Requires DevelopmentMode=1 and ProcessEvent Hook availability.",
      "inputSchema": {"type":"object","required":["functionFullName"],"properties":{"functionFullName":{"type":"string","minLength":1},"phase":{"type":"string","enum":["pre","post"]},"objectClass":{"type":"string","minLength":1}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":true}
    },
    {
      "name": "ue4ss_hooks_list",
      "description": "List ProcessEvent observers registered by this MCP session. Read-only.",
      "inputSchema": {"type":"object","properties":{},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_hook_unregister",
      "description": "Unregister one MCP ProcessEvent observer and wait for callbacks already in flight to finish.",
      "inputSchema": {"type":"object","required":["hookId"],"properties":{"hookId":{"type":"string","minLength":1}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_hook_events_read",
      "description": "Read a bounded page of owned ProcessEvent event snapshots by sequence cursor. Reports cursor expiration when the bounded queue has overwritten older events.",
      "inputSchema": {"type":"object","properties":{"cursor":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":1,"maximum":1000}},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_object_inspect",
      "description": "Resolve a lifecycle-safe object handle and list reflected properties and functions with bounded metadata. Old-session, old-map and stale serial handles are rejected.",
      "inputSchema": {"type":"object","required":["handle"],"properties":{"handle":{"type":"string","minLength":1},"limit":{"type":"integer","minimum":1,"maximum":1000}},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_property_read",
      "description": "Resolve an object handle and export one reflected property through Unreal FProperty text serialization on the game thread.",
      "inputSchema": {"type":"object","required":["handle","propertyName"],"properties":{"handle":{"type":"string","minLength":1},"propertyName":{"type":"string","minLength":1}},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_property_watch_start",
      "description": "Start bounded game-thread sampling of one reflected property identified only by a lifecycle-safe object handle and property name. Records the initial ExportText value, then changed values unless emitUnchanged is true. Requires a live handle, reflected property, initialized UEngine::Tick callback, and historyLimit from 1 to 1000; side effects are an in-memory observer and bounded history.",
      "inputSchema": {"type":"object","required":["handle","propertyName","historyLimit"],"properties":{"handle":{"type":"string","minLength":1},"propertyName":{"type":"string","minLength":1},"historyLimit":{"type":"integer","minimum":1,"maximum":1000},"emitUnchanged":{"type":"boolean","default":false}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}
    },
    {
      "name": "ue4ss_property_watches_list",
      "description": "List active and stopped property watches with bounded-history cursors. Read-only and thread-safe; requires this MCP server session.",
      "inputSchema": {"type":"object","properties":{},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_property_watch_read",
      "description": "Read a bounded page of value or expired events for one property watch by sequence cursor. Reports cursorExpired when bounded history overwrote unread events and truncated when more events remain. Read-only; requires a watchId from this MCP server session.",
      "inputSchema": {"type":"object","required":["watchId"],"properties":{"watchId":{"type":"string","minLength":1},"cursor":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":1,"maximum":1000}},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_property_watch_stop",
      "description": "Idempotently stop future sampling for one property watch while retaining its bounded history for reads. Requires a watchId string; side effect is disabling that in-memory observer.",
      "inputSchema": {"type":"object","required":["watchId"],"properties":{"watchId":{"type":"string","minLength":1}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_property_write",
      "description": "Resolve an object handle, import a text value through the reflected FProperty, and read it back on the game thread. Requires DevelopmentMode=1; invalid Unreal text is rejected.",
      "inputSchema": {"type":"object","required":["handle","propertyName","value"],"properties":{"handle":{"type":"string","minLength":1},"propertyName":{"type":"string","minLength":1},"value":{"type":"string"}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":true}
    },
    {
      "name": "ue4ss_function_call",
      "description": "Invoke a reflected UFunction on a lifecycle-safe object handle on the game thread. Parameter values are Unreal text keyed by parameter name; returns exported out and return values. Requires DevelopmentMode=1.",
      "inputSchema": {"type":"object","required":["handle","functionName"],"properties":{"handle":{"type":"string","minLength":1},"functionName":{"type":"string","minLength":1},"arguments":{"type":"object","additionalProperties":{"type":"string"}}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":true}
    },
)json";
                result += R"json(
    {
      "name": "ue4ss_lua_execute",
      "description": "Execute arbitrary Lua code inside a named started Lua mod and return primitive result values or a traceback. Side effects are unrestricted in development mode. Requires DevelopmentMode=1, a started Lua mod and the UE4SS event loop; common failures are syntax/runtime errors or a missing mod.",
      "inputSchema": {"type":"object","required":["modName","code"],"properties":{"modName":{"type":"string","minLength":1},"code":{"type":"string"}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":true}
    },
    {
      "name": "ue4ss_lua_mod_reload",
      "description": "Uninstall and reinstall a named Lua mod, then report its actual started state. Side effects include callback/Hook teardown and entry-script execution. Requires DevelopmentMode=1 and an installed Lua mod; C++ mods are rejected because safe hot unload is unavailable.",
      "inputSchema": {"type":"object","required":["modName"],"properties":{"modName":{"type":"string","minLength":1}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":true}
    },
    {
      "name": "ue4ss_lua_mod_control",
      "description": "Start, stop or restart one Lua mod on the UE4SS event-loop thread and return its post-action installed/started state. Stop remembers the runtime path before uninstall; start recreates a stopped/uninstalled Lua mod from that known path; restart uses the existing reinstall path. Side effects include Lua state, callback and Hook teardown/creation and entry-script execution. Requires DevelopmentMode=1 and the event loop. This never edits mods.txt (persisted=false); C++ mods fail with mod.cpp_restart_required because hot unload is unsafe.",
      "inputSchema": {"type":"object","required":["name","action"],"properties":{"name":{"type":"string","minLength":1},"action":{"type":"string","enum":["start","stop","restart"]}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":true}
    },
    {
      "name": "ue4ss_lua_debug_attach",
      "description": "Install the UE4SS Lua line debugger on a named started mod's game-thread Hook state. Enables breakpoints in ProcessEvent and game-thread callbacks. Requires DevelopmentMode=1.",
      "inputSchema": {"type":"object","required":["modName"],"properties":{"modName":{"type":"string","minLength":1}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_lua_debug_detach",
      "description": "Continue a paused Lua Hook state if necessary and remove its line debugger. Requires DevelopmentMode=1.",
      "inputSchema": {"type":"object","required":["modName"],"properties":{"modName":{"type":"string","minLength":1}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_lua_breakpoint_set",
      "description": "Create or replace a Lua source-line breakpoint. The optional condition is evaluated against frame locals with globals as fallback. Requires DevelopmentMode=1.",
      "inputSchema": {"type":"object","required":["source","line"],"properties":{"source":{"type":"string","minLength":1},"line":{"type":"integer","minimum":1},"condition":{"type":"string"}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_lua_breakpoint_remove",
      "description": "Remove a Lua source-line breakpoint. Requires DevelopmentMode=1.",
      "inputSchema": {"type":"object","required":["source","line"],"properties":{"source":{"type":"string","minLength":1},"line":{"type":"integer","minimum":1}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_lua_breakpoints_list",
      "description": "List Lua source-line breakpoints and hit counts.",
      "inputSchema": {"type":"object","properties":{},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_lua_debug_status",
      "description": "Read the current Lua pause state, source line, call stack, local variables and stack values from the snapshot captured on the Lua owner thread.",
      "inputSchema": {"type":"object","properties":{},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_lua_debug_control",
      "description": "Continue or step a paused Lua Hook state. Control is delivered directly so it remains available while the game thread is paused.",
      "inputSchema": {"type":"object","required":["action"],"properties":{"action":{"type":"string","enum":["continue","stepInto","stepOver","stepOut"]}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}
    },
    {
      "name": "ue4ss_lua_errors_read",
      "description": "Read bounded structured Lua errors with traceback, call stack and stack snapshots captured by UE4SS.",
      "inputSchema": {"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":100}},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
)json";
                result += R"json(
    {
      "name": "ue4ss_events_read",
      "description": "Read a bounded page of session events by sequence cursor, including operations, World transitions and asynchronous task state changes. Reports cursor expiration after bounded retention.",
      "inputSchema": {"type":"object","properties":{"cursor":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":1,"maximum":1000}},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_operations_read",
      "description": "Read bounded structured operation records produced during this MCP session by sequence cursor.",
      "inputSchema": {"type":"object","properties":{"cursor":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":1,"maximum":1000}},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_tasks_list",
      "description": "List asynchronous tasks with running, succeeded, failed or cancelled state, progress, current action and recent output.",
      "inputSchema": {"type":"object","properties":{},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_task_get",
      "description": "Read one asynchronous task's current state, partial changes, result and errors.",
      "inputSchema": {"type":"object","required":["taskId"],"properties":{"taskId":{"type":"string","minLength":1}},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_task_cancel",
      "description": "Request cooperative cancellation of one running asynchronous task. Completed tasks are left unchanged.",
      "inputSchema": {"type":"object","required":["taskId"],"properties":{"taskId":{"type":"string","minLength":1}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_wait_start",
      "description": "Start a cancellable asynchronous wait for a duration or for UE4SS.log to contain text. Returns immediately with a task identifier and bounded progress state.",
      "inputSchema": {"type":"object","properties":{"durationMs":{"type":"integer","minimum":0,"maximum":3600000},"logContains":{"type":"string","minLength":1},"timeoutMs":{"type":"integer","minimum":100,"maximum":3600000}},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}
    },
    {
      "name": "ue4ss_dumper_profile",
      "description": "Report availability, output locations, executor and cancellation limits for the seven built-in UE4SS dumpers. Read-only. Availability requires an initialized Unreal runtime and a live UE4SS event loop; actors and staticMeshes additionally require a loaded World.",
      "inputSchema": {"type":"object","properties":{},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_dumper_start",
      "description": "Start exactly one built-in C++ dumper on the UE4SS event loop and return immediately with an AsyncTask. Requires DevelopmentMode=1, initialized Unreal runtime and a live UE4SS event loop. Side effects create or overwrite files below the UE4SS working directory and some generators may load assets. Cancellation is honored only before the dumper entrance begins; running dumpers are non-cooperative.",
      "inputSchema": {"type":"object","required":["kind"],"properties":{"kind":{"type":"string","enum":["objects","cxxHeaders","uhtHeaders","luaTypes","usmap","actors","staticMeshes"]}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":true}
    },
    {
      "name": "ue4ss_kismet_debug_profile",
      "description": "Report whether the loaded KismetDebuggerMod exposes the versioned MCP debugger interface and whether opcode interception is enabled.",
      "inputSchema": {"type":"object","properties":{},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_kismet_debug_enable",
      "description": "Enable Kismet bytecode opcode interception through the loaded KismetDebuggerMod. Requires DevelopmentMode=1 and a compatible debugger mod.",
      "inputSchema": {"type":"object","properties":{},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_kismet_debug_disable",
      "description": "Restore the original Kismet opcode table, resume any paused script and wait for in-flight debugger hooks to drain.",
      "inputSchema": {"type":"object","properties":{},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_kismet_breakpoint_set",
      "description": "Persist a Kismet breakpoint identified by exact UFunction full name and bytecode offset. Requires DevelopmentMode=1.",
      "inputSchema": {"type":"object","required":["functionFullName","bytecodeOffset"],"properties":{"functionFullName":{"type":"string","minLength":1},"bytecodeOffset":{"type":"integer","minimum":0}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_kismet_breakpoint_remove",
      "description": "Remove a persisted Kismet breakpoint by UFunction full name and bytecode offset. Requires DevelopmentMode=1.",
      "inputSchema": {"type":"object","required":["functionFullName","bytecodeOffset"],"properties":{"functionFullName":{"type":"string","minLength":1},"bytecodeOffset":{"type":"integer","minimum":0}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_kismet_breakpoints_list",
      "description": "List persisted Kismet breakpoints keyed by UFunction full name.",
      "inputSchema": {"type":"object","properties":{},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_kismet_debug_status",
      "description": "Read an owned Kismet pause snapshot with expression, function, bytecode offset, call stack and reflected frame locals.",
      "inputSchema": {"type":"object","properties":{},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_kismet_debug_control",
      "description": "Request pause or directly continue/step a paused Kismet script. Direct controls remain available while the game thread is blocked.",
      "inputSchema": {"type":"object","required":["action"],"properties":{"action":{"type":"string","enum":["pause","continue","stepInto","stepOver","stepOut"]}},"additionalProperties":false},
      "annotations": {"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}
    }
  ]
}
)json";
                return result;
            }();
            return parse_json(definitions);
        }
    } // namespace

    class Server::Impl
    {
      private:
        struct AsyncTask
        {
            mutable std::mutex mutex;
            std::atomic_bool cancel_requested{false};
            std::shared_ptr<std::atomic<PendingState>> execution_state{};
            Json state{json_object()};
            std::jthread worker{};
        };

        struct PropertyWatch
        {
            std::string id;
            std::string handle;
            std::string property_name;
            size_t history_limit{};
            bool emit_unchanged{};
            bool active{true};
            bool expired{};
            uint64_t next_sequence{1};
            std::optional<std::string> last_value{};
            std::deque<Json> history{};
        };
        struct DumperFileState
        {
            uintmax_t size{};
            int64_t modified_time_milliseconds{};
        };

        using DumperFileSnapshot = std::unordered_map<std::string, DumperFileState>;

      public:
        Impl(UE4SSProgram& program, std::string bind_address, uint16_t port, int64_t operation_timeout_ms, bool development_mode)
            : m_program(program), m_bind_address(std::move(bind_address)), m_requested_port(port),
              m_operation_timeout(std::chrono::milliseconds{std::clamp<int64_t>(operation_timeout_ms, 100, 120000)}), m_development_mode(development_mode),
              m_session_id(process_creation_id())
        {
        }

        ~Impl()
        {
            stop();
        }

        auto start() -> bool
        {
            if (m_running.exchange(true))
            {
                return true;
            }

            WSADATA winsock_data{};
            if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0)
            {
                m_running = false;
                Output::send<LogLevel::Error>(STR("MCP: WSAStartup failed.\n"));
                return false;
            }
            m_winsock_initialized = true;

            m_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (m_listen_socket == INVALID_SOCKET)
            {
                stop();
                Output::send<LogLevel::Error>(STR("MCP: Could not create listening socket.\n"));
                return false;
            }

            int reuse_address = 1;
            setsockopt(m_listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse_address), sizeof(reuse_address));

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(m_requested_port);
            if (inet_pton(AF_INET, m_bind_address.c_str(), &address.sin_addr) != 1)
            {
                Output::send<LogLevel::Error>(STR("MCP: BindAddress '{}' is not an IPv4 address.\n"), ensure_str(m_bind_address));
                stop();
                return false;
            }
            if ((ntohl(address.sin_addr.s_addr) >> 24U) != 127U)
            {
                Output::send<LogLevel::Error>(STR("MCP: Refusing non-loopback BindAddress '{}'; development server is local-only.\n"), ensure_str(m_bind_address));
                stop();
                return false;
            }

            if (::bind(m_listen_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(m_listen_socket, SOMAXCONN) == SOCKET_ERROR)
            {
                Output::send<LogLevel::Error>(STR("MCP: Failed to bind {}:{} (Winsock error {}).\n"), ensure_str(m_bind_address), m_requested_port, WSAGetLastError());
                stop();
                return false;
            }

            sockaddr_in bound_address{};
            int bound_address_size = sizeof(bound_address);
            if (getsockname(m_listen_socket, reinterpret_cast<sockaddr*>(&bound_address), &bound_address_size) == 0)
            {
                m_bound_port = ntohs(bound_address.sin_port);
            }

            HMODULE owning_module{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    reinterpret_cast<LPCWSTR>(protocol_version.data()),
                                    &owning_module))
            {
                Output::send<LogLevel::Error>(STR("MCP: Failed to pin UE4SS.dll; refusing to start an unload-unsafe server thread.\n"));
                stop();
                return false;
            }

            activate_runtime();
            for (auto& client_thread : m_client_threads)
            {
                client_thread = std::jthread{[this] {
                    process_client_queue();
                }};
            }

            m_thread = std::jthread{[this] {
                serve();
            }};
            Output::send(STR("MCP: Listening at http://{}:{}/mcp (session {}).\n"), ensure_str(m_bind_address), m_bound_port, ensure_str(m_session_id));
            return true;
        }

        auto activate_runtime() -> void
        {
            // The versioned Kismet export's module is pinned when discovered. Its API gates every
            // call on the current mod instance, so full mod restarts can clear/recreate that instance
            // without invalidating function pointers held by active MCP clients.
            if (!m_kismet_api.load())
            {
                m_kismet_api.store(discover_kismet_debugger_api());
            }
            if (m_game_thread_callback_id != Unreal::Hook::ERROR_ID || !UE4SSRuntime::IsEngineTickAvailable())
            {
                return;
            }
            m_game_thread_callback_id = Unreal::Hook::RegisterEngineTickPreCallback(
                    [this](auto&, Unreal::UEngine*, float, bool) {
                        process_game_thread_queue();
                    },
                    {false, false, STR("UE4SS"), STR("McpServer")});
        }

        auto stop() -> void
        {
            if (!m_running.exchange(false) && m_listen_socket.load() == INVALID_SOCKET && !m_winsock_initialized)
            {
                return;
            }
            {
                std::lock_guard lock{m_property_watches_mutex};
                m_property_watches.clear();
                m_property_watch_order.clear();
                m_property_watch_rotation = 0;
            }
            {
                std::lock_guard lock{m_request_mutex};
                for (const auto& [_, cancellation] : m_active_requests)
                {
                    cancellation->store(true);
                }
            }
            if (GUI::LuaDebugger::has_instance())
            {
                auto& debugger = GUI::LuaDebugger::get();
                debugger.clear_all_breakpoints();
                debugger.continue_execution();
            }
            const auto listen_socket = m_listen_socket.exchange(INVALID_SOCKET);
            if (listen_socket != INVALID_SOCKET)
            {
                shutdown(listen_socket, SD_BOTH);
                closesocket(listen_socket);
            }
            {
                std::scoped_lock lock{m_client_mutex};
                for (const auto client : m_pending_clients)
                {
                    shutdown(client, SD_BOTH);
                    closesocket(client);
                }
                m_pending_clients.clear();
                for (const auto client : m_active_clients)
                {
                    shutdown(client, SD_BOTH);
                }
            }
            m_client_condition.notify_all();
            if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id())
            {
                m_thread.join();
            }
            for (auto& client_thread : m_client_threads)
            {
                if (client_thread.joinable() && client_thread.get_id() != std::this_thread::get_id())
                {
                    client_thread.join();
                }
            }
            stop_tasks();
            if (m_kismet_enabled_by_mcp.exchange(false))
            {
                if (const auto* api = kismet_debugger_api())
                {
                    api->disable();
                }
            }
            m_kismet_api.store(nullptr);
            std::vector<Unreal::Hook::GlobalCallbackId> process_event_callback_ids{};
            {
                std::lock_guard lock{m_hook_mutex};
                process_event_callback_ids.reserve(m_process_event_hooks.size());
                for (const auto& [_, hook] : m_process_event_hooks)
                {
                    process_event_callback_ids.emplace_back(hook.callback_id);
                }
                m_process_event_hooks.clear();
            }
            for (const auto callback_id : process_event_callback_ids)
            {
                Unreal::Hook::UnregisterCallback(callback_id);
            }
            if (m_game_thread_callback_id != Unreal::Hook::ERROR_ID)
            {
                Unreal::Hook::UnregisterCallback(m_game_thread_callback_id);
                m_game_thread_callback_id = Unreal::Hook::ERROR_ID;
            }
            if (m_winsock_initialized)
            {
                WSACleanup();
                m_winsock_initialized = false;
            }
        }

        auto abandon_for_process_exit() -> void
        {
            m_abandoned = true;
            m_running = false;
            const auto listen_socket = m_listen_socket.exchange(INVALID_SOCKET);
            if (listen_socket != INVALID_SOCKET)
            {
                closesocket(listen_socket);
            }
            {
                std::scoped_lock lock{m_client_mutex};
                for (const auto client : m_pending_clients)
                {
                    closesocket(client);
                }
                m_pending_clients.clear();
                for (const auto client : m_active_clients)
                {
                    closesocket(client);
                }
                m_active_clients.clear();
            }
            m_client_condition.notify_all();
            if (m_thread.joinable())
            {
                m_thread.detach();
            }
            for (auto& client_thread : m_client_threads)
            {
                if (client_thread.joinable())
                {
                    client_thread.detach();
                }
            }
        }

        [[nodiscard]] auto abandoned() const -> bool
        {
            return m_abandoned;
        }

        [[nodiscard]] auto port() const -> uint16_t
        {
            return m_bound_port;
        }

      private:
        auto serve() -> void
        {
            while (m_running)
            {
                const auto client = accept(m_listen_socket.load(), nullptr, nullptr);
                if (client == INVALID_SOCKET)
                {
                    if (m_running)
                    {
                        Output::send<LogLevel::Warning>(STR("MCP: accept failed with Winsock error {}.\n"), WSAGetLastError());
                    }
                    break;
                }
                {
                    std::scoped_lock lock{m_client_mutex};
                    if (!m_running)
                    {
                        shutdown(client, SD_BOTH);
                        closesocket(client);
                        break;
                    }
                    m_pending_clients.emplace_back(client);
                }
                m_client_condition.notify_one();
            }
        }
        auto process_client_queue() -> void
        {
            while (true)
            {
                SOCKET client{INVALID_SOCKET};
                {
                    std::unique_lock lock{m_client_mutex};
                    m_client_condition.wait(lock, [this] {
                        return !m_running || !m_pending_clients.empty();
                    });
                    if (m_pending_clients.empty())
                    {
                        if (!m_running)
                        {
                            return;
                        }
                        continue;
                    }
                    client = m_pending_clients.front();
                    m_pending_clients.pop_front();
                    m_active_clients.emplace(client);
                }
                handle_client(client);
                {
                    std::scoped_lock lock{m_client_mutex};
                    m_active_clients.erase(client);
                }
                shutdown(client, SD_BOTH);
                closesocket(client);
            }
        }

        auto handle_client(SOCKET client) -> void
        {
            try
            {
                std::string request;
                request.reserve(8192);
                std::array<char, 8192> buffer{};
                size_t header_end = std::string::npos;
                size_t content_length = 0;

                while (request.size() < maximum_http_request_size)
                {
                    const auto received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
                    if (received <= 0)
                    {
                        return;
                    }
                    request.append(buffer.data(), static_cast<size_t>(received));
                    if (header_end == std::string::npos && (header_end = request.find("\r\n\r\n")) != std::string::npos)
                    {
                        content_length = parse_content_length(std::string_view{request}.substr(0, header_end));
                    }
                    if (header_end != std::string::npos && request.size() >= header_end + 4 + content_length)
                    {
                        break;
                    }
                }

                if (header_end == std::string::npos || request.size() < header_end + 4 + content_length)
                {
                    send_http(client, 400, R"({"error":"incomplete_request"})");
                    return;
                }

                const auto headers = std::string_view{request}.substr(0, header_end);
                const auto first_line_end = headers.find("\r\n");
                const auto request_line = headers.substr(0, first_line_end);
                const auto first_space = request_line.find(' ');
                const auto second_space = request_line.find(' ', first_space + 1);
                if (first_space == std::string_view::npos || second_space == std::string_view::npos)
                {
                    send_http(client, 400, R"({"error":"invalid_request_line"})");
                    return;
                }
                const auto method = request_line.substr(0, first_space);
                const auto path = request_line.substr(first_space + 1, second_space - first_space - 1);

                if (const auto origin = header_value(headers, "origin"); origin && !is_local_origin(*origin))
                {
                    send_http(client, 403, R"({"error":"origin_rejected"})");
                    return;
                }
                if (method == "GET" && path == "/health")
                {
                    Json health = json_object();
                    health["status"] = "ok";
                    health["sessionId"] = m_session_id;
                    health["protocolVersion"] = std::string{protocol_version};
                    send_http(client, 200, serialize_json(health));
                    return;
                }
                if (method == "GET" && path == "/mcp")
                {
                    send_http(client, 405, R"({"error":"method_not_allowed"})");
                    return;
                }
                if (method != "POST" || path != "/mcp")
                {
                    send_http(client, 404, R"({"error":"not_found"})");
                    return;
                }

                const auto body = std::string_view{request}.substr(header_end + 4, content_length);
                auto json_request = parse_json(body);
                const auto* version_member = find_member(json_request, "jsonrpc");
                const auto* method_member = find_member(json_request, "method");
                const auto* id_member = find_member(json_request, "id");
                const auto id_is_valid = !id_member || id_member->is_string() || id_member->is_number() || id_member->is_null();
                if (!json_request.is_object() || !version_member || !version_member->is_string() || version_member->get<std::string>() != "2.0" ||
                    !method_member || !method_member->is_string() || !id_is_valid)
                {
                    send_http(client, 200, serialize_json(json_rpc_error(id_is_valid ? id_member : nullptr, -32600, "Invalid Request")));
                    return;
                }

                const auto rpc_method = method_member->get<std::string>();
                const auto requested_protocol = header_value(headers, "mcp-protocol-version");
                if (rpc_method != "initialize" && (!requested_protocol || trim(*requested_protocol) != protocol_version))
                {
                    send_http(client, 400, R"({"error":"unsupported_protocol_version"})");
                    return;
                }
                if (!id_member)
                {
                    if (rpc_method == "notifications/cancelled")
                    {
                        cancel_request(json_request);
                    }
                    send_http(client, 202, {});
                    return;
                }

                const auto request_key = serialize_json(*id_member);
                const auto cancellation = std::make_shared<std::atomic_bool>(false);
                {
                    std::lock_guard lock{m_request_mutex};
                    m_active_requests[request_key] = cancellation;
                }
                s_current_request_cancellation = cancellation;
                const auto release_request = [&] {
                    s_current_request_cancellation.reset();
                    std::lock_guard lock{m_request_mutex};
                    const auto active = m_active_requests.find(request_key);
                    if (active != m_active_requests.end() && active->second == cancellation)
                    {
                        m_active_requests.erase(active);
                    }
                };

                Json response = json_object();
                response["jsonrpc"] = "2.0";
                response["id"] = *id_member;
                try
                {
                    response["result"] = dispatch(rpc_method, json_request);
                }
                catch (const McpError& error)
                {
                    const auto rpc_code = error.code() == "transport.method_not_found"                                                  ? -32601
                                          : error.code() == "transport.invalid_arguments" || error.code() == "transport.tool_not_found" ? -32602
                                                                                                                                        : -32000;
                    Json rpc_error = json_object();
                    rpc_error["code"] = rpc_code;
                    rpc_error["message"] = error.what();
                    rpc_error["data"] = JsonObject{{"code", error.code()}};
                    response["error"] = std::move(rpc_error);
                }
                catch (...)
                {
                    release_request();
                    throw;
                }
                release_request();
                send_http(client, 200, serialize_json(response));
            }
            catch (const McpError& error)
            {
                if (error.code() == "transport.invalid_json")
                {
                    send_http(client, 200, serialize_json(json_rpc_error(nullptr, -32700, error.what())));
                }
                else
                {
                    Json response = json_object();
                    response["error"] = error.code();
                    response["message"] = error.what();
                    send_http(client, 400, serialize_json(response));
                }
            }
            catch (const std::exception& error)
            {
                Json response = json_object();
                response["error"] = "internal_error";
                response["message"] = error.what();
                send_http(client, 500, serialize_json(response));
            }
        }

        auto cancel_request(const Json& request) -> void
        {
            const auto* params = find_member(request, "params");
            const auto* request_id = params ? find_member(*params, "requestId") : nullptr;
            if (!request_id || (!request_id->is_string() && !request_id->is_number() && !request_id->is_null()))
            {
                return;
            }
            const auto key = serialize_json(*request_id);
            std::lock_guard lock{m_request_mutex};
            if (const auto active = m_active_requests.find(key); active != m_active_requests.end())
            {
                active->second->store(true);
            }
        }

        auto dispatch(const std::string& method, const Json& request) -> Json
        {
            if (method == "initialize")
            {
                Json result = json_object();
                result["protocolVersion"] = std::string{protocol_version};
                auto& capabilities = result["capabilities"] = JsonObject{};
                capabilities["tools"] = JsonObject{{"listChanged", false}};
                capabilities["resources"] = JsonObject{{"subscribe", false}, {"listChanged", false}};
                auto& server_info = result["serverInfo"] = JsonObject{};
                server_info["name"] = "ue4ss-ai-mcp";
                server_info["version"] = fmt::format("{}.{}.{}", UE4SS_LIB_VERSION_MAJOR, UE4SS_LIB_VERSION_MINOR, UE4SS_LIB_VERSION_HOTFIX);
                result["instructions"] = "This endpoint controls the current local UE4SS game session. Capability status is evidence-based; inspect "
                                         "ue4ss_capabilities before mutating runtime state.";
                return result;
            }
            if (method == "ping")
            {
                return json_object();
            }
            if (method == "tools/list")
            {
                return tool_definitions();
            }
            if (method == "tools/call")
            {
                const auto* params = find_member(request, "params");
                if (!params)
                {
                    throw McpError{"transport.invalid_arguments", "tools/call requires params"};
                }
                const auto name = required_string(*params, "name");
                const auto* arguments = find_member(*params, "arguments");
                const Json empty_arguments = JsonObject{};
                return call_tool(name, arguments ? *arguments : empty_arguments);
            }
            if (method == "resources/list")
            {
                return resources_list();
            }
            if (method == "resources/read")
            {
                const auto* params = find_member(request, "params");
                if (!params)
                {
                    throw McpError{"transport.invalid_arguments", "resources/read requires params"};
                }
                return read_resource(required_string(*params, "uri"));
            }
            throw McpError{"transport.method_not_found", fmt::format("Unsupported MCP method '{}'", method)};
        }

        auto call_tool(const std::string& name, const Json& arguments) -> Json
        {
            try
            {
                Json data{};
                if (name == "ue4ss_session_snapshot")
                {
                    data = run_on_event_loop([this] {
                        return session_snapshot();
                    });
                }
                else if (name == "ue4ss_capabilities")
                {
                    data = run_on_event_loop([this] {
                        return capabilities_snapshot();
                    });
                }
                else if (name == "ue4ss_mods_list")
                {
                    data = run_on_event_loop([this] {
                        return mods_snapshot();
                    });
                }
                else if (name == "ue4ss_logs_read")
                {
                    data = read_log(arguments);
                }
                else if (name == "ue4ss_objects_query")
                {
                    data = run_on_game_thread([this, arguments] {
                        return query_objects(arguments);
                    });
                }
                else if (name == "ue4ss_object_inspect")
                {
                    const auto handle = required_string(arguments, "handle");
                    const auto limit = std::clamp<int64_t>(optional_integer(arguments, "limit", 250), 1, 1000);
                    data = run_on_game_thread([this, handle, limit] {
                        return inspect_object(handle, static_cast<size_t>(limit));
                    });
                }
                else if (name == "ue4ss_property_read")
                {
                    const auto handle = required_string(arguments, "handle");
                    const auto property_name = required_string(arguments, "propertyName");
                    data = run_on_game_thread([this, handle, property_name] {
                        return read_property(handle, property_name);
                    });
                }
                else if (name == "ue4ss_property_watch_start")
                {
                    const auto handle = required_string(arguments, "handle");
                    const auto property_name = required_string(arguments, "propertyName");
                    const auto history_limit = required_integer(arguments, "historyLimit", 1, 1000);
                    const auto emit_unchanged = optional_boolean(arguments, "emitUnchanged", false);
                    data = run_on_game_thread([this, handle, property_name, history_limit, emit_unchanged] {
                        return start_property_watch(handle, property_name, static_cast<size_t>(history_limit), emit_unchanged);
                    });
                }
                else if (name == "ue4ss_property_watches_list")
                {
                    data = property_watches_snapshot();
                }
                else if (name == "ue4ss_property_watch_read")
                {
                    data = read_property_watch(arguments);
                }
                else if (name == "ue4ss_property_watch_stop")
                {
                    data = stop_property_watch(required_string(arguments, "watchId"));
                }
                else if (name == "ue4ss_property_write")
                {
                    require_development_mode(name);
                    const auto handle = required_string(arguments, "handle");
                    const auto property_name = required_string(arguments, "propertyName");
                    const auto value = required_string(arguments, "value");
                    data = run_on_game_thread([this, handle, property_name, value] {
                        return write_property(handle, property_name, value);
                    });
                }
                else if (name == "ue4ss_function_call")
                {
                    require_development_mode(name);
                    const auto handle = required_string(arguments, "handle");
                    const auto function_name = required_string(arguments, "functionName");
                    const auto* call_arguments = find_member(arguments, "arguments");
                    const Json values = call_arguments ? *call_arguments : JsonObject{};
                    if (!values.is_object())
                    {
                        throw McpError{"transport.invalid_arguments", "Argument 'arguments' must be an object"};
                    }
                    data = run_on_game_thread([this, handle, function_name, values] {
                        return call_function(handle, function_name, values);
                    });
                }
                else if (name == "ue4ss_asset_load")
                {
                    require_development_mode(name);
                    const auto object_path = required_string(arguments, "objectPath");
                    data = run_on_game_thread([this, object_path] {
                        return load_asset(object_path);
                    });
                }
                else if (name == "ue4ss_console_execute")
                {
                    require_development_mode(name);
                    const auto command = required_string(arguments, "command");
                    data = run_on_game_thread([this, command] {
                        return execute_console(command);
                    });
                }
                else if (name == "ue4ss_hook_register")
                {
                    require_development_mode(name);
                    data = register_process_event_hook(arguments);
                }
                else if (name == "ue4ss_hooks_list")
                {
                    data = hooks_snapshot();
                }
                else if (name == "ue4ss_hook_unregister")
                {
                    data = unregister_process_event_hook(required_string(arguments, "hookId"));
                }
                else if (name == "ue4ss_hook_events_read")
                {
                    data = read_hook_events(arguments);
                }
                else if (name == "ue4ss_lua_execute")
                {
                    require_development_mode(name);
                    const auto mod_name = required_string(arguments, "modName");
                    const auto code = required_string(arguments, "code");
                    data = run_on_event_loop([this, mod_name, code] {
                        return execute_lua(mod_name, code);
                    });
                }
                else if (name == "ue4ss_lua_mod_control")
                {
                    require_development_mode(name);
                    const auto mod_name = required_string(arguments, "name");
                    const auto action = required_string(arguments, "action");
                    if (action != "start" && action != "stop" && action != "restart")
                    {
                        throw McpError{"transport.invalid_arguments", "Argument 'action' must be 'start', 'stop' or 'restart'"};
                    }
                    data = run_on_event_loop([this, mod_name, action] {
                        return control_lua_mod(mod_name, action);
                    });
                }
                else if (name == "ue4ss_lua_mod_reload")
                {
                    require_development_mode(name);
                    const auto mod_name = required_string(arguments, "modName");
                    data = run_on_event_loop([this, mod_name] {
                        return reload_lua_mod(mod_name);
                    });
                }
                else if (name == "ue4ss_lua_debug_attach")
                {
                    require_development_mode(name);
                    const auto mod_name = required_string(arguments, "modName");
                    data = run_on_event_loop([this, mod_name] {
                        return attach_lua_debugger(mod_name);
                    });
                }
                else if (name == "ue4ss_lua_debug_detach")
                {
                    require_development_mode(name);
                    GUI::LuaDebugger::get().continue_execution();
                    const auto mod_name = required_string(arguments, "modName");
                    data = run_on_event_loop([this, mod_name] {
                        return detach_lua_debugger(mod_name);
                    });
                }
                else if (name == "ue4ss_lua_breakpoint_set")
                {
                    require_development_mode(name);
                    const auto source = required_string(arguments, "source");
                    const auto line = required_integer(arguments, "line", 1, std::numeric_limits<int>::max());
                    const auto condition = optional_string(arguments, "condition").value_or("");
                    GUI::LuaDebugger::get().add_breakpoint(source, static_cast<int>(line), condition);
                    data = lua_breakpoints_snapshot();
                }
                else if (name == "ue4ss_lua_breakpoint_remove")
                {
                    require_development_mode(name);
                    const auto source = required_string(arguments, "source");
                    const auto line = required_integer(arguments, "line", 1, std::numeric_limits<int>::max());
                    GUI::LuaDebugger::get().remove_breakpoint(source, static_cast<int>(line));
                    data = lua_breakpoints_snapshot();
                }
                else if (name == "ue4ss_lua_breakpoints_list")
                {
                    data = lua_breakpoints_snapshot();
                }
                else if (name == "ue4ss_lua_debug_status")
                {
                    data = lua_debug_snapshot();
                }
                else if (name == "ue4ss_lua_debug_control")
                {
                    require_development_mode(name);
                    data = control_lua_debugger(required_string(arguments, "action"));
                }
                else if (name == "ue4ss_lua_errors_read")
                {
                    const auto limit = std::clamp<int64_t>(optional_integer(arguments, "limit", 100), 1, 100);
                    data = lua_errors_snapshot(static_cast<size_t>(limit));
                }
                else if (name == "ue4ss_events_read")
                {
                    data = read_journal(arguments, false);
                }
                else if (name == "ue4ss_operations_read")
                {
                    data = read_journal(arguments, true);
                }
                else if (name == "ue4ss_tasks_list")
                {
                    data = tasks_snapshot();
                }
                else if (name == "ue4ss_task_get")
                {
                    data = task_snapshot(required_string(arguments, "taskId"));
                }
                else if (name == "ue4ss_task_cancel")
                {
                    data = cancel_task(required_string(arguments, "taskId"));
                }
                else if (name == "ue4ss_wait_start")
                {
                    data = start_wait_task(arguments);
                }
                else if (name == "ue4ss_dumper_profile")
                {
                    data = m_program.can_process_events() ? run_on_event_loop([this] {
                        return dumper_profile();
                    })
                                                          : dumper_profile();
                }
                else if (name == "ue4ss_dumper_start")
                {
                    require_development_mode(name);
                    data = start_dumper_task(required_string(arguments, "kind"));
                }
                else if (name == "ue4ss_kismet_debug_profile")
                {
                    data = kismet_debug_profile();
                }
                else if (name == "ue4ss_kismet_debug_enable")
                {
                    require_development_mode(name);
                    data = set_kismet_debugger_enabled(true);
                }
                else if (name == "ue4ss_kismet_debug_disable")
                {
                    require_development_mode(name);
                    data = set_kismet_debugger_enabled(false);
                }
                else if (name == "ue4ss_kismet_breakpoint_set")
                {
                    require_development_mode(name);
                    data = set_kismet_breakpoint(required_string(arguments, "functionFullName"),
                                                 static_cast<uint64_t>(required_integer(arguments, "bytecodeOffset", 0, std::numeric_limits<int64_t>::max())),
                                                 true);
                }
                else if (name == "ue4ss_kismet_breakpoint_remove")
                {
                    require_development_mode(name);
                    data = set_kismet_breakpoint(required_string(arguments, "functionFullName"),
                                                 static_cast<uint64_t>(required_integer(arguments, "bytecodeOffset", 0, std::numeric_limits<int64_t>::max())),
                                                 false);
                }
                else if (name == "ue4ss_kismet_breakpoints_list")
                {
                    data = kismet_breakpoints_snapshot();
                }
                else if (name == "ue4ss_kismet_debug_status")
                {
                    data = kismet_debug_snapshot();
                }
                else if (name == "ue4ss_kismet_debug_control")
                {
                    require_development_mode(name);
                    data = control_kismet_debugger(required_string(arguments, "action"));
                }
                else
                {
                    throw McpError{"transport.tool_not_found", fmt::format("Unknown tool '{}'", name)};
                }
                const auto status = name == "ue4ss_wait_start" || name == "ue4ss_dumper_start" ? "accepted" : "succeeded";
                return tool_result(name, status, std::move(data), std::nullopt);
            }
            catch (const McpError& error)
            {
                if (error.code() == "transport.invalid_arguments" || error.code() == "transport.tool_not_found")
                {
                    throw;
                }
                const auto status = error.code() == "operation.indeterminate" ? "indeterminate" : "failed";
                return tool_result(name, status, json_object(), std::pair{error.code(), std::string{error.what()}});
            }
            catch (const std::exception& error)
            {
                return tool_result(name, "failed", json_object(), std::pair{std::string{"operation.failed"}, std::string{error.what()}});
            }
        }

        auto tool_result(const std::string& target, std::string_view status, Json data, std::optional<std::pair<std::string, std::string>> error) -> Json
        {
            Json operation = json_object();
            operation["operationId"] = fmt::format("{}-{}", m_session_id, m_next_operation_id.fetch_add(1));
            operation["sessionId"] = m_session_id;
            operation["target"] = target;
            operation["status"] = std::string{status};
            operation["actualChanges"] = JsonArray{};
            operation["currentState"] = std::move(data);
            Json readback_evidence = json_object();
            readback_evidence["kind"] = "runtime_readback";
            readback_evidence["source"] = target;
            operation["evidence"] = JsonArray{std::move(readback_evidence)};
            if (status == "succeeded" && (target == "ue4ss_lua_execute" || target == "ue4ss_lua_mod_reload" || target == "ue4ss_lua_mod_control" ||
                                          target == "ue4ss_asset_load" || target == "ue4ss_console_execute" || target == "ue4ss_hook_register" ||
                                          target == "ue4ss_hook_unregister" || target == "ue4ss_property_write" || target == "ue4ss_function_call"))
            {
                Json change = json_object();
                change["kind"] = target == "ue4ss_lua_execute"       ? "lua_execution"
                                 : target == "ue4ss_lua_mod_reload"  ? "lua_mod_reload"
                                 : target == "ue4ss_lua_mod_control" ? "lua_mod_control"
                                 : target == "ue4ss_asset_load"      ? "asset_load"
                                 : target == "ue4ss_console_execute" ? "console_execution"
                                 : target == "ue4ss_hook_register"   ? "hook_registration"
                                 : target == "ue4ss_hook_unregister" ? "hook_unregistration"
                                 : target == "ue4ss_property_write"  ? "property_write"
                                                                     : "function_call";
                change["applied"] = true;
                operation["actualChanges"].get<JsonArray>().emplace_back(std::move(change));
            }
            operation["restartRequired"] = error && error->first == "mod.cpp_restart_required";
            if (target == "ue4ss_lua_mod_control")
            {
                operation["persisted"] = false;
            }
            operation["errors"] = JsonArray{};
            operation["suggestedActions"] = JsonArray{};
            if (error)
            {
                Json structured_error = json_object();
                structured_error["code"] = error->first;
                structured_error["stage"] = target;
                structured_error["message"] = error->second;
                structured_error["retryable"] = error->first == "operation.timeout" || error->first == "lifecycle.runtime_not_ready";
                operation["errors"].get<JsonArray>().emplace_back(std::move(structured_error));
            }
            record_operation(operation);
            Json result = json_object();
            result["structuredContent"] = operation;
            Json text_content = json_object();
            text_content["type"] = "text";
            text_content["text"] = serialize_json(operation);
            result["content"] = JsonArray{std::move(text_content)};
            result["isError"] = error.has_value();
            return result;
        }

        auto append_event(std::string type, Json data) -> uint64_t
        {
            std::lock_guard lock{m_journal_mutex};
            const auto sequence = m_next_event_sequence++;
            Json event = json_object();
            event["sequence"] = sequence;
            event["type"] = std::move(type);
            event["sessionId"] = m_session_id;
            event["monotonicMilliseconds"] = static_cast<uint64_t>(GetTickCount64());
            event["data"] = std::move(data);
            m_events.emplace_back(std::move(event));
            if (m_events.size() > max_journal_entries)
            {
                m_events.pop_front();
            }
            return sequence;
        }

        auto record_operation(Json& operation) -> void
        {
            std::lock_guard lock{m_journal_mutex};
            const auto operation_sequence = m_next_operation_sequence++;
            operation["sequence"] = operation_sequence;
            operation["monotonicMilliseconds"] = static_cast<uint64_t>(GetTickCount64());
            m_operations.emplace_back(operation);
            if (m_operations.size() > max_journal_entries)
            {
                m_operations.pop_front();
            }

            Json event_data = json_object();
            event_data["operationId"] = *find_member(operation, "operationId");
            event_data["target"] = *find_member(operation, "target");
            event_data["status"] = *find_member(operation, "status");
            Json event = json_object();
            event["sequence"] = m_next_event_sequence++;
            event["type"] = "operation.completed";
            event["sessionId"] = m_session_id;
            event["monotonicMilliseconds"] = static_cast<uint64_t>(GetTickCount64());
            event["data"] = std::move(event_data);
            m_events.emplace_back(std::move(event));
            if (m_events.size() > max_journal_entries)
            {
                m_events.pop_front();
            }
        }

        auto read_journal(const Json& arguments, bool operations) -> Json
        {
            const auto cursor = static_cast<uint64_t>(optional_integer(arguments, "cursor", 0));
            const auto limit = static_cast<size_t>(std::clamp<int64_t>(optional_integer(arguments, "limit", 100), 1, 1000));
            JsonArray entries{};
            uint64_t oldest_sequence{};
            uint64_t newest_sequence{};
            {
                std::lock_guard lock{m_journal_mutex};
                const auto& journal = operations ? m_operations : m_events;
                const std::string_view sequence_name = operations ? "sequence" : "sequence";
                if (!journal.empty())
                {
                    oldest_sequence = find_member(journal.front(), sequence_name)->get<uint64_t>();
                    newest_sequence = find_member(journal.back(), sequence_name)->get<uint64_t>();
                }
                entries.reserve(std::min(limit, journal.size()));
                for (const auto& entry : journal)
                {
                    if (find_member(entry, sequence_name)->get<uint64_t>() > cursor && entries.size() < limit)
                    {
                        entries.emplace_back(entry);
                    }
                }
            }
            const auto next_cursor = entries.empty() ? cursor : find_member(entries.back(), "sequence")->get<uint64_t>();
            Json result = json_object();
            result[operations ? "operations" : "events"] = std::move(entries);
            result["oldestCursor"] = oldest_sequence > 0 ? oldest_sequence - 1 : 0;
            result["nextCursor"] = next_cursor;
            result["newestSequence"] = newest_sequence;
            result["cursorExpired"] = cursor > 0 && oldest_sequence > 0 && cursor + 1 < oldest_sequence;
            result["truncated"] = next_cursor < newest_sequence;
            return result;
        }

        auto copy_task_state(const std::shared_ptr<AsyncTask>& task) const -> Json
        {
            std::lock_guard lock{task->mutex};
            return task->state;
        }

        auto task_snapshot(const std::string& task_id) const -> Json
        {
            std::shared_ptr<AsyncTask> task;
            {
                std::lock_guard lock{m_tasks_mutex};
                const auto found = m_tasks.find(task_id);
                if (found == m_tasks.end())
                {
                    throw McpError{"task.not_found", fmt::format("Task '{}' was not found", task_id)};
                }
                task = found->second;
            }
            return copy_task_state(task);
        }

        auto tasks_snapshot() const -> Json
        {
            std::vector<std::shared_ptr<AsyncTask>> tasks;
            {
                std::lock_guard lock{m_tasks_mutex};
                tasks.reserve(m_tasks.size());
                for (const auto& [_, task] : m_tasks)
                {
                    tasks.emplace_back(task);
                }
            }
            JsonArray states{};
            states.reserve(tasks.size());
            for (const auto& task : tasks)
            {
                states.emplace_back(copy_task_state(task));
            }
            Json result = json_object();
            result["tasks"] = std::move(states);
            result["count"] = tasks.size();
            return result;
        }

        auto cancel_task(const std::string& task_id) -> Json
        {
            std::shared_ptr<AsyncTask> task;
            {
                std::lock_guard lock{m_tasks_mutex};
                const auto found = m_tasks.find(task_id);
                if (found == m_tasks.end())
                {
                    throw McpError{"task.not_found", fmt::format("Task '{}' was not found", task_id)};
                }
                task = found->second;
            }
            task->cancel_requested.store(true);
            task->worker.request_stop();
            bool cancellation_accepted = true;
            bool non_cooperative = false;
            if (task->execution_state)
            {
                auto queued = PendingState::Queued;
                cancellation_accepted = task->execution_state->compare_exchange_strong(queued, PendingState::Cancelled);
                non_cooperative = !cancellation_accepted && task->execution_state->load() == PendingState::Running;
            }
            {
                std::lock_guard lock{task->mutex};
                task->state["cancelRequested"] = true;
                task->state["cancellationAccepted"] = cancellation_accepted;
                task->state["cancellationMessage"] = cancellation_accepted ? "Cancellation was accepted before execution began"
                                                     : non_cooperative     ? "The dumper entrance has begun and is non-cooperative; execution will continue"
                                                                           : "The task has already completed";
                return task->state;
            }
        }

        auto start_wait_task(const Json& arguments) -> Json
        {
            const auto duration_ms = optional_integer(arguments, "durationMs", -1);
            const auto log_contains = optional_string(arguments, "logContains");
            const auto timeout_ms = optional_integer(arguments, "timeoutMs", duration_ms >= 0 ? std::max<int64_t>(duration_ms + 1000, 1000) : 30000);
            if (duration_ms < 0 && !log_contains)
            {
                throw McpError{"transport.invalid_arguments", "durationMs or logContains is required"};
            }

            const auto task_id = fmt::format("{}-task-{}", m_session_id, m_next_task_id.fetch_add(1));
            auto task = std::make_shared<AsyncTask>();
            task->state["taskId"] = task_id;
            task->state["sessionId"] = m_session_id;
            task->state["kind"] = "wait";
            task->state["status"] = "running";
            task->state["progress"] = 0.0;
            task->state["currentAction"] = log_contains ? "waiting for log text" : "waiting for duration";
            task->state["recentOutput"] = JsonArray{};
            task->state["actualChanges"] = JsonArray{};
            task->state["result"] = json_object();
            task->state["errors"] = JsonArray{};
            {
                std::lock_guard lock{m_tasks_mutex};
                m_tasks.emplace(task_id, task);
            }

            Json started = json_object();
            started["taskId"] = task_id;
            append_event("task.started", started);
            task->worker = std::jthread{[this, task, task_id, duration_ms, log_contains, timeout_ms](std::stop_token stop_token) {
                const auto start = std::chrono::steady_clock::now();
                bool condition_met = false;
                while (!stop_token.stop_requested() && !task->cancel_requested.load())
                {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                    if (duration_ms >= 0 && elapsed >= duration_ms)
                    {
                        condition_met = true;
                    }
                    if (log_contains)
                    {
                        std::ifstream stream{log_path(), std::ios::binary};
                        const std::string contents{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
                        condition_met = contents.find(*log_contains) != std::string::npos;
                    }
                    {
                        std::lock_guard lock{task->mutex};
                        task->state["progress"] = timeout_ms > 0 ? std::min(1.0, static_cast<double>(elapsed) / static_cast<double>(timeout_ms)) : 0.0;
                    }
                    if (condition_met || elapsed >= timeout_ms)
                    {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{50});
                }

                std::string final_status;
                {
                    std::lock_guard lock{task->mutex};
                    if (stop_token.stop_requested() || task->cancel_requested.load())
                    {
                        final_status = "cancelled";
                    }
                    else if (condition_met)
                    {
                        final_status = "succeeded";
                        task->state["progress"] = 1.0;
                        task->state["result"] = JsonObject{{"conditionMet", true}};
                    }
                    else
                    {
                        final_status = "failed";
                        Json error = json_object();
                        error["code"] = "operation.timeout";
                        error["stage"] = "wait";
                        error["message"] = "Wait condition did not become true before timeout";
                        error["retryable"] = true;
                        task->state["errors"].get<JsonArray>().emplace_back(std::move(error));
                    }
                    task->state["status"] = final_status;
                    task->state["currentAction"] = nullptr;
                }
                Json completed = json_object();
                completed["taskId"] = task_id;
                completed["status"] = final_status;
                append_event("task.completed", std::move(completed));
            }};
            return copy_task_state(task);
        }

        auto dumper_working_directory() const -> std::filesystem::path
        {
            return std::filesystem::path{m_program.get_working_directory()};
        }

        auto dumper_relative_output(const std::filesystem::path& output) const -> std::string
        {
            const auto relative = output.lexically_relative(dumper_working_directory());
            if (!relative.empty() && *relative.begin() != "..")
            {
                return relative.generic_string();
            }
            return output.generic_string();
        }

        auto dumper_profile() const -> Json
        {
            const auto working_directory = dumper_working_directory();
            const bool runtime_initialized = Unreal::UnrealInitializer::StaticStorage::bIsInitialized;
            const bool event_loop_available = m_program.can_process_events();
            const bool available = m_development_mode && runtime_initialized && event_loop_available;
            bool world_available = false;
            if (runtime_initialized && event_loop_available)
            {
                try
                {
                    auto* viewport_object = Unreal::UObjectGlobals::FindFirstOf(std::string_view{"GameViewportClient"});
                    auto* viewport = static_cast<Unreal::UGameViewportClient*>(viewport_object);
                    auto* world = viewport ? viewport->GetWorld() : nullptr;
                    world_available = world && !world->IsUnreachable();
                }
                catch (...)
                {
                    world_available = false;
                }
            }
            Json dumpers = json_object();
            const auto add = [&](std::string_view kind, std::string output, std::string prerequisites) {
                Json item = json_object();
                item["available"] = available;
                item["availability"] = available ? "available" : "unavailable";
                item["outputLocation"] = std::move(output);
                item["executor"] = "UE4SS event loop";
                item["thread"] = "UE4SS event-loop thread; the MCP request thread is not blocked";
                item["cancellation"] = "queued-only; non-cooperative after the C++ dumper entrance begins";
                item["concurrency"] = "exclusive across all dumper kinds";
                item["prerequisites"] = std::move(prerequisites);
                dumpers[std::string{kind}] = std::move(item);
            };
            add("objects",
                dumper_relative_output(std::filesystem::path{m_program.get_object_dumper_output_directory()} / UE4SSProgram::m_object_dumper_file_name),
                "DevelopmentMode=1, initialized GUObjectArray and a live UE4SS event loop");
            add("cxxHeaders",
                dumper_relative_output(working_directory / "CXXHeaderDump"),
                "DevelopmentMode=1, initialized reflection data and a live UE4SS event loop; settings may load assets");
            add("uhtHeaders",
                dumper_relative_output(working_directory / "UHTHeaderDump"),
                "DevelopmentMode=1, initialized reflection data and a live UE4SS event loop");
            add("luaTypes",
                dumper_relative_output(working_directory / "Mods" / "shared" / "types"),
                "DevelopmentMode=1, initialized reflection data and a live UE4SS event loop; settings may load assets");
            add("usmap", "*.usmap", "DevelopmentMode=1, initialized reflection data, KismetSystemLibrary and a live UE4SS event loop");
            add("actors", "*-ue4ss_actor_data.csv", "DevelopmentMode=1, a loaded World with actor instances and a live UE4SS event loop");
            add("staticMeshes", "*-ue4ss_static_mesh_data.csv", "DevelopmentMode=1, a loaded World with StaticMeshActor instances and a live UE4SS event loop");
            dumpers["actors"]["available"] = available && world_available;
            dumpers["actors"]["availability"] = available && world_available ? "available" : "unavailable";
            dumpers["staticMeshes"]["available"] = available && world_available;
            dumpers["staticMeshes"]["availability"] = available && world_available ? "available" : "unavailable";

            Json profile = json_object();
            profile["workingDirectory"] = working_directory.generic_string();
            profile["developmentMode"] = m_development_mode;
            profile["runtimeInitialized"] = runtime_initialized;
            profile["eventLoopAvailable"] = event_loop_available;
            profile["worldAvailable"] = world_available;
            profile["dumpers"] = std::move(dumpers);
            profile["artifactLimit"] = 1000;
            return profile;
        }

        auto scan_dumper_files() const -> DumperFileSnapshot
        {
            DumperFileSnapshot files{};
            const auto root = dumper_working_directory();
            std::error_code error;
            std::filesystem::recursive_directory_iterator iterator{root, std::filesystem::directory_options::skip_permission_denied, error};
            const std::filesystem::recursive_directory_iterator end{};
            while (iterator != end)
            {
                error.clear();
                if (iterator->is_regular_file(error) && !error)
                {
                    const auto relative = iterator->path().lexically_relative(root).generic_string();
                    DumperFileState state{};
                    state.size = iterator->file_size(error);
                    if (!error)
                    {
                        const auto modified = iterator->last_write_time(error);
                        if (!error)
                        {
                            const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                                    modified - decltype(modified)::clock::now() + std::chrono::system_clock::now());
                            state.modified_time_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(system_time.time_since_epoch()).count();
                            files[relative] = state;
                        }
                    }
                }
                error.clear();
                iterator.increment(error);
            }
            return files;
        }

        auto dumper_artifact_result(const DumperFileSnapshot& before, const DumperFileSnapshot& after) const -> Json
        {
            struct ChangedFile
            {
                std::string relative_path;
                DumperFileState state;
                bool created{};
            };
            std::vector<ChangedFile> changed{};
            changed.reserve(after.size());
            for (const auto& [relative_path, state] : after)
            {
                const auto previous = before.find(relative_path);
                if (previous == before.end() || previous->second.size != state.size ||
                    previous->second.modified_time_milliseconds != state.modified_time_milliseconds)
                {
                    changed.emplace_back(relative_path, state, previous == before.end());
                }
            }
            std::ranges::sort(changed, {}, &ChangedFile::relative_path);

            JsonArray artifacts{};
            artifacts.reserve(std::min<size_t>(changed.size(), 1000));
            for (size_t index = 0; index < changed.size() && index < 1000; ++index)
            {
                const auto& changed_file = changed[index];
                Json artifact = json_object();
                artifact["relativePath"] = changed_file.relative_path;
                artifact["size"] = changed_file.state.size;
                artifact["modifiedTimeUnixMilliseconds"] = changed_file.state.modified_time_milliseconds;
                artifact["change"] = changed_file.created ? "created" : "modified";
                artifacts.emplace_back(std::move(artifact));
            }

            Json result = json_object();
            result["workingDirectory"] = dumper_working_directory().generic_string();
            result["artifacts"] = std::move(artifacts);
            result["artifactCount"] = changed.size();
            result["truncated"] = changed.size() > 1000;
            return result;
        }

        auto execute_dumper(const std::string& kind) -> void
        {
            const auto working_directory = dumper_working_directory();
            if (kind == "objects")
            {
                UE4SSProgram::dump_all_objects_and_properties(
                        (std::filesystem::path{m_program.get_object_dumper_output_directory()} / UE4SSProgram::m_object_dumper_file_name).native());
            }
            else if (kind == "cxxHeaders")
            {
                m_program.generate_cxx_headers(working_directory / "CXXHeaderDump");
            }
            else if (kind == "uhtHeaders")
            {
                m_program.generate_uht_compatible_headers();
            }
            else if (kind == "luaTypes")
            {
                m_program.generate_lua_types(working_directory / "Mods" / "shared" / "types");
            }
            else if (kind == "usmap")
            {
                OutTheShade::generate_usmap();
            }
            else if (kind == "actors")
            {
                GUI::Dumpers::call_generate_all_actor_file();
            }
            else if (kind == "staticMeshes")
            {
                GUI::Dumpers::call_generate_static_mesh_file();
            }
            else
            {
                throw McpError{"transport.invalid_arguments", fmt::format("Unsupported dumper kind '{}'", kind)};
            }
        }

        auto is_dumper_kind(std::string_view kind) const -> bool
        {
            static constexpr std::array<std::string_view, 7> kinds{"objects", "cxxHeaders", "uhtHeaders", "luaTypes", "usmap", "actors", "staticMeshes"};
            return std::ranges::find(kinds, kind) != kinds.end();
        }

        auto is_expected_dumper_artifact(std::string_view kind, std::string_view relative_path) const -> bool
        {
            if (kind == "objects")
            {
                return relative_path ==
                       dumper_relative_output(std::filesystem::path{m_program.get_object_dumper_output_directory()} / UE4SSProgram::m_object_dumper_file_name);
            }
            if (kind == "cxxHeaders")
            {
                return relative_path.starts_with("CXXHeaderDump/");
            }
            if (kind == "uhtHeaders")
            {
                return relative_path.starts_with("UHTHeaderDump/");
            }
            if (kind == "luaTypes")
            {
                return relative_path.starts_with("Mods/shared/types/");
            }
            if (kind == "usmap")
            {
                return relative_path.find('/') == std::string_view::npos && relative_path.ends_with(".usmap");
            }
            if (kind == "actors")
            {
                return relative_path.find('/') == std::string_view::npos && relative_path.ends_with("-ue4ss_actor_data.csv");
            }
            return kind == "staticMeshes" && relative_path.find('/') == std::string_view::npos && relative_path.ends_with("-ue4ss_static_mesh_data.csv");
        }

        auto count_expected_dumper_artifacts(std::string_view kind, const DumperFileSnapshot& before, const DumperFileSnapshot& after) const -> size_t
        {
            size_t count{};
            for (const auto& [relative_path, state] : after)
            {
                const auto previous = before.find(relative_path);
                const bool changed = previous == before.end() || previous->second.size != state.size ||
                                     previous->second.modified_time_milliseconds != state.modified_time_milliseconds;
                if (changed && is_expected_dumper_artifact(kind, relative_path))
                {
                    ++count;
                }
            }
            return count;
        }

        auto finalize_dumper_task(const std::shared_ptr<AsyncTask>& task,
                                  const std::string& task_id,
                                  const std::string& kind,
                                  const DumperFileSnapshot& before,
                                  std::string status,
                                  std::optional<std::string> error_message = std::nullopt) -> void
        {
            const auto after = scan_dumper_files();
            const auto expected_artifact_count = count_expected_dumper_artifacts(kind, before, after);
            if (status == "succeeded" && expected_artifact_count == 0)
            {
                status = "failed";
                error_message = fmt::format("The {} dumper returned without creating or modifying an expected output file", kind);
            }
            auto result = dumper_artifact_result(before, after);
            result["expectedArtifactCount"] = expected_artifact_count;
            const auto artifacts = *find_member(result, "artifacts");
            {
                std::lock_guard lock{task->mutex};
                task->state["status"] = status;
                if (status == "succeeded")
                {
                    task->state["progress"] = 1.0;
                }
                task->state["currentAction"] = nullptr;
                task->state["cancellable"] = false;
                task->state["executionPhase"] = status;
                task->state["result"] = std::move(result);
                task->state["actualChanges"] = artifacts;
                if (error_message)
                {
                    Json error = json_object();
                    error["code"] = "dumper.execution_failed";
                    error["stage"] = kind;
                    error["message"] = *error_message;
                    error["retryable"] = false;
                    task->state["errors"].get<JsonArray>().emplace_back(std::move(error));
                }
                Json output = json_object();
                output["message"] = status == "succeeded"   ? "Dumper completed and filesystem evidence was collected"
                                    : status == "cancelled" ? "Dumper was cancelled before its C++ entrance began"
                                                            : "Dumper failed; filesystem evidence was still collected";
                task->state["recentOutput"].get<JsonArray>().emplace_back(std::move(output));
            }
            m_dumper_task_active.store(false);
            Json completed = json_object();
            completed["taskId"] = task_id;
            completed["status"] = std::move(status);
            append_event("task.completed", std::move(completed));
        }

        auto start_dumper_task(const std::string& kind) -> Json
        {
            if (!is_dumper_kind(kind))
            {
                throw McpError{"transport.invalid_arguments", fmt::format("Unsupported dumper kind '{}'", kind)};
            }
            if (!Unreal::UnrealInitializer::StaticStorage::bIsInitialized)
            {
                throw McpError{"lifecycle.runtime_not_ready", "Unreal runtime is not initialized"};
            }
            if (!m_program.can_process_events())
            {
                throw McpError{"lifecycle.runtime_not_ready", "UE4SS event loop is not ready"};
            }
            bool expected = false;
            if (!m_dumper_task_active.compare_exchange_strong(expected, true))
            {
                throw McpError{"dumper.busy", "Another dumper task is queued or running"};
            }

            const auto task_id = fmt::format("{}-task-{}", m_session_id, m_next_task_id.fetch_add(1));
            auto task = std::make_shared<AsyncTask>();
            task->execution_state = std::make_shared<std::atomic<PendingState>>(PendingState::Queued);
            task->state["taskId"] = task_id;
            task->state["sessionId"] = m_session_id;
            task->state["kind"] = kind;
            task->state["taskType"] = "dumper";
            task->state["status"] = "running";
            task->state["progress"] = 0.0;
            task->state["currentAction"] = "scanning working directory before dump";
            task->state["executionPhase"] = "queued";
            task->state["cancellable"] = true;
            task->state["cancellationPolicy"] = "queued-only; non-cooperative after the C++ dumper entrance begins";
            task->state["cancelRequested"] = false;
            task->state["recentOutput"] = JsonArray{};
            task->state["actualChanges"] = JsonArray{};
            task->state["result"] = json_object();
            task->state["errors"] = JsonArray{};
            {
                std::lock_guard lock{m_tasks_mutex};
                m_tasks.emplace(task_id, task);
            }

            Json queued = json_object();
            queued["taskId"] = task_id;
            queued["kind"] = kind;
            append_event("task.queued", std::move(queued));
            try
            {
                task->worker = std::jthread{[this, task, task_id, kind](std::stop_token stop_token) {
                    const auto before = scan_dumper_files();
                    auto execution_state = task->execution_state;
                    if (stop_token.stop_requested() || task->cancel_requested.load())
                    {
                        auto expected_state = PendingState::Queued;
                        execution_state->compare_exchange_strong(expected_state, PendingState::Cancelled);
                    }

                    auto completion = std::make_shared<std::promise<void>>();
                    auto future = completion->get_future();
                    if (execution_state->load() == PendingState::Queued)
                    {
                        {
                            std::lock_guard lock{task->mutex};
                            task->state["currentAction"] = "queued on UE4SS event loop";
                            task->state["progress"] = 0.25;
                        }
                        try
                        {
                            m_program.queue_event([this, task, task_id, kind, execution_state, completion] {
                                auto expected_state = PendingState::Queued;
                                if (!execution_state->compare_exchange_strong(expected_state, PendingState::Running))
                                {
                                    return;
                                }
                                {
                                    std::lock_guard lock{task->mutex};
                                    task->state["currentAction"] = fmt::format("running {} dumper", kind);
                                    task->state["executionPhase"] = "running";
                                    task->state["progress"] = 0.5;
                                    task->state["cancellable"] = false;
                                }
                                Json running = json_object();
                                running["taskId"] = task_id;
                                running["kind"] = kind;
                                append_event("task.started", std::move(running));
                                try
                                {
                                    execute_dumper(kind);
                                    completion->set_value();
                                }
                                catch (...)
                                {
                                    completion->set_exception(std::current_exception());
                                }
                                execution_state->store(PendingState::Completed);
                            });
                        }
                        catch (...)
                        {
                            execution_state->store(PendingState::Completed);
                            completion->set_exception(std::current_exception());
                        }
                    }

                    while (execution_state->load() == PendingState::Queued)
                    {
                        if (stop_token.stop_requested() || task->cancel_requested.load())
                        {
                            auto expected_state = PendingState::Queued;
                            execution_state->compare_exchange_strong(expected_state, PendingState::Cancelled);
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds{25});
                    }
                    if (execution_state->load() == PendingState::Cancelled)
                    {
                        finalize_dumper_task(task, task_id, kind, before, "cancelled");
                        return;
                    }

                    while (execution_state->load() == PendingState::Running)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds{25});
                    }
                    try
                    {
                        future.get();
                        finalize_dumper_task(task, task_id, kind, before, "succeeded");
                    }
                    catch (const std::exception& error)
                    {
                        finalize_dumper_task(task, task_id, kind, before, "failed", error.what());
                    }
                    catch (...)
                    {
                        finalize_dumper_task(task, task_id, kind, before, "failed", "The dumper threw a non-standard exception");
                    }
                }};
            }
            catch (...)
            {
                {
                    std::lock_guard lock{m_tasks_mutex};
                    m_tasks.erase(task_id);
                }
                m_dumper_task_active.store(false);
                throw;
            }
            return copy_task_state(task);
        }

        auto stop_tasks() -> void
        {
            std::vector<std::shared_ptr<AsyncTask>> tasks;
            {
                std::lock_guard lock{m_tasks_mutex};
                for (const auto& [_, task] : m_tasks)
                {
                    task->cancel_requested.store(true);
                    task->worker.request_stop();
                    tasks.emplace_back(task);
                }
            }
            for (const auto& task : tasks)
            {
                if (task->worker.joinable() && task->worker.get_id() != std::this_thread::get_id())
                {
                    task->worker.join();
                }
            }
        }

        template <typename Callable>
        auto run_on_event_loop(Callable callable) -> Json
        {
            if (!m_program.can_process_events())
            {
                throw McpError{"lifecycle.runtime_not_ready", "UE4SS event loop is not ready"};
            }

            auto promise = std::make_shared<std::promise<Json>>();
            auto state = std::make_shared<std::atomic<PendingState>>(PendingState::Queued);
            auto future = promise->get_future();
            m_program.queue_event([promise, state, callable = std::move(callable)]() mutable {
                auto expected = PendingState::Queued;
                if (!state->compare_exchange_strong(expected, PendingState::Running))
                {
                    return;
                }
                try
                {
                    promise->set_value(callable());
                }
                catch (...)
                {
                    promise->set_exception(std::current_exception());
                }
                state->store(PendingState::Completed);
            });
            return await_scheduled_result(future, state, "UE4SS event loop");
        }

        template <typename Callable>
        auto run_on_game_thread(Callable callable) -> Json
        {
            if (m_game_thread_callback_id == Unreal::Hook::ERROR_ID)
            {
                throw McpError{"thread.unavailable", "UEngine::Tick game-thread callback is unavailable"};
            }

            auto promise = std::make_shared<std::promise<Json>>();
            auto state = std::make_shared<std::atomic<PendingState>>(PendingState::Queued);
            auto future = promise->get_future();
            {
                std::lock_guard lock{m_game_thread_queue_mutex};
                m_game_thread_queue.emplace_back([promise, state, callable = std::move(callable)]() mutable {
                    auto expected = PendingState::Queued;
                    if (!state->compare_exchange_strong(expected, PendingState::Running))
                    {
                        return;
                    }
                    try
                    {
                        promise->set_value(callable());
                    }
                    catch (...)
                    {
                        promise->set_exception(std::current_exception());
                    }
                    state->store(PendingState::Completed);
                });
            }
            return await_scheduled_result(future, state, "game thread");
        }

        auto await_scheduled_result(std::future<Json>& future, const std::shared_ptr<std::atomic<PendingState>>& state, std::string_view executor) const -> Json
        {
            const auto deadline = std::chrono::steady_clock::now() + m_operation_timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (future.wait_for(std::chrono::milliseconds{50}) == std::future_status::ready)
                {
                    return future.get();
                }
                if (s_current_request_cancellation && s_current_request_cancellation->load())
                {
                    auto expected = PendingState::Queued;
                    if (state->compare_exchange_strong(expected, PendingState::Cancelled))
                    {
                        throw McpError{"operation.cancelled", fmt::format("Operation was cancelled before it ran on the {}", executor)};
                    }
                    if (state->load() == PendingState::Completed && future.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready)
                    {
                        return future.get();
                    }
                    throw McpError{"operation.indeterminate", fmt::format("Operation cancellation was requested after it began on the {}", executor)};
                }
            }

            auto expected = PendingState::Queued;
            if (state->compare_exchange_strong(expected, PendingState::Cancelled))
            {
                throw McpError{"operation.timeout", fmt::format("Operation was cancelled before it ran on the {}", executor)};
            }
            if (state->load() == PendingState::Completed && future.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready)
            {
                return future.get();
            }
            throw McpError{"operation.indeterminate", fmt::format("Operation began on the {} but did not complete before the timeout", executor)};
        }

        auto process_game_thread_queue() -> void
        {
            if (!m_running)
            {
                return;
            }
            std::vector<std::function<void()>> ready{};
            {
                std::lock_guard lock{m_game_thread_queue_mutex};
                const auto count = std::min<size_t>(m_game_thread_queue.size(), 5);
                ready.reserve(count);
                std::move(m_game_thread_queue.begin(), m_game_thread_queue.begin() + static_cast<std::ptrdiff_t>(count), std::back_inserter(ready));
                m_game_thread_queue.erase(m_game_thread_queue.begin(), m_game_thread_queue.begin() + static_cast<std::ptrdiff_t>(count));
            }
            for (auto& operation : ready)
            {
                operation();
            }
            sample_property_watches();
        }

        auto log_cursor() const -> uint64_t
        {
            std::error_code error;
            const auto size = std::filesystem::file_size(log_path(), error);
            return error ? 0 : static_cast<uint64_t>(size);
        }

        auto execute_console(const std::string& command) -> Json
        {
            auto* viewport = static_cast<Unreal::UGameViewportClient*>(Unreal::UObjectGlobals::FindFirstOf(std::string_view{"GameViewportClient"}));
            if (!viewport)
            {
                throw McpError{"lifecycle.viewport_not_ready", "No live GameViewportClient was found"};
            }

            constexpr size_t maximum_output_bytes = 65536;
            BoundedOutputCapture output_device{maximum_output_bytes};
            const auto log_cursor_before = log_cursor();
            const auto wide_command = ensure_str(command);
            const auto handled = viewport->ProcessConsoleExec(wide_command.c_str(), output_device, nullptr);
            const auto log_cursor_after = log_cursor();
            Json result = json_object();
            result["command"] = command;
            result["handled"] = handled;
            result["outputCaptureAvailable"] = true;
            result["output"] = output_device.output();
            result["outputTruncated"] = output_device.truncated();
            result["logCursorBefore"] = log_cursor_before;
            result["logCursorAfter"] = log_cursor_after;
            return result;
        }

        auto register_process_event_hook(const Json& arguments) -> Json
        {
            const auto function_full_name = required_string(arguments, "functionFullName");
            const auto phase = optional_string(arguments, "phase").value_or("pre");
            if (phase != "pre" && phase != "post")
            {
                throw McpError{"transport.invalid_arguments", "Argument 'phase' must be 'pre' or 'post'"};
            }
            const auto object_class = optional_string(arguments, "objectClass");
            const auto hook_id = fmt::format("hook-{}", m_next_hook_id.fetch_add(1));

            auto callback = [this, hook_id, function_full_name, object_class, phase](Unreal::Hook::TCallbackIterationData<void>&,
                                                                                     Unreal::UObject* object,
                                                                                     Unreal::UFunction* function,
                                                                                     void* parameters) {
                if (!m_running || !object || !function)
                {
                    return;
                }
                try
                {
                    const auto observed_function = to_string(function->GetFullName());
                    if (observed_function != function_full_name)
                    {
                        return;
                    }
                    auto* object_class_ptr = object->GetClassPrivate();
                    const auto observed_class = object_class_ptr ? to_string(object_class_ptr->GetNamePrivate().ToString()) : std::string{};
                    if (object_class && observed_class != *object_class)
                    {
                        return;
                    }

                    Json event = json_object();
                    const auto sequence = m_next_hook_event_sequence.fetch_add(1);
                    event["sequence"] = sequence;
                    event["hookId"] = hook_id;
                    event["phase"] = phase;
                    event["functionFullName"] = observed_function;
                    event["objectClass"] = observed_class;
                    event["objectFullName"] = to_string(object->GetFullName());
                    event["objectAddress"] = fmt::format("0x{:X}", reinterpret_cast<uintptr_t>(object));
                    event["threadId"] = GetCurrentThreadId();
                    event["monotonicMilliseconds"] = GetTickCount64();

                    Json parameter_values = json_object();
                    uint64_t parameter_count{};
                    bool parameters_truncated{};
                    if (parameters)
                    {
                        constexpr uint64_t max_parameters = 128;
                        constexpr size_t max_parameter_value_length = 4096;
                        for (auto* property : Unreal::TFieldRange<Unreal::FProperty>(function, Unreal::EFieldIterationFlags::IncludeDeprecated))
                        {
                            if (!property->HasAnyPropertyFlags(Unreal::CPF_Parm))
                            {
                                continue;
                            }
                            if (parameter_count >= max_parameters)
                            {
                                parameters_truncated = true;
                                break;
                            }
                            Unreal::FString exported{};
                            auto* value = property->ContainerPtrToValuePtr<void>(parameters);
                            property->ExportTextItem(exported, value, value, object, 0);
                            auto text = to_string(StringType{*exported});
                            if (text.size() > max_parameter_value_length)
                            {
                                text.resize(max_parameter_value_length);
                                parameters_truncated = true;
                            }
                            parameter_values[to_string(property->GetName())] = std::move(text);
                            ++parameter_count;
                        }
                    }
                    event["parameters"] = std::move(parameter_values);
                    event["parameterCount"] = parameter_count;
                    event["parametersTruncated"] = parameters_truncated;

                    std::lock_guard lock{m_hook_mutex};
                    m_hook_events.emplace_back(std::move(event));
                    if (m_hook_events.size() > max_hook_events)
                    {
                        m_hook_events.pop_front();
                    }
                }
                catch (const std::exception& error)
                {
                    Output::send<LogLevel::Warning>(STR("MCP: ProcessEvent observer '{}' could not copy an event: {}\n"),
                                                    ensure_str(hook_id),
                                                    ensure_str(error.what()));
                }
            };

            const Unreal::Hook::FCallbackOptions options{false, true, STR("UE4SS"), STR("McpProcessEventObserver")};
            const auto callback_id = phase == "pre" ? Unreal::Hook::RegisterProcessEventPreCallback(std::move(callback), options)
                                                    : Unreal::Hook::RegisterProcessEventPostCallback(std::move(callback), options);
            if (callback_id == Unreal::Hook::ERROR_ID)
            {
                throw McpError{"hook.registration_failed", "ProcessEvent observer registration failed; inspect UE4SS.log for hook readiness evidence"};
            }
            {
                std::lock_guard lock{m_hook_mutex};
                m_process_event_hooks.emplace(hook_id, ProcessEventHook{callback_id, function_full_name, object_class, phase});
            }

            Json result = json_object();
            result["hookId"] = hook_id;
            result["functionFullName"] = function_full_name;
            result["phase"] = phase;
            if (object_class)
            {
                result["objectClass"] = *object_class;
            }
            result["registered"] = true;
            return result;
        }

        auto hooks_snapshot() -> Json
        {
            JsonArray hooks{};
            std::lock_guard lock{m_hook_mutex};
            hooks.reserve(m_process_event_hooks.size());
            for (const auto& [hook_id, hook] : m_process_event_hooks)
            {
                Json item = json_object();
                item["hookId"] = hook_id;
                item["functionFullName"] = hook.function_full_name;
                item["phase"] = hook.phase;
                if (hook.object_class)
                {
                    item["objectClass"] = *hook.object_class;
                }
                hooks.emplace_back(std::move(item));
            }
            Json result = json_object();
            result["hooks"] = std::move(hooks);
            result["count"] = m_process_event_hooks.size();
            return result;
        }

        auto unregister_process_event_hook(const std::string& hook_id) -> Json
        {
            Unreal::Hook::GlobalCallbackId callback_id{Unreal::Hook::ERROR_ID};
            {
                std::lock_guard lock{m_hook_mutex};
                const auto hook = m_process_event_hooks.find(hook_id);
                if (hook != m_process_event_hooks.end())
                {
                    callback_id = hook->second.callback_id;
                    m_process_event_hooks.erase(hook);
                }
            }
            const auto removed = callback_id != Unreal::Hook::ERROR_ID && Unreal::Hook::UnregisterCallback(callback_id);
            Json result = json_object();
            result["hookId"] = hook_id;
            result["registered"] = false;
            result["removed"] = removed;
            return result;
        }

        auto read_hook_events(const Json& arguments) -> Json
        {
            const auto cursor = static_cast<uint64_t>(std::max<int64_t>(optional_integer(arguments, "cursor", 0), 0));
            const auto limit = static_cast<size_t>(std::clamp<int64_t>(optional_integer(arguments, "limit", 100), 1, 1000));
            JsonArray events{};
            uint64_t oldest_sequence = m_next_hook_event_sequence.load();
            uint64_t newest_sequence = oldest_sequence > 0 ? oldest_sequence - 1 : 0;
            {
                std::lock_guard lock{m_hook_mutex};
                if (!m_hook_events.empty())
                {
                    oldest_sequence = find_member(m_hook_events.front(), "sequence")->get<uint64_t>();
                    newest_sequence = find_member(m_hook_events.back(), "sequence")->get<uint64_t>();
                }
                events.reserve(std::min(limit, m_hook_events.size()));
                for (const auto& event : m_hook_events)
                {
                    const auto sequence = find_member(event, "sequence")->get<uint64_t>();
                    if (sequence > cursor && events.size() < limit)
                    {
                        events.emplace_back(event);
                    }
                }
            }
            const auto next_cursor = events.empty() ? cursor : find_member(events.back(), "sequence")->get<uint64_t>();
            Json result = json_object();
            result["events"] = std::move(events);
            result["oldestCursor"] = oldest_sequence > 0 ? oldest_sequence - 1 : 0;
            result["nextCursor"] = next_cursor;
            result["newestSequence"] = newest_sequence;
            result["cursorExpired"] = cursor > 0 && oldest_sequence > 0 && cursor + 1 < oldest_sequence;
            result["truncated"] = next_cursor < newest_sequence;
            return result;
        }

        auto session_snapshot() -> Json
        {
            Json snapshot = json_object();
            snapshot["sessionId"] = m_session_id;
            snapshot["processId"] = static_cast<uint64_t>(GetCurrentProcessId());
            snapshot["executable"] = executable_path();
            snapshot["ue4ssVersion"] = fmt::format("{}.{}.{}.{}.{}",
                                                   UE4SS_LIB_VERSION_MAJOR,
                                                   UE4SS_LIB_VERSION_MINOR,
                                                   UE4SS_LIB_VERSION_HOTFIX,
                                                   UE4SS_LIB_VERSION_PRERELEASE,
                                                   UE4SS_LIB_VERSION_BETA);
            snapshot["ue4ssBuild"] = UE4SS_LIB_BUILD_GITSHA;
            snapshot["unrealVersion"] = fmt::format("{}.{}", Unreal::Version::Major, Unreal::Version::Minor);
            snapshot["programStarted"] = m_program.is_program_started();
            std::ostringstream event_loop_thread;
            event_loop_thread << m_program.get_event_loop_thread_id();
            snapshot["eventLoopThread"] = std::move(event_loop_thread).str();
            snapshot["mcpDevelopmentMode"] = m_development_mode;

            try
            {
                snapshot["world"] = run_on_game_thread([this] {
                    return world_snapshot();
                });
            }
            catch (const McpError& error)
            {
                Json world = json_object();
                world["available"] = false;
                world["name"] = nullptr;
                world["path"] = nullptr;
                world["probeError"] = error.what();
                snapshot["world"] = std::move(world);
            }

            size_t started_mods = 0;
            for (const auto& mod : m_program.m_mods)
            {
                started_mods += mod->is_started() ? 1U : 0U;
            }
            snapshot["mods"] = JsonObject{{"discovered", static_cast<uint64_t>(m_program.m_mods.size())}, {"started", static_cast<uint64_t>(started_mods)}};
            return snapshot;
        }

        auto world_snapshot() -> Json
        {
            Json world = json_object();
            world["available"] = false;
            world["name"] = nullptr;
            world["path"] = nullptr;
            try
            {
                auto* viewport_object = Unreal::UObjectGlobals::FindFirstOf(std::string_view{"GameViewportClient"});
                auto* viewport = static_cast<Unreal::UGameViewportClient*>(viewport_object);
                auto* current_world = viewport ? viewport->GetWorld() : nullptr;
                if (current_world != m_last_world)
                {
                    Json transition = json_object();
                    transition["previousAddress"] = fmt::format("0x{:X}", reinterpret_cast<uintptr_t>(m_last_world));
                    transition["currentAddress"] = fmt::format("0x{:X}", reinterpret_cast<uintptr_t>(current_world));
                    m_last_world = current_world;
                    ++m_world_generation;
                    transition["generation"] = m_world_generation;
                    append_event("world.changed", std::move(transition));
                }
                if (current_world && !current_world->IsUnreachable())
                {
                    world["available"] = true;
                    world["name"] = to_string(current_world->GetName());
                    world["path"] = to_string(current_world->GetPathName());
                    world["address"] = fmt::format("0x{:X}", reinterpret_cast<uintptr_t>(current_world));
                    world["generation"] = m_world_generation;
                }
            }
            catch (const std::exception& error)
            {
                world["probeError"] = error.what();
            }
            return world;
        }

        auto capabilities_snapshot() -> Json
        {
            Json result = json_object();
            JsonArray capabilities{};
            const auto add = [&](std::string name, std::string status, std::string evidence, std::string missing = {}) {
                Json capability = json_object();
                capability["name"] = std::move(name);
                capability["status"] = std::move(status);
                capability["evidence"] = std::move(evidence);
                capability["missing"] = std::move(missing);
                capabilities.emplace_back(std::move(capability));
            };

            add("session.snapshot", "available", "MCP endpoint is executing inside the initialized UE4SS process");
            add("mods.list", m_program.can_process_events() ? "available" : "unavailable", "UE4SS event-loop state", "running event loop");
            add("logs.read", std::filesystem::exists(log_path()) ? "available" : "degraded", log_path().generic_string(), "UE4SS.log file");
            add("runtime.objects.find",
                Unreal::UObjectArray::GetGUObjectArrayAddress() && m_game_thread_callback_id != Unreal::Hook::ERROR_ID ? "available" : "unavailable",
                Unreal::UObjectArray::GetGUObjectArrayAddress() ? "GUObjectArray address is initialized" : "GUObjectArray address is null",
                m_game_thread_callback_id != Unreal::Hook::ERROR_ID ? "initialized GUObjectArray" : "UEngine::Tick game-thread callback");
            const bool asset_load_prerequisites =
                    m_development_mode && Unreal::UObjectArray::GetGUObjectArrayAddress() && m_game_thread_callback_id != Unreal::Hook::ERROR_ID;
            add("runtime.asset.load",
                asset_load_prerequisites ? "available" : "unavailable",
                asset_load_prerequisites ? "exact AssetRegistry GetAssetByObjectPath/GetAsset is scheduled on the game thread; registry is resolved per call"
                                         : "asset-load structural prerequisites are incomplete",
                !m_development_mode ? "DevelopmentMode=1"
                                    : (!Unreal::UObjectArray::GetGUObjectArrayAddress() ? "initialized GUObjectArray" : "UEngine::Tick game-thread callback"));

            const bool has_started_lua_mod = std::ranges::any_of(m_program.m_mods, [](const auto& mod) {
                const auto* lua_mod = dynamic_cast<const LuaMod*>(mod.get());
                return lua_mod && lua_mod->is_started() && lua_mod->m_hook_lua;
            });
            const bool has_known_lua_path = !m_stopped_lua_mod_paths.empty() || std::ranges::any_of(m_program.m_mods, [](const auto& mod) {
                return dynamic_cast<const LuaMod*>(mod.get()) != nullptr;
            });
            add("lua.execute",
                m_development_mode && has_started_lua_mod && m_game_thread_callback_id != Unreal::Hook::ERROR_ID ? "available" : "degraded",
                m_development_mode ? "development mode enabled" : "development mode disabled",
                has_started_lua_mod ? "UEngine::Tick game-thread callback" : "DevelopmentMode=1 and a started Lua mod hook state");
            add("lua.reload", m_development_mode ? "available" : "unavailable", "UE4SS Lua reinstall path", "DevelopmentMode=1");
            const bool lua_control_executor_ready = m_development_mode && m_program.can_process_events();
            add("lua.mod.control",
                lua_control_executor_ready && has_known_lua_path ? "available" : (lua_control_executor_ready ? "degraded" : "unavailable"),
                has_known_lua_path ? "known Lua mod path with UE4SS runtime start/uninstall/reinstall paths"
                                   : "no Lua mod path is currently known to this MCP session",
                !m_development_mode ? "DevelopmentMode=1" : (!m_program.can_process_events() ? "running UE4SS event loop" : "known Lua mod path"));
            add("lua.debug",
                m_development_mode && has_started_lua_mod && m_game_thread_callback_id != Unreal::Hook::ERROR_ID ? "available" : "degraded",
                "hook-state line debugger with conditional breakpoints, paused locals and stepping",
                "DevelopmentMode=1, a started Lua mod and a resolved UEngine::Tick callback");
            const auto* kismet_api = kismet_debugger_api();
            add("kismet.debug",
                m_development_mode && kismet_api ? "available" : "degraded",
                kismet_api ? "KismetDebuggerMod MCP API version 1 is loaded" : "KismetDebuggerMod MCP API version 1 is unavailable",
                m_development_mode ? "loaded compatible KismetDebuggerMod" : "DevelopmentMode=1");
            add("runtime.console.execute",
                m_development_mode && m_game_thread_callback_id != Unreal::Hook::ERROR_ID ? "available" : "unavailable",
                m_game_thread_callback_id != Unreal::Hook::ERROR_ID
                        ? "UEngine::Tick game-thread callback is registered; bounded ProcessConsoleExec output capture is installed per command"
                        : "UEngine::Tick game-thread callback is unavailable",
                m_development_mode ? "live GameViewportClient" : "DevelopmentMode=1");
            add("runtime.process_event.observe",
                m_development_mode && UE4SSRuntime::IsProcessEventAvailable() ? "available" : "unavailable",
                UE4SSRuntime::IsProcessEventAvailable() ? "UObject::ProcessEventInternal is resolved" : "UObject::ProcessEventInternal is unresolved",
                m_development_mode ? "resolved ProcessEvent Hook" : "DevelopmentMode=1");
            add("blueprint.runtime", "degraded", "generic UObject discovery is available", "structured Blueprint creation/editor adapter");
            add("cpp.build", "unavailable", "no matching external toolchain adapter connected", "C++ workspace/toolchain adapter");
            add("native.debug", "unavailable", "no native debugger adapter connected", "debugger and symbol adapter");
            add("runtime.memory", "unavailable", "memory tools are not exposed by this server build", "memory operation implementation");

            result["capabilities"] = std::move(capabilities);
            result["refreshedAtMonotonicMs"] = static_cast<uint64_t>(GetTickCount64());
            return result;
        }

        auto mods_snapshot() -> Json
        {
            Json result = json_object();
            JsonArray mods{};
            for (const auto& mod : m_program.m_mods)
            {
                Json item = json_object();
                item["id"] = static_cast<uint64_t>(mod->get_id());
                item["name"] = to_string(mod->get_name());
                item["path"] = mod->get_path().generic_string();
                item["type"] = dynamic_cast<LuaMod*>(mod.get()) ? "lua" : (dynamic_cast<CppMod*>(mod.get()) ? "cpp" : "unknown");
                item["installable"] = mod->is_installable();
                item["installed"] = mod->is_installed();
                item["started"] = mod->is_started();
                mods.emplace_back(std::move(item));
            }
            result["mods"] = std::move(mods);
            result["count"] = static_cast<uint64_t>(m_program.m_mods.size());
            return result;
        }

        auto read_log(const Json& arguments) -> Json
        {
            const auto cursor_value = optional_integer(arguments, "cursor", 0);
            const auto max_bytes_value = std::clamp<int64_t>(optional_integer(arguments, "maxBytes", 16384), 1, 65536);
            if (cursor_value < 0)
            {
                throw McpError{"transport.invalid_arguments", "cursor must not be negative"};
            }

            const auto path = log_path();
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                throw McpError{"artifact.not_found", fmt::format("UE4SS log is not readable at '{}'", path.generic_string())};
            }
            stream.seekg(0, std::ios::end);
            const auto size = static_cast<uint64_t>(stream.tellg());
            const auto cursor = std::min<uint64_t>(static_cast<uint64_t>(cursor_value), size);
            stream.seekg(static_cast<std::streamoff>(cursor), std::ios::beg);
            std::string chunk(static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(max_bytes_value), size - cursor)), '\0');
            stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            chunk.resize(static_cast<size_t>(stream.gcount()));

            Json result = json_object();
            result["path"] = path.generic_string();
            result["cursor"] = cursor;
            result["nextCursor"] = cursor + chunk.size();
            result["endOfFile"] = cursor + chunk.size() >= size;
            result["totalBytesObserved"] = size;
            result["text"] = std::move(chunk);
            return result;
        }

        auto resolve_object_handle(const std::string& handle) -> Unreal::UObject*
        {
            (void)world_snapshot();
            std::array<std::string_view, 4> parts{};
            size_t start{};
            for (size_t index{}; index < parts.size(); ++index)
            {
                const auto separator = handle.find(':', start);
                if (index + 1 == parts.size())
                {
                    if (separator != std::string::npos)
                    {
                        throw McpError{"object.invalid_handle", "Object handle has too many fields"};
                    }
                    parts[index] = std::string_view{handle}.substr(start);
                }
                else
                {
                    if (separator == std::string::npos)
                    {
                        throw McpError{"object.invalid_handle", "Object handle has too few fields"};
                    }
                    parts[index] = std::string_view{handle}.substr(start, separator - start);
                    start = separator + 1;
                }
            }
            if (parts[0] != m_session_id)
            {
                throw McpError{"object.session_mismatch", "Object handle belongs to a different UE4SS process session"};
            }

            const auto parse_number = [](std::string_view value, std::string_view field) -> uint64_t {
                uint64_t parsed{};
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (error != std::errc{} || end != value.data() + value.size())
                {
                    throw McpError{"object.invalid_handle", fmt::format("Object handle {} is invalid", field)};
                }
                return parsed;
            };
            const auto generation = parse_number(parts[1], "world generation");
            if (generation != m_world_generation)
            {
                throw McpError{"object.world_changed",
                               fmt::format("Object handle world generation {} is stale; current generation is {}", generation, m_world_generation)};
            }
            const auto index_value = parse_number(parts[2], "index");
            const auto serial_value = parse_number(parts[3], "serial");
            if (index_value > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
                serial_value > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
            {
                throw McpError{"object.invalid_handle", "Object handle index or serial is out of range"};
            }

            Unreal::FWeakObjectPtr weak{};
            weak.ObjectIndex = static_cast<int32_t>(index_value);
            weak.ObjectSerialNumber = static_cast<int32_t>(serial_value);
            auto* object = weak.Get();
            if (!object || object->IsUnreachable())
            {
                throw McpError{"object.stale_handle", "Object handle no longer resolves to a live UObject"};
            }
            return object;
        }

        static auto export_property_value(Unreal::UObject* object, Unreal::FProperty* property) -> std::string
        {
            Unreal::FString exported{};
            auto* value = property->ContainerPtrToValuePtr<void>(object);
            property->ExportTextItem(exported, value, value, object, 0);
            return to_string(StringType{*exported});
        }

        auto append_property_watch_event_locked(PropertyWatch& watch,
                                                std::string_view kind,
                                                const std::optional<std::string>& value,
                                                std::string_view error_code = {},
                                                std::string_view message = {}) -> void
        {
            Json event = json_object();
            event["sequence"] = watch.next_sequence++;
            event["timestampMs"] =
                    static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            event["kind"] = std::string{kind};
            if (value)
            {
                event["value"] = *value;
            }
            else
            {
                event["value"] = nullptr;
            }
            if (!error_code.empty())
            {
                event["errorCode"] = std::string{error_code};
                event["message"] = std::string{message};
            }
            watch.history.emplace_back(std::move(event));
            while (watch.history.size() > watch.history_limit)
            {
                watch.history.pop_front();
            }
        }

        auto start_property_watch(const std::string& handle, const std::string& property_name, size_t history_limit, bool emit_unchanged) -> Json
        {
            auto* object = resolve_object_handle(handle);
            auto* property = object->GetPropertyByNameInChain(ensure_str(property_name).c_str());
            if (!property)
            {
                throw McpError{"property.not_found", fmt::format("Property '{}' was not found on the resolved object", property_name)};
            }
            auto initial_value = export_property_value(object, property);

            PropertyWatch watch{};
            watch.id = fmt::format("property-watch-{}", m_next_property_watch_id.fetch_add(1));
            watch.handle = handle;
            watch.property_name = property_name;
            watch.history_limit = history_limit;
            watch.emit_unchanged = emit_unchanged;
            watch.last_value = initial_value;
            append_property_watch_event_locked(watch, "value", watch.last_value);

            Json result = json_object();
            result["watchId"] = watch.id;
            result["handle"] = watch.handle;
            result["propertyName"] = watch.property_name;
            result["historyLimit"] = static_cast<uint64_t>(watch.history_limit);
            result["emitUnchanged"] = watch.emit_unchanged;
            result["active"] = true;
            result["initialSequence"] = static_cast<uint64_t>(1);
            {
                std::lock_guard lock{m_property_watches_mutex};
                m_property_watch_order.emplace_back(watch.id);
                m_property_watches.emplace(watch.id, std::move(watch));
            }
            return result;
        }

        auto property_watches_snapshot() const -> Json
        {
            JsonArray watches{};
            std::lock_guard lock{m_property_watches_mutex};
            watches.reserve(m_property_watch_order.size());
            for (const auto& id : m_property_watch_order)
            {
                const auto iterator = m_property_watches.find(id);
                if (iterator == m_property_watches.end())
                {
                    continue;
                }
                const auto& watch = iterator->second;
                const auto oldest_sequence = watch.history.empty() ? watch.next_sequence : find_member(watch.history.front(), "sequence")->get<uint64_t>();
                Json item = json_object();
                item["watchId"] = watch.id;
                item["handle"] = watch.handle;
                item["propertyName"] = watch.property_name;
                item["historyLimit"] = static_cast<uint64_t>(watch.history_limit);
                item["emitUnchanged"] = watch.emit_unchanged;
                item["active"] = watch.active;
                item["expired"] = watch.expired;
                item["eventCount"] = static_cast<uint64_t>(watch.history.size());
                item["oldestCursor"] = oldest_sequence > 0 ? oldest_sequence - 1 : 0;
                item["newestSequence"] = watch.next_sequence - 1;
                watches.emplace_back(std::move(item));
            }
            Json result = json_object();
            result["watches"] = std::move(watches);
            result["count"] = static_cast<uint64_t>(m_property_watches.size());
            return result;
        }

        auto read_property_watch(const Json& arguments) const -> Json
        {
            const auto watch_id = required_string(arguments, "watchId");
            const auto cursor_value = optional_integer(arguments, "cursor", 0);
            const auto limit_value = optional_integer(arguments, "limit", 100);
            if (cursor_value < 0)
            {
                throw McpError{"transport.invalid_arguments", "cursor must not be negative"};
            }
            if (limit_value < 1 || limit_value > 1000)
            {
                throw McpError{"transport.invalid_arguments", "limit must be between 1 and 1000"};
            }
            const auto cursor = static_cast<uint64_t>(cursor_value);
            const auto limit = static_cast<size_t>(limit_value);

            JsonArray events{};
            uint64_t oldest_sequence{};
            uint64_t newest_sequence{};
            bool active{};
            bool expired{};
            {
                std::lock_guard lock{m_property_watches_mutex};
                const auto iterator = m_property_watches.find(watch_id);
                if (iterator == m_property_watches.end())
                {
                    throw McpError{"property_watch.not_found", fmt::format("Property watch '{}' does not exist in this server session", watch_id)};
                }
                const auto& watch = iterator->second;
                oldest_sequence = watch.history.empty() ? watch.next_sequence : find_member(watch.history.front(), "sequence")->get<uint64_t>();
                newest_sequence = watch.next_sequence - 1;
                active = watch.active;
                expired = watch.expired;
                events.reserve(std::min(limit, watch.history.size()));
                for (const auto& event : watch.history)
                {
                    const auto sequence = find_member(event, "sequence")->get<uint64_t>();
                    if (sequence > cursor && events.size() < limit)
                    {
                        events.emplace_back(event);
                    }
                }
            }
            const auto next_cursor = events.empty() ? cursor : find_member(events.back(), "sequence")->get<uint64_t>();
            Json result = json_object();
            result["watchId"] = watch_id;
            result["events"] = std::move(events);
            result["oldestCursor"] = oldest_sequence > 0 ? oldest_sequence - 1 : 0;
            result["nextCursor"] = next_cursor;
            result["newestSequence"] = newest_sequence;
            result["cursorExpired"] = cursor + 1 < oldest_sequence;
            result["truncated"] = next_cursor < newest_sequence;
            result["active"] = active;
            result["expired"] = expired;
            return result;
        }

        auto stop_property_watch(const std::string& watch_id) -> Json
        {
            bool found{};
            bool was_active{};
            {
                std::lock_guard lock{m_property_watches_mutex};
                const auto iterator = m_property_watches.find(watch_id);
                if (iterator != m_property_watches.end())
                {
                    found = true;
                    was_active = iterator->second.active;
                    iterator->second.active = false;
                }
            }
            Json result = json_object();
            result["watchId"] = watch_id;
            result["found"] = found;
            result["stopped"] = true;
            result["wasActive"] = was_active;
            return result;
        }

        auto sample_property_watches() -> void
        {
            struct PendingSample
            {
                std::string id;
                std::string handle;
                std::string property_name;
            };
            constexpr size_t maximum_samples_per_tick = 16;
            constexpr size_t maximum_watch_slots_per_tick = 64;
            std::vector<PendingSample> samples{};
            {
                std::lock_guard lock{m_property_watches_mutex};
                const auto watch_count = m_property_watch_order.size();
                samples.reserve(std::min(watch_count, maximum_samples_per_tick));
                const auto slots_to_visit = std::min(watch_count, maximum_watch_slots_per_tick);
                for (size_t visited{}; visited < slots_to_visit && samples.size() < maximum_samples_per_tick; ++visited)
                {
                    if (m_property_watch_rotation >= watch_count)
                    {
                        m_property_watch_rotation = 0;
                    }
                    const auto& id = m_property_watch_order[m_property_watch_rotation++];
                    const auto iterator = m_property_watches.find(id);
                    if (iterator != m_property_watches.end() && iterator->second.active)
                    {
                        samples.emplace_back(PendingSample{id, iterator->second.handle, iterator->second.property_name});
                    }
                }
            }

            for (const auto& sample : samples)
            {
                std::optional<std::string> value{};
                std::string error_code{};
                std::string error_message{};
                try
                {
                    auto* object = resolve_object_handle(sample.handle);
                    auto* property = object->GetPropertyByNameInChain(ensure_str(sample.property_name).c_str());
                    if (!property)
                    {
                        throw McpError{"property.not_found", fmt::format("Property '{}' was not found on the resolved object", sample.property_name)};
                    }
                    value = export_property_value(object, property);
                }
                catch (const McpError& error)
                {
                    error_code = error.code();
                    error_message = error.what();
                }
                catch (const std::exception& error)
                {
                    error_code = "property_watch.sample_failed";
                    error_message = error.what();
                }

                std::lock_guard lock{m_property_watches_mutex};
                const auto iterator = m_property_watches.find(sample.id);
                if (iterator == m_property_watches.end() || !iterator->second.active)
                {
                    continue;
                }
                auto& watch = iterator->second;
                if (!error_code.empty())
                {
                    append_property_watch_event_locked(watch, "expired", std::nullopt, error_code, error_message);
                    watch.active = false;
                    watch.expired = true;
                    continue;
                }
                if (watch.emit_unchanged || !watch.last_value || *watch.last_value != *value)
                {
                    append_property_watch_event_locked(watch, "value", value);
                }
                watch.last_value = std::move(value);
            }
        }

        auto inspect_object(const std::string& handle, size_t limit) -> Json
        {
            auto* object = resolve_object_handle(handle);
            auto* object_class = object->GetClassPrivate();
            if (!object_class)
            {
                throw McpError{"object.invalid_class", "Resolved UObject has no class"};
            }

            JsonArray properties{};
            JsonArray functions{};
            bool truncated{};
            const auto append_fields = [&](Unreal::UStruct* owner) {
                for (auto* property : Unreal::TFieldRange<Unreal::FProperty>(owner, Unreal::EFieldIterationFlags::IncludeDeprecated))
                {
                    if (properties.size() + functions.size() >= limit)
                    {
                        truncated = true;
                        return false;
                    }
                    Json item = json_object();
                    item["name"] = to_string(property->GetName());
                    item["type"] = to_string(property->GetClass().GetFName().ToString());
                    item["owner"] = to_string(owner->GetName());
                    item["offset"] = property->GetOffset_Internal();
                    item["size"] = property->GetSize();
                    item["arrayDim"] = property->GetArrayDim();
                    item["flags"] = static_cast<uint64_t>(property->GetPropertyFlags());
                    properties.emplace_back(std::move(item));
                }
                for (auto* function : Unreal::TFieldRange<Unreal::UFunction>(owner, Unreal::EFieldIterationFlags::IncludeDeprecated))
                {
                    if (properties.size() + functions.size() >= limit)
                    {
                        truncated = true;
                        return false;
                    }
                    Json item = json_object();
                    item["name"] = to_string(function->GetName());
                    item["fullName"] = to_string(function->GetFullName());
                    item["owner"] = to_string(owner->GetName());
                    item["flags"] = static_cast<uint64_t>(function->GetFunctionFlags());
                    item["parameterBytes"] = function->GetParmsSize();
                    item["parameterCount"] = function->GetNumParms();
                    functions.emplace_back(std::move(item));
                }
                return true;
            };
            if (append_fields(object_class))
            {
                for (auto* super : Unreal::TSuperStructRange(object_class))
                {
                    if (!append_fields(super))
                    {
                        break;
                    }
                }
            }

            Json result = json_object();
            result["handle"] = handle;
            result["name"] = to_string(object->GetName());
            result["fullName"] = to_string(object->GetFullName());
            result["class"] = to_string(object_class->GetName());
            result["properties"] = std::move(properties);
            result["functions"] = std::move(functions);
            result["truncated"] = truncated;
            return result;
        }

        auto read_property(const std::string& handle, const std::string& property_name) -> Json
        {
            auto* object = resolve_object_handle(handle);
            auto* property = object->GetPropertyByNameInChain(ensure_str(property_name).c_str());
            if (!property)
            {
                throw McpError{"reflection.property_not_found", fmt::format("Property '{}' was not found on the resolved object", property_name)};
            }
            Json result = json_object();
            result["handle"] = handle;
            result["propertyName"] = property_name;
            result["type"] = to_string(property->GetClass().GetFName().ToString());
            result["value"] = export_property_value(object, property);
            return result;
        }

        auto write_property(const std::string& handle, const std::string& property_name, const std::string& value) -> Json
        {
            auto* object = resolve_object_handle(handle);
            auto* property = object->GetPropertyByNameInChain(ensure_str(property_name).c_str());
            if (!property)
            {
                throw McpError{"reflection.property_not_found", fmt::format("Property '{}' was not found on the resolved object", property_name)};
            }
            Unreal::FOutputDevice output_device{};
            const auto wide_value = ensure_str(value);
            if (!property->ImportText(wide_value.c_str(), property->ContainerPtrToValuePtr<void>(object), 0, object, &output_device))
            {
                throw McpError{"reflection.invalid_value", fmt::format("Unreal rejected '{}' as a value for property '{}'", value, property_name)};
            }

            Json result = json_object();
            result["handle"] = handle;
            result["propertyName"] = property_name;
            result["requestedValue"] = value;
            result["value"] = export_property_value(object, property);
            return result;
        }

        auto call_function(const std::string& handle, const std::string& function_name, const Json& values) -> Json
        {
            auto* object = resolve_object_handle(handle);
            auto* function = object->GetFunctionByNameInChain(ensure_str(function_name).c_str());
            if (!function)
            {
                throw McpError{"reflection.function_not_found", fmt::format("Function '{}' was not found on the resolved object", function_name)};
            }

            std::vector<uint8_t> parameters(static_cast<size_t>(function->GetStructureSize()));
            function->InitializeStruct(parameters.data());
            const auto destroy_parameters = [&] {
                function->DestroyStruct(parameters.data());
            };
            try
            {
                std::unordered_set<std::string> consumed{};
                for (auto* property : Unreal::TFieldRange<Unreal::FProperty>(function, Unreal::EFieldIterationFlags::IncludeDeprecated))
                {
                    if (!property->HasAnyPropertyFlags(Unreal::CPF_Parm) || property->HasAnyPropertyFlags(Unreal::CPF_ReturnParm))
                    {
                        continue;
                    }
                    const auto name = to_string(property->GetName());
                    const auto argument = values.get<JsonObject>().find(name);
                    if (argument == values.get<JsonObject>().end())
                    {
                        continue;
                    }
                    if (!argument->second.is_string())
                    {
                        throw McpError{"transport.invalid_arguments", fmt::format("Function argument '{}' must be an Unreal text string", name)};
                    }
                    const auto text = ensure_str(argument->second.get<std::string>());
                    Unreal::FOutputDevice output_device{};
                    if (!property->ImportText(text.c_str(), property->ContainerPtrToValuePtr<void>(parameters.data()), 0, object, &output_device))
                    {
                        throw McpError{"reflection.invalid_argument", fmt::format("Unreal rejected the value for function argument '{}'", name)};
                    }
                    consumed.emplace(name);
                }
                for (const auto& [name, _] : values.get<JsonObject>())
                {
                    if (!consumed.contains(name))
                    {
                        throw McpError{"reflection.unknown_argument", fmt::format("Function '{}' has no input parameter named '{}'", function_name, name)};
                    }
                }

                object->ProcessEvent(function, parameters.data());
                Json outputs = json_object();
                for (auto* property : Unreal::TFieldRange<Unreal::FProperty>(function, Unreal::EFieldIterationFlags::IncludeDeprecated))
                {
                    if (!property->HasAnyPropertyFlags(Unreal::CPF_OutParm | Unreal::CPF_ReturnParm))
                    {
                        continue;
                    }
                    Unreal::FString exported{};
                    auto* property_value = property->ContainerPtrToValuePtr<void>(parameters.data());
                    property->ExportTextItem(exported, property_value, property_value, object, 0);
                    outputs[to_string(property->GetName())] = to_string(StringType{*exported});
                }

                Json result = json_object();
                result["handle"] = handle;
                result["functionName"] = function_name;
                result["functionFullName"] = to_string(function->GetFullName());
                result["outputs"] = std::move(outputs);
                destroy_parameters();
                return result;
            }
            catch (...)
            {
                destroy_parameters();
                throw;
            }
        }

        auto query_objects(const Json& arguments) -> Json
        {
            if (!Unreal::UObjectArray::GetGUObjectArrayAddress())
            {
                throw McpError{"lifecycle.runtime_not_ready", "GUObjectArray is not initialized"};
            }
            if (!arguments.is_object())
            {
                throw McpError{"transport.invalid_arguments", "ue4ss_objects_query arguments must be an object"};
            }

            const auto optional_boolean = [&](std::string_view field, bool fallback) {
                const auto* value = find_member(arguments, field);
                if (!value)
                {
                    return fallback;
                }
                if (!value->is_boolean())
                {
                    throw McpError{"transport.invalid_arguments", fmt::format("Argument '{}' must be a boolean", field)};
                }
                return value->get<bool>();
            };
            const auto string_array = [&](std::string_view field) {
                std::vector<std::string> values{};
                const auto* value = find_member(arguments, field);
                if (!value)
                {
                    return values;
                }
                if (!value->is_array())
                {
                    throw McpError{"transport.invalid_arguments", fmt::format("Argument '{}' must be an array of strings", field)};
                }
                const auto& array = value->get<JsonArray>();
                if (array.size() > 128)
                {
                    throw McpError{"transport.invalid_arguments", fmt::format("Argument '{}' may contain at most 128 strings", field)};
                }
                values.reserve(array.size());
                for (const auto& item : array)
                {
                    if (!item.is_string() || item.get<std::string>().empty())
                    {
                        throw McpError{"transport.invalid_arguments", fmt::format("Argument '{}' must contain only non-empty strings", field)};
                    }
                    values.emplace_back(item.get<std::string>());
                }
                return values;
            };

            struct TextFilter
            {
                std::string value{};
                std::optional<std::regex> expression{};
            };
            const bool use_regex = optional_boolean("regex", false);
            const bool case_sensitive = optional_boolean("caseSensitive", false);
            const bool include_derived = optional_boolean("includeDerived", false);
            const auto make_text_filter = [&](std::string_view field) -> std::optional<TextFilter> {
                auto value = optional_string(arguments, field);
                if (!value)
                {
                    return std::nullopt;
                }
                if (value->empty())
                {
                    throw McpError{"transport.invalid_arguments", fmt::format("Argument '{}' must not be empty", field)};
                }
                TextFilter filter{std::move(*value), std::nullopt};
                if (use_regex)
                {
                    auto flags = std::regex_constants::ECMAScript;
                    if (!case_sensitive)
                    {
                        flags |= std::regex_constants::icase;
                    }
                    try
                    {
                        filter.expression.emplace(filter.value, flags);
                    }
                    catch (const std::regex_error& error)
                    {
                        throw McpError{"transport.invalid_arguments",
                                       fmt::format("Argument '{}' is not a valid ECMAScript regular expression: {}", field, error.what())};
                    }
                }
                return filter;
            };
            const auto name_filter = make_text_filter("name");
            const auto full_name_filter = make_text_filter("fullName");
            const auto path_filter = make_text_filter("path");
            const auto class_filter = make_text_filter("className");
            const auto outer_filter = make_text_filter("outer");
            const auto text_matches = [&](std::string_view candidate, const TextFilter& filter) {
                if (filter.expression)
                {
                    return std::regex_search(candidate.begin(), candidate.end(), *filter.expression);
                }
                if (case_sensitive)
                {
                    return candidate.find(filter.value) != std::string_view::npos;
                }
                return std::search(candidate.begin(), candidate.end(), filter.value.begin(), filter.value.end(), [](char left, char right) {
                           return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
                       }) != candidate.end();
            };
            const auto literal_equals = [&](std::string_view left, std::string_view right) {
                return left.size() == right.size() && (case_sensitive || std::equal(left.begin(), left.end(), right.begin(), [](char lhs, char rhs) {
                                                           return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
                                                       }));
            };

            const auto instance_mode = optional_string(arguments, "instanceMode").value_or("any");
            if (instance_mode != "any" && instance_mode != "instances" && instance_mode != "nonInstances" && instance_mode != "classDefaultObjects")
            {
                throw McpError{"transport.invalid_arguments", "Argument 'instanceMode' must be any, instances, nonInstances or classDefaultObjects"};
            }
            const auto has_properties = string_array("hasProperties");
            const auto property_types = string_array("propertyTypes");
            const auto limit_value = optional_integer(arguments, "limit", 100);
            if (limit_value < 1 || limit_value > 500)
            {
                throw McpError{"transport.invalid_arguments", "Argument 'limit' must be between 1 and 500"};
            }
            const auto limit = static_cast<size_t>(limit_value);
            const auto cursor_value = optional_integer(arguments, "cursor", 0);
            if (cursor_value < 0)
            {
                throw McpError{"transport.invalid_arguments", "Argument 'cursor' must be non-negative"};
            }
            const auto max_scan_value = optional_integer(arguments, "maxScanObjects", 50000);
            if (max_scan_value < 1 || max_scan_value > 200000)
            {
                throw McpError{"transport.invalid_arguments", "Argument 'maxScanObjects' must be between 1 and 200000"};
            }
            const auto cursor = static_cast<uint64_t>(cursor_value);
            const auto max_scan = static_cast<uint64_t>(max_scan_value);

            std::optional<uintptr_t> address{};
            std::optional<std::string> requested_address = optional_string(arguments, "address");
            if (requested_address)
            {
                const auto normalized = trim(*requested_address);
                const bool hexadecimal = normalized.size() > 2 && normalized[0] == '0' && (normalized[1] == 'x' || normalized[1] == 'X');
                const auto digits = hexadecimal ? normalized.substr(2) : normalized;
                uint64_t parsed{};
                const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), parsed, hexadecimal ? 16 : 10);
                if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size() || parsed == 0 ||
                    parsed > static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max()))
                {
                    throw McpError{"transport.invalid_arguments", "Argument 'address' must be a non-zero hexadecimal (0x...) or decimal UObject address"};
                }
                address = static_cast<uintptr_t>(parsed);
            }

            const bool has_effective_filter = name_filter || full_name_filter || path_filter || class_filter || outer_filter || address ||
                                              !has_properties.empty() || !property_types.empty() || instance_mode != "any";
            if (!has_effective_filter)
            {
                throw McpError{"transport.invalid_arguments", "ue4ss_objects_query requires at least one effective filtering condition"};
            }

            Json query = json_object();
            const auto record_text_filter = [&](std::string_view field, const std::optional<TextFilter>& filter) {
                if (filter)
                {
                    query[std::string{field}] = filter->value;
                }
            };
            record_text_filter("name", name_filter);
            record_text_filter("fullName", full_name_filter);
            record_text_filter("path", path_filter);
            record_text_filter("className", class_filter);
            record_text_filter("outer", outer_filter);
            if (address)
            {
                query["address"] = fmt::format("0x{:X}", *address);
            }
            query["regex"] = use_regex;
            query["caseSensitive"] = case_sensitive;
            query["includeDerived"] = include_derived;
            query["instanceMode"] = instance_mode;
            query["limit"] = static_cast<uint64_t>(limit);
            query["cursor"] = cursor;
            query["maxScanObjects"] = max_scan;
            Json has_properties_json = JsonArray{};
            Json property_types_json = JsonArray{};
            for (const auto& property : has_properties)
            {
                has_properties_json.get<JsonArray>().emplace_back(property);
            }
            for (const auto& type : property_types)
            {
                property_types_json.get<JsonArray>().emplace_back(type);
            }
            query["hasProperties"] = std::move(has_properties_json);
            query["propertyTypes"] = std::move(property_types_json);

            (void)world_snapshot();
            std::vector<StringType> reflected_property_names{};
            reflected_property_names.reserve(has_properties.size());
            for (const auto& property : has_properties)
            {
                reflected_property_names.emplace_back(ensure_str(property));
            }

            JsonArray objects{};
            objects.reserve(limit);
            const auto object_count = static_cast<uint64_t>(Unreal::UObjectArray::GetNumElements());
            const auto scan_end = std::min(object_count, cursor + max_scan);
            uint64_t scanned{};
            uint64_t matched{};
            uint64_t next_cursor = cursor;
            bool truncated{};
            for (uint64_t current_position = cursor; current_position < scan_end; ++current_position)
            {
                ++scanned;
                next_cursor = current_position + 1;
                const auto index = static_cast<int32_t>(current_position);
                auto* object_item = Unreal::UObjectArray::IndexToObject(index);
                if (!object_item || !object_item->IsValid(false))
                {
                    continue;
                }
                auto* object = object_item->GetUObject();

                if (!object || object->IsUnreachable())
                {
                    continue;
                }
                if (object_item->GetUObject() != object)
                {
                    continue;
                }
                auto* object_class = object->GetClassPrivate();
                if (!object_class)
                {
                    continue;
                }

                const bool is_class_default = object->HasAnyFlags(Unreal::RF_ClassDefaultObject);
                const bool is_instance = !object->HasAnyFlags(static_cast<Unreal::EObjectFlags>(Unreal::RF_ClassDefaultObject | Unreal::RF_ArchetypeObject)) &&
                                         !object->IsA<Unreal::UStruct>() && !object->IsA<Unreal::UField>() && !object->IsA<Unreal::UPackage>();
                if ((instance_mode == "instances" && !is_instance) || (instance_mode == "nonInstances" && is_instance) ||
                    (instance_mode == "classDefaultObjects" && !is_class_default))
                {
                    continue;
                }
                if (address && reinterpret_cast<uintptr_t>(object) != *address)
                {
                    continue;
                }
                if (name_filter && !text_matches(to_string(object->GetName()), *name_filter))
                {
                    continue;
                }
                if (full_name_filter && !text_matches(to_string(object->GetFullName()), *full_name_filter))
                {
                    continue;
                }
                if (path_filter && !text_matches(to_string(object->GetPathName()), *path_filter))
                {
                    continue;
                }
                if (outer_filter)
                {
                    auto* outer = object->GetOuterPrivate();
                    if (!outer || !text_matches(to_string(outer->GetPathName()), *outer_filter))
                    {
                        continue;
                    }
                }
                if (class_filter)
                {
                    bool class_matches = text_matches(to_string(object_class->GetName()), *class_filter);
                    if (!class_matches && include_derived)
                    {
                        for (auto* super : Unreal::TSuperStructRange(object_class))
                        {
                            if (super && text_matches(to_string(super->GetName()), *class_filter))
                            {
                                class_matches = true;
                                break;
                            }
                        }
                    }
                    if (!class_matches)
                    {
                        continue;
                    }
                }
                bool has_all_properties = true;
                for (const auto& property_name : reflected_property_names)
                {
                    if (!object->GetPropertyByNameInChain(property_name.c_str()))
                    {
                        has_all_properties = false;
                        break;
                    }
                }
                if (!has_all_properties)
                {
                    continue;
                }
                if (!property_types.empty())
                {
                    std::vector<bool> found_types(property_types.size(), false);
                    auto* property_owner = object->IsA<Unreal::UStruct>() ? static_cast<Unreal::UStruct*>(object) : static_cast<Unreal::UStruct*>(object_class);
                    for (auto* property :
                         Unreal::TFieldRange<Unreal::FProperty>(property_owner,
                                                                Unreal::EFieldIterationFlags::IncludeSuper | Unreal::EFieldIterationFlags::IncludeDeprecated))
                    {
                        const auto property_type = to_string(property->GetClass().GetFName().ToString());
                        for (size_t type_index = 0; type_index < property_types.size(); ++type_index)
                        {
                            if (!found_types[type_index] && literal_equals(property_type, property_types[type_index]))
                            {
                                found_types[type_index] = true;
                            }
                        }
                    }
                    if (std::ranges::find(found_types, false) != found_types.end())
                    {
                        continue;
                    }
                }

                ++matched;
                if (objects.size() >= limit)
                {
                    next_cursor = current_position;
                    truncated = true;
                    break;
                }

                const auto serial_number = Unreal::UObjectArray::AllocateSerialNumber(index);
                Json item = json_object();
                item["handle"] = fmt::format("{}:{}:{}:{}", m_session_id, m_world_generation, index, serial_number);
                item["name"] = to_string(object->GetName());
                item["fullName"] = to_string(object->GetFullName());
                item["path"] = to_string(object->GetPathName());
                item["className"] = to_string(object_class->GetName());
                if (auto* outer = object->GetOuterPrivate())
                {
                    item["outer"] = to_string(outer->GetPathName());
                }
                else
                {
                    item["outer"] = nullptr;
                }
                item["address"] = fmt::format("0x{:X}", reinterpret_cast<uintptr_t>(object));
                item["worldGeneration"] = m_world_generation;
                item["objectIndex"] = index;
                item["serialNumber"] = serial_number;
                item["lifecycle"] = "valid";
                objects.emplace_back(std::move(item));
            }
            if (!truncated && next_cursor < object_count)
            {
                truncated = true;
            }

            Json result = json_object();
            result["query"] = std::move(query);
            result["objects"] = std::move(objects);
            result["returned"] = static_cast<uint64_t>(result["objects"].get<JsonArray>().size());
            result["matched"] = matched;
            result["scanned"] = scanned;
            result["truncated"] = truncated;
            if (truncated)
            {
                result["nextCursor"] = next_cursor;
            }
            else
            {
                result["nextCursor"] = nullptr;
            }
            result["matchCountComplete"] = !truncated;
            return result;
        }
        auto load_asset(const std::string& object_path) -> Json
        {
            if (!Unreal::UObjectArray::GetGUObjectArrayAddress())
            {
                throw McpError{"lifecycle.runtime_not_ready", "GUObjectArray is not initialized"};
            }
            (void)world_snapshot();

            auto* asset_registry = static_cast<Unreal::UAssetRegistry*>(Unreal::UAssetRegistryHelpers::GetAssetRegistry().ObjectPointer);
            if (!asset_registry)
            {
                throw McpError{"asset.registry_unavailable", "Unreal AssetRegistry is unavailable"};
            }

            const Unreal::FName asset_name{ensure_str(object_path), Unreal::FNAME_Add};
            Unreal::FAssetData asset_data = asset_registry->GetAssetByObjectPath(asset_name);
            const bool was_found =
                    (Unreal::Version::IsAtMost(5, 0) && asset_data.ObjectPath().GetComparisonIndex()) || asset_data.PackageName().GetComparisonIndex();
            if (!was_found)
            {
                throw McpError{"asset.not_found", fmt::format("AssetRegistry has no asset at exact object path '{}'", object_path)};
            }

            auto* loaded_asset = Unreal::UAssetRegistryHelpers::GetAsset(asset_data);
            if (!loaded_asset || loaded_asset->IsUnreachable())
            {
                throw McpError{"asset.load_failed", fmt::format("Asset '{}' was found but could not be loaded as a UObject", object_path)};
            }

            const auto index = loaded_asset->GetInternalIndex();
            auto* object_item = Unreal::UObjectArray::IndexToObject(index);
            if (!object_item || !object_item->IsValid(false) || object_item->GetUObject() != loaded_asset)
            {
                throw McpError{"asset.load_failed", fmt::format("Asset '{}' loaded without a valid GUObjectArray identity", object_path)};
            }
            auto* object_class = loaded_asset->GetClassPrivate();
            if (!object_class)
            {
                throw McpError{"asset.load_failed", fmt::format("Asset '{}' loaded without a reflected class", object_path)};
            }

            const auto serial_number = Unreal::UObjectArray::AllocateSerialNumber(index);
            const auto handle = fmt::format("{}:{}:{}:{}", m_session_id, m_world_generation, index, serial_number);
            Json identity = json_object();
            identity["handle"] = handle;
            identity["name"] = to_string(loaded_asset->GetName());
            identity["fullName"] = to_string(loaded_asset->GetFullName());
            identity["path"] = to_string(loaded_asset->GetPathName());
            identity["class"] = to_string(object_class->GetName());
            identity["worldGeneration"] = m_world_generation;
            identity["objectIndex"] = index;
            identity["serialNumber"] = serial_number;
            identity["lifecycle"] = "valid";

            Json result = json_object();
            result["objectPath"] = object_path;
            result["wasFound"] = true;
            result["didLoad"] = true;
            result["handle"] = handle;
            result["identity"] = std::move(identity);
            return result;
        }

        auto execute_lua(const std::string& mod_name, const std::string& code) -> Json
        {
            LuaMod* selected_mod{};
            for (const auto& mod : m_program.m_mods)
            {
                auto* lua_mod = dynamic_cast<LuaMod*>(mod.get());
                if (lua_mod && to_string(lua_mod->get_name()) == mod_name)
                {
                    selected_mod = lua_mod;
                    break;
                }
            }
            if (!selected_mod)
            {
                throw McpError{"mod.not_found", fmt::format("Lua mod '{}' was not found", mod_name)};
            }
            if (!selected_mod->is_started() || !selected_mod->m_hook_lua)
            {
                throw McpError{"lifecycle.runtime_not_ready", fmt::format("Lua mod '{}' has no started game-thread Lua state", mod_name)};
            }

            return run_on_game_thread([selected_mod, mod_name, code] {
                auto* lua = selected_mod->m_hook_lua->get_lua_state();
                const auto base = lua_gettop(lua);
                if (luaL_loadbuffer(lua, code.data(), code.size(), "=mcp") != LUA_OK)
                {
                    const std::string message = lua_tostring(lua, -1) ? lua_tostring(lua, -1) : "Lua compilation failed";
                    if (GUI::LuaDebugger::has_instance())
                    {
                        GUI::LuaDebugger::get().record_error(lua, message, message);
                    }
                    lua_settop(lua, base);
                    throw McpError{"lua.compile_failed", message};
                }
                if (lua_pcall(lua, 0, LUA_MULTRET, 0) != LUA_OK)
                {
                    const char* message = lua_tostring(lua, -1);
                    luaL_traceback(lua, lua, message ? message : "Lua execution failed", 1);
                    const std::string traceback = lua_tostring(lua, -1) ? lua_tostring(lua, -1) : "Lua execution failed";
                    if (GUI::LuaDebugger::has_instance())
                    {
                        GUI::LuaDebugger::get().record_error(lua, message ? message : "Lua execution failed", traceback);
                    }
                    lua_settop(lua, base);
                    throw McpError{"lua.execution_failed", traceback};
                }

                JsonArray values{};
                const auto top = lua_gettop(lua);
                for (int index = base + 1; index <= top; ++index)
                {
                    switch (lua_type(lua, index))
                    {
                    case LUA_TNIL:
                        values.emplace_back(nullptr);
                        break;
                    case LUA_TBOOLEAN:
                        values.emplace_back(lua_toboolean(lua, index) != 0);
                        break;
                    case LUA_TNUMBER:
                        if (lua_isinteger(lua, index))
                        {
                            values.emplace_back(static_cast<int64_t>(lua_tointeger(lua, index)));
                        }
                        else
                        {
                            values.emplace_back(static_cast<double>(lua_tonumber(lua, index)));
                        }
                        break;
                    case LUA_TSTRING:
                        values.emplace_back(std::string{lua_tostring(lua, index)});
                        break;
                    default: {
                        Json value = json_object();
                        value["luaType"] = lua_typename(lua, lua_type(lua, index));
                        values.emplace_back(std::move(value));
                        break;
                    }
                    }
                }
                lua_settop(lua, base);

                Json result = json_object();
                result["modName"] = mod_name;
                result["codeExecuted"] = true;
                result["returnValues"] = std::move(values);
                return result;
            });
        }

        auto reload_lua_mod(const std::string& mod_name) -> Json
        {
            LuaMod* selected_mod{};
            for (const auto& mod : m_program.m_mods)
            {
                auto* lua_mod = dynamic_cast<LuaMod*>(mod.get());
                if (lua_mod && to_string(lua_mod->get_name()) == mod_name)
                {
                    selected_mod = lua_mod;
                    break;
                }
            }
            if (!selected_mod)
            {
                throw McpError{"mod.not_found", fmt::format("Lua mod '{}' was not found", mod_name)};
            }

            const auto old_id = selected_mod->get_id();
            m_program.queue_reinstall_mod(selected_mod);
            LuaMod* reloaded_mod{};
            for (const auto& mod : m_program.m_mods)
            {
                auto* lua_mod = dynamic_cast<LuaMod*>(mod.get());
                if (lua_mod && to_string(lua_mod->get_name()) == mod_name)
                {

                    reloaded_mod = lua_mod;
                    break;
                }
            }
            if (!reloaded_mod || !reloaded_mod->is_started())
            {
                if (reloaded_mod)
                {
                    m_program.queue_uninstall_mod(reloaded_mod);
                }
                throw McpError{"mod.reload_failed", fmt::format("Lua mod '{}' did not start successfully during reload", mod_name)};
            }

            Json result = json_object();
            result["modName"] = mod_name;
            result["previousId"] = static_cast<uint64_t>(old_id);
            result["currentId"] = static_cast<uint64_t>(reloaded_mod->get_id());
            result["installed"] = reloaded_mod->is_installed();
            result["started"] = reloaded_mod->is_started();
            result["restartRequired"] = false;
            return result;
        }
        auto control_lua_mod(const std::string& mod_name, const std::string& action) -> Json
        {
            LuaMod* selected_mod{};
            bool cpp_mod_found{};
            for (const auto& mod : m_program.m_mods)
            {
                if (auto* lua_mod = dynamic_cast<LuaMod*>(mod.get()); lua_mod && to_string(lua_mod->get_name()) == mod_name)
                {
                    selected_mod = lua_mod;
                    break;
                }
                if (auto* cpp_mod = dynamic_cast<CppMod*>(mod.get()); cpp_mod && to_string(cpp_mod->get_name()) == mod_name)
                {
                    cpp_mod_found = true;
                }
            }
            if (!selected_mod && cpp_mod_found && !m_stopped_lua_mod_paths.contains(mod_name))
            {
                throw McpError{"mod.cpp_restart_required",
                               fmt::format("C++ mod '{}' cannot be controlled at runtime; restart the game process to change it safely", mod_name)};
            }

            const auto state_from = [&](LuaMod* mod, const std::filesystem::path& path) {
                Json state = json_object();
                state["name"] = mod_name;
                state["action"] = action;
                state["path"] = path.generic_string();
                state["type"] = "lua";
                if (mod)
                {
                    state["id"] = static_cast<uint64_t>(mod->get_id());
                }
                else
                {
                    state["id"] = nullptr;
                }
                state["installed"] = mod && mod->is_installed();
                state["started"] = mod && mod->is_started();
                state["persisted"] = false;
                state["restartRequired"] = false;
                return state;
            };

            if (action == "restart")
            {
                if (!selected_mod)
                {
                    throw McpError{"mod.not_found", fmt::format("Installed Lua mod '{}' was not found for restart", mod_name)};
                }
                const auto path = selected_mod->get_path();
                m_stopped_lua_mod_paths[mod_name] = path;
                auto state = reload_lua_mod(mod_name);
                state["name"] = mod_name;
                state["action"] = action;
                state["path"] = path.generic_string();
                state["type"] = "lua";
                state["persisted"] = false;
                state["restartRequired"] = false;
                return state;
            }

            if (action == "stop")
            {
                if (!selected_mod)
                {
                    const auto known = m_stopped_lua_mod_paths.find(mod_name);
                    if (known == m_stopped_lua_mod_paths.end())
                    {
                        throw McpError{"mod.not_found", fmt::format("Lua mod '{}' was not found", mod_name)};
                    }
                    return state_from(nullptr, known->second);
                }
                const auto path = selected_mod->get_path();
                m_stopped_lua_mod_paths[mod_name] = path;
                m_program.queue_uninstall_mod(selected_mod);
                LuaMod* remaining_mod{};
                for (const auto& mod : m_program.m_mods)
                {
                    auto* lua_mod = dynamic_cast<LuaMod*>(mod.get());
                    if (lua_mod && to_string(lua_mod->get_name()) == mod_name)
                    {
                        remaining_mod = lua_mod;
                        break;
                    }
                }
                return state_from(remaining_mod, path);
            }

            std::filesystem::path path;
            if (selected_mod)
            {
                path = selected_mod->get_path();
                m_stopped_lua_mod_paths[mod_name] = path;
                if (selected_mod->is_started())
                {
                    return state_from(selected_mod, path);
                }
            }
            else
            {
                const auto known = m_stopped_lua_mod_paths.find(mod_name);
                if (known == m_stopped_lua_mod_paths.end())
                {
                    throw McpError{"mod.not_found", fmt::format("Lua mod '{}' has no known runtime path", mod_name)};
                }
                path = known->second;
            }

            m_program.queue_start_lua_mod_by_path(path);
            LuaMod* started_mod{};
            for (const auto& mod : m_program.m_mods)
            {
                auto* lua_mod = dynamic_cast<LuaMod*>(mod.get());
                if (lua_mod && to_string(lua_mod->get_name()) == mod_name)
                {
                    started_mod = lua_mod;
                    break;
                }
            }
            if (!started_mod || !started_mod->is_started())
            {
                if (started_mod)
                {
                    m_program.queue_uninstall_mod(started_mod);
                }
                throw McpError{"mod.start_failed", fmt::format("Lua mod '{}' did not start successfully", mod_name)};
            }
            return state_from(started_mod, path);
        }

        auto attach_lua_debugger(const std::string& mod_name) -> Json
        {
            LuaMod* selected_mod{};
            for (const auto& mod : m_program.m_mods)
            {
                auto* lua_mod = dynamic_cast<LuaMod*>(mod.get());
                if (lua_mod && to_string(lua_mod->get_name()) == mod_name)
                {
                    selected_mod = lua_mod;
                    break;
                }
            }
            if (!selected_mod || !selected_mod->is_started())
            {
                throw McpError{"mod.not_ready", fmt::format("Started Lua mod '{}' was not found", mod_name)};
            }

            return run_on_game_thread([selected_mod, mod_name] {
                auto* hook_lua = selected_mod->ensure_hook_lua();
                auto* state = hook_lua->get_lua_state();
                auto& debugger = GUI::LuaDebugger::get();
                debugger.register_lua_state(state, mod_name, "hook");
                debugger.install_debug_hook(state);
                Json result = json_object();
                result["modName"] = mod_name;
                result["stateType"] = "hook";
                result["attached"] = debugger.has_debug_hook(state);
                return result;
            });
        }

        auto detach_lua_debugger(const std::string& mod_name) -> Json
        {
            LuaMod* selected_mod{};
            for (const auto& mod : m_program.m_mods)
            {
                auto* lua_mod = dynamic_cast<LuaMod*>(mod.get());
                if (lua_mod && to_string(lua_mod->get_name()) == mod_name)
                {
                    selected_mod = lua_mod;
                    break;
                }
            }
            if (!selected_mod)
            {
                throw McpError{"mod.not_found", fmt::format("Lua mod '{}' was not found", mod_name)};
            }

            return run_on_game_thread([selected_mod, mod_name] {
                auto& debugger = GUI::LuaDebugger::get();
                if (selected_mod->m_hook_lua)
                {
                    debugger.unregister_lua_state(selected_mod->m_hook_lua->get_lua_state());
                }
                Json result = json_object();
                result["modName"] = mod_name;
                result["stateType"] = "hook";
                result["attached"] = false;
                return result;
            });
        }

        auto lua_breakpoints_snapshot() const -> Json
        {
            JsonArray breakpoints{};
            for (const auto& breakpoint : GUI::LuaDebugger::get().get_breakpoints())
            {
                Json item = json_object();
                item["source"] = breakpoint.source_file;
                item["line"] = breakpoint.line;
                item["condition"] = breakpoint.condition;
                item["enabled"] = breakpoint.enabled;
                item["hitCount"] = breakpoint.hit_count;
                breakpoints.emplace_back(std::move(item));
            }
            Json result = json_object();
            result["breakpoints"] = std::move(breakpoints);
            return result;
        }

        static auto lua_stack_slot_json(const GUI::LuaStackSlot& slot) -> Json
        {
            Json item = json_object();
            item["index"] = slot.index;
            item["type"] = slot.type_name;
            item["value"] = slot.value_preview;
            item["isTable"] = slot.is_table;
            item["isUserdata"] = slot.is_userdata;
            return item;
        }

        static auto lua_call_stack_json(const GUI::LuaCallStackEntry& entry) -> Json
        {
            Json item = json_object();
            item["functionName"] = entry.function_name;
            item["source"] = entry.source;
            item["line"] = entry.line_number;
            item["currentLine"] = entry.current_line;
            item["what"] = entry.what;
            item["nameWhat"] = entry.name_what;
            return item;
        }

        auto lua_debug_snapshot() const -> Json
        {
            const auto snapshot = GUI::LuaDebugger::get().get_paused_snapshot();
            Json result = json_object();
            result["paused"] = snapshot.paused;
            result["modName"] = snapshot.mod_name;
            result["stateType"] = snapshot.state_type;
            result["source"] = snapshot.source;
            result["line"] = snapshot.line;
            result["generation"] = snapshot.generation;

            JsonArray stack_slots{};
            for (const auto& slot : snapshot.stack_slots)
            {
                stack_slots.emplace_back(lua_stack_slot_json(slot));
            }
            result["stack"] = std::move(stack_slots);

            JsonArray call_stack{};
            for (const auto& entry : snapshot.call_stack)
            {
                call_stack.emplace_back(lua_call_stack_json(entry));
            }
            result["callStack"] = std::move(call_stack);

            JsonArray frames{};
            for (const auto& frame : snapshot.stack_frames)
            {
                Json frame_json = json_object();
                frame_json["functionName"] = frame.function_name;
                frame_json["source"] = frame.source;
                frame_json["line"] = frame.line_number;
                frame_json["currentLine"] = frame.current_line;
                frame_json["what"] = frame.what;
                JsonArray locals{};
                for (const auto& local : frame.locals)
                {
                    Json local_json = json_object();
                    local_json["name"] = local.name;
                    local_json["type"] = local.type_name;
                    local_json["value"] = local.value_preview;
                    local_json["isTable"] = local.is_table;
                    local_json["isUserdata"] = local.is_userdata;
                    locals.emplace_back(std::move(local_json));
                }
                frame_json["locals"] = std::move(locals);
                frames.emplace_back(std::move(frame_json));
            }
            result["frames"] = std::move(frames);
            return result;
        }

        auto control_lua_debugger(const std::string& action) -> Json
        {
            auto& debugger = GUI::LuaDebugger::get();
            if (!debugger.is_paused())
            {
                throw McpError{"lifecycle.debugger_not_paused", "No Lua Hook state is paused"};
            }
            if (action == "continue")
            {
                debugger.continue_execution();
            }
            else if (action == "stepInto")
            {
                debugger.step_into();
            }
            else if (action == "stepOver")
            {
                debugger.step_over();
            }
            else if (action == "stepOut")
            {
                debugger.step_out();
            }
            else
            {
                throw McpError{"transport.invalid_arguments", fmt::format("Unknown Lua debug action '{}'", action)};
            }

            Json result = json_object();
            result["action"] = action;
            result["accepted"] = true;
            return result;
        }

        auto lua_errors_snapshot(size_t limit) const -> Json
        {
            const auto history = GUI::LuaDebugger::get().get_error_history();
            const size_t start = history.size() > limit ? history.size() - limit : 0;
            JsonArray errors{};
            for (size_t index = start; index < history.size(); ++index)
            {
                const auto& error = history[index];
                Json item = json_object();
                item["timestampMs"] = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(error.timestamp.time_since_epoch()).count());
                item["modName"] = error.mod_name;
                item["message"] = error.error_message;
                item["traceback"] = error.traceback;
                JsonArray call_stack{};
                for (const auto& entry : error.call_stack)
                {
                    call_stack.emplace_back(lua_call_stack_json(entry));
                }
                item["callStack"] = std::move(call_stack);
                JsonArray stack{};
                for (const auto& slot : error.stack_snapshot)
                {
                    stack.emplace_back(lua_stack_slot_json(slot));
                }
                item["stack"] = std::move(stack);
                errors.emplace_back(std::move(item));
            }
            Json result = json_object();
            result["errors"] = std::move(errors);
            result["total"] = static_cast<uint64_t>(history.size());
            return result;
        }

        auto discover_kismet_debugger_api() const -> const KismetDebuggerApiV1*
        {
            for (const auto& mod : m_program.m_mods)
            {
                auto* cpp_mod = dynamic_cast<CppMod*>(mod.get());
                if (!cpp_mod || !cpp_mod->is_started())
                {
                    continue;
                }
                const auto mod_name = to_string(cpp_mod->get_name());
                if (mod_name != "KismetDebuggerMod" && mod_name != "KismetDebugger")
                {
                    continue;
                }
                const auto accessor = reinterpret_cast<GetKismetDebuggerApi>(cpp_mod->get_export("ue4ss_get_kismet_debugger_api"));
                if (!accessor)
                {
                    return nullptr;
                }
                HMODULE debugger_module{};
                if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(accessor), &debugger_module))
                {
                    return nullptr;
                }
                const auto* api = accessor(kismet_debugger_api_version);
                if (!api || api->version != kismet_debugger_api_version || api->size < sizeof(KismetDebuggerApiV1))
                {
                    return nullptr;
                }
                return api;
            }
            return nullptr;
        }

        auto kismet_debugger_api() const -> const KismetDebuggerApiV1*
        {
            return m_kismet_api.load();
        }

        auto require_kismet_debugger_api() const -> const KismetDebuggerApiV1*
        {
            const auto* api = kismet_debugger_api();
            if (!api)
            {
                throw McpError{"debug.adapter_unavailable", "Compatible KismetDebuggerMod MCP interface is unavailable"};
            }
            return api;
        }

        auto kismet_debug_profile() const -> Json
        {
            Json result = json_object();
            const auto* api = kismet_debugger_api();
            result["available"] = api != nullptr;
            result["apiVersion"] = api ? api->version : uint32_t{};
            result["enabled"] = api && api->is_enabled && api->is_enabled();
            result["supportedActions"] = JsonArray{"pause", "continue", "stepInto", "stepOver", "stepOut"};
            if (!api)
            {
                result["missingCondition"] = "Loaded KismetDebuggerMod with MCP API version 1";
            }
            return result;
        }

        auto set_kismet_debugger_enabled(bool enabled) -> Json
        {
            const auto* api = require_kismet_debugger_api();
            if (enabled)
            {
                const bool was_enabled = api->is_enabled();
                if (!api->enable())
                {
                    throw McpError{"debug.enable_failed", api->last_error ? api->last_error() : "Kismet debugger enable failed"};
                }
                if (!was_enabled)
                {
                    m_kismet_enabled_by_mcp.store(true);
                }
            }
            else
            {
                api->disable();
                m_kismet_enabled_by_mcp.store(false);
            }
            return kismet_debug_snapshot();
        }

        auto kismet_debug_snapshot() const -> Json
        {
            const auto* api = require_kismet_debugger_api();
            if (!api->snapshot_json)
            {
                throw McpError{"debug.adapter_invalid", "Kismet debugger snapshot function is unavailable"};
            }
            const char* serialized = api->snapshot_json();
            if (!serialized)
            {
                throw McpError{"debug.snapshot_failed", "Kismet debugger returned no snapshot"};
            }
            return parse_json(serialized);
        }

        auto kismet_breakpoints_snapshot() const -> Json
        {
            const auto* api = require_kismet_debugger_api();
            const char* serialized = api->breakpoints_json ? api->breakpoints_json() : nullptr;
            if (!serialized)
            {
                throw McpError{"debug.snapshot_failed", "Kismet debugger returned no breakpoint snapshot"};
            }
            Json result = json_object();
            result["breakpoints"] = parse_json(serialized);
            return result;
        }

        auto set_kismet_breakpoint(const std::string& function_full_name, uint64_t bytecode_offset, bool enabled) -> Json
        {
            const auto* api = require_kismet_debugger_api();
            const bool changed = enabled ? api->add_breakpoint(function_full_name.c_str(), bytecode_offset)
                                         : api->remove_breakpoint(function_full_name.c_str(), bytecode_offset);
            if (!changed)
            {
                throw McpError{"debug.breakpoint_failed", api->last_error ? api->last_error() : "Kismet breakpoint operation failed"};
            }
            return kismet_breakpoints_snapshot();
        }

        auto control_kismet_debugger(const std::string& action) -> Json
        {
            const auto* api = require_kismet_debugger_api();
            if (!api->is_enabled())
            {
                throw McpError{"lifecycle.debugger_not_ready", "Kismet debugger is not enabled"};
            }
            if (action == "pause")
            {
                if (!api->pause_execution())
                {
                    throw McpError{"debug.pause_failed", "Kismet debugger could not request a pause"};
                }
            }
            else
            {
                const auto snapshot = kismet_debug_snapshot();
                const auto* paused = find_member(snapshot, "paused");
                if (!paused || !paused->is_boolean() || !paused->get<bool>())
                {
                    throw McpError{"lifecycle.debugger_not_paused", "No Kismet script is paused"};
                }
                if (action == "continue")
                {
                    api->continue_execution();
                }
                else if (action == "stepInto")
                {
                    api->step_into();
                }
                else if (action == "stepOver")
                {
                    api->step_over();
                }
                else if (action == "stepOut")
                {
                    api->step_out();
                }
                else
                {
                    throw McpError{"transport.invalid_arguments", fmt::format("Unknown Kismet debug action '{}'", action)};
                }
            }
            Json result = json_object();
            result["action"] = action;
            result["accepted"] = true;
            return result;
        }

        auto resources_list() -> Json
        {
            JsonArray resources{};
            const auto add = [&](std::string uri, std::string name, std::string description) {
                Json resource = json_object();
                resource["uri"] = std::move(uri);
                resource["name"] = std::move(name);
                resource["description"] = std::move(description);
                resource["mimeType"] = "application/json";
                resources.emplace_back(std::move(resource));
            };
            add("ue4ss://session", "Current UE4SS session", "Process, Unreal, World, thread and mod summary");
            add("ue4ss://capabilities", "Current capability evidence", "Refreshable available/degraded/unavailable capability states");
            add("ue4ss://mods", "Discovered mods", "Installed and started Lua/C++ mod states");
            add("ue4ss://logs", "UE4SS log page", "First bounded page of UE4SS.log; use ue4ss_logs_read for cursor pagination");
            Json result = json_object();
            result["resources"] = std::move(resources);
            return result;
        }

        auto read_resource(const std::string& uri) -> Json
        {
            Json data{};
            if (uri == "ue4ss://session")
            {
                data = run_on_event_loop([this] {
                    return session_snapshot();
                });
            }
            else if (uri == "ue4ss://capabilities")
            {
                data = run_on_event_loop([this] {
                    return capabilities_snapshot();
                });
            }
            else if (uri == "ue4ss://mods")
            {
                data = run_on_event_loop([this] {
                    return mods_snapshot();
                });
            }
            else if (uri == "ue4ss://logs")
            {
                data = read_log(JsonObject{});
            }
            else
            {
                throw McpError{"artifact.not_found", fmt::format("Unknown resource URI '{}'", uri)};
            }

            Json content = json_object();
            content["uri"] = uri;
            content["mimeType"] = "application/json";
            content["text"] = serialize_json(data);
            Json result = json_object();
            result["contents"] = JsonArray{std::move(content)};
            return result;
        }

        auto require_development_mode(const std::string& tool_name) const -> void
        {
            if (!m_development_mode)
            {
                throw McpError{"capability.unavailable", fmt::format("Tool '{}' requires MCP DevelopmentMode=1", tool_name)};
            }
        }

        auto log_path() const -> std::filesystem::path
        {
            return std::filesystem::path{m_program.get_working_directory()} / UE4SSProgram::m_log_file_name;
        }

        static auto json_rpc_error(const Json* id, int code, std::string message) -> Json
        {
            Json response = json_object();
            response["jsonrpc"] = "2.0";
            response["id"] = id ? *id : Json{nullptr};
            response["error"] = JsonObject{{"code", code}, {"message", std::move(message)}};
            return response;
        }

        static auto parse_content_length(std::string_view headers) -> size_t
        {
            const auto value = header_value(headers, "content-length");
            if (!value)
            {
                return 0;
            }
            size_t parsed{};
            try
            {
                parsed = static_cast<size_t>(std::stoull(std::string{*value}));
            }
            catch (...)
            {
                throw McpError{"transport.invalid_http", "Invalid Content-Length"};
            }
            if (parsed > maximum_http_request_size)
            {
                throw McpError{"transport.request_too_large", "MCP request exceeds the 1 MiB limit"};
            }
            return parsed;
        }

        static auto header_value(std::string_view headers, std::string_view requested_name) -> std::optional<std::string_view>
        {
            size_t line_start = headers.find("\r\n");
            if (line_start == std::string_view::npos)
            {
                return std::nullopt;
            }
            line_start += 2;
            while (line_start < headers.size())
            {
                const auto line_end = headers.find("\r\n", line_start);
                const auto line = headers.substr(line_start, line_end == std::string_view::npos ? headers.size() - line_start : line_end - line_start);
                const auto colon = line.find(':');
                if (colon != std::string_view::npos && lowercase(std::string{line.substr(0, colon)}) == requested_name)
                {
                    return trim(line.substr(colon + 1));
                }
                if (line_end == std::string_view::npos)
                {
                    break;
                }
                line_start = line_end + 2;
            }
            return std::nullopt;
        }

        static auto is_local_origin(std::string_view origin) -> bool
        {
            const auto normalized = lowercase(std::string{trim(origin)});
            const auto scheme_end = normalized.find("://");
            if (scheme_end == std::string::npos)
            {
                return false;
            }
            const auto scheme = std::string_view{normalized}.substr(0, scheme_end);
            if (scheme != "http" && scheme != "https")
            {
                return false;
            }

            const auto authority = std::string_view{normalized}.substr(scheme_end + 3);
            const auto has_valid_port = [](std::string_view suffix) {
                if (suffix.empty())
                {
                    return true;
                }
                if (suffix.front() != ':' || suffix.size() == 1 || !std::ranges::all_of(suffix.substr(1), [](unsigned char value) {
                        return std::isdigit(value) != 0;
                    }))
                {
                    return false;
                }
                try
                {
                    const auto port = std::stoul(std::string{suffix.substr(1)});
                    return port <= std::numeric_limits<uint16_t>::max();
                }
                catch (...)
                {
                    return false;
                }
            };

            for (const auto host : {std::string_view{"localhost"}, std::string_view{"127.0.0.1"}})
            {
                if (authority.starts_with(host) && has_valid_port(authority.substr(host.size())))
                {
                    return true;
                }
            }
            return false;
        }

        static auto send_http(SOCKET client, int status, std::string body) -> void
        {
            const std::string_view reason = status == 200   ? "OK"
                                            : status == 202 ? "Accepted"
                                            : status == 400 ? "Bad Request"
                                            : status == 403 ? "Forbidden"
                                            : status == 404 ? "Not Found"
                                            : status == 405 ? "Method Not Allowed"
                                                            : "Internal Server Error";
            const std::string_view allow_header = status == 405 ? "Allow: POST\r\n" : "";
            const auto response = fmt::format(
                    "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nMCP-Protocol-Version: {}\r\n{}Content-Length: {}\r\nConnection: close\r\n\r\n{}",
                    status,
                    reason,
                    protocol_version,
                    allow_header,
                    body.size(),
                    body);
            size_t sent_total{};
            while (sent_total < response.size())
            {
                const auto sent = send(client, response.data() + sent_total, static_cast<int>(response.size() - sent_total), 0);
                if (sent <= 0)
                {
                    break;
                }
                sent_total += static_cast<size_t>(sent);
            }
        }

        struct ProcessEventHook
        {
            Unreal::Hook::GlobalCallbackId callback_id;
            std::string function_full_name;
            std::optional<std::string> object_class;
            std::string phase;
        };

        static constexpr size_t max_hook_events = 4096;
        static constexpr size_t max_journal_entries = 4096;

      private:
        UE4SSProgram& m_program;
        inline static thread_local std::shared_ptr<std::atomic_bool> s_current_request_cancellation{};
        mutable std::mutex m_request_mutex{};
        std::unordered_map<std::string, std::shared_ptr<std::atomic_bool>> m_active_requests{};
        std::string m_bind_address;
        uint16_t m_requested_port{};
        std::chrono::milliseconds m_operation_timeout{};
        bool m_development_mode{};
        std::string m_session_id;
        std::atomic_uint64_t m_next_operation_id{1};
        std::atomic_uint64_t m_next_task_id{1};
        mutable std::mutex m_journal_mutex{};
        uint64_t m_next_operation_sequence{1};
        uint64_t m_next_event_sequence{1};
        std::deque<Json> m_operations{};
        std::deque<Json> m_events{};
        mutable std::mutex m_tasks_mutex{};
        std::unordered_map<std::string, std::shared_ptr<AsyncTask>> m_tasks{};
        std::unordered_map<std::string, std::filesystem::path> m_stopped_lua_mod_paths{};
        std::atomic_bool m_dumper_task_active{false};
        std::atomic_bool m_running{};
        bool m_winsock_initialized{};
        std::atomic<SOCKET> m_listen_socket{INVALID_SOCKET};
        std::array<std::jthread, 4> m_client_threads{};
        std::mutex m_client_mutex{};
        std::condition_variable m_client_condition{};
        std::deque<SOCKET> m_pending_clients{};
        std::unordered_set<SOCKET> m_active_clients{};
        uint16_t m_bound_port{};
        std::jthread m_thread{};
        Unreal::Hook::GlobalCallbackId m_game_thread_callback_id{Unreal::Hook::ERROR_ID};
        std::mutex m_game_thread_queue_mutex{};
        std::vector<std::function<void()>> m_game_thread_queue{};
        std::atomic_uint64_t m_next_property_watch_id{1};
        mutable std::mutex m_property_watches_mutex{};
        std::unordered_map<std::string, PropertyWatch> m_property_watches{};
        std::vector<std::string> m_property_watch_order{};
        size_t m_property_watch_rotation{};
        std::atomic_uint64_t m_next_hook_id{1};
        std::atomic_bool m_kismet_enabled_by_mcp{false};
        std::atomic<const KismetDebuggerApiV1*> m_kismet_api{nullptr};
        std::atomic_uint64_t m_next_hook_event_sequence{1};
        std::mutex m_hook_mutex{};
        std::unordered_map<std::string, ProcessEventHook> m_process_event_hooks{};
        std::deque<Json> m_hook_events{};
        Unreal::UWorld* m_last_world{};
        uint64_t m_world_generation{};
        bool m_abandoned{};
    };

    Server::Server(UE4SSProgram& program, std::string bind_address, uint16_t port, int64_t operation_timeout_ms, bool development_mode)
        : m_impl(std::make_unique<Impl>(program, std::move(bind_address), port, operation_timeout_ms, development_mode))
    {
    }

    Server::~Server()
    {
        if (m_impl && m_impl->abandoned())
        {
            // Process detach runs under loader lock. The OS owns remaining thread/socket cleanup.
            (void)m_impl.release();
        }
    }

    auto Server::start() -> bool
    {
        return m_impl->start();
    }
    auto Server::activate_runtime() -> void
    {
        m_impl->activate_runtime();
    }

    auto Server::stop() -> void
    {
        m_impl->stop();
    }

    auto Server::abandon_for_process_exit() -> void
    {
        m_impl->abandon_for_process_exit();
    }

    auto Server::port() const -> uint16_t
    {
        return m_impl->port();
    }
} // namespace RC::Mcp
