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
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <unordered_set>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <DynamicOutput/DynamicOutput.hpp>
#include <GUI/LuaDebugger.hpp>
#include <Helpers/String.hpp>
#include <Mod/CppMod.hpp>
#include <Mod/LuaMod.hpp>
#include <UE4SSRuntime.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
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
      "name": "ue4ss_objects_find",
      "description": "Find live UObject instances by reflected class name and return bounded results with path, address and lifecycle-safe session/index/serial handles. Read-only but may briefly scan runtime objects. Requires initialized GUObjectArray and the UE4SS event loop.",
      "inputSchema": {"type":"object","required":["className"],"properties":{"className":{"type":"string","minLength":1},"limit":{"type":"integer","minimum":1,"maximum":500}},"additionalProperties":false},
      "annotations": {"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}
    },
    {
      "name": "ue4ss_console_execute",
      "description": "Execute an Unreal console command through the current GameViewportClient on the game thread. Returns whether Unreal handled the command; textual command output remains available through ue4ss_logs_read. Requires DevelopmentMode=1, a live viewport and the UE4SS event loop.",
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
            Json state{json_object()};
            std::jthread worker{};
        };

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
                else if (name == "ue4ss_objects_find")
                {
                    const auto class_name = required_string(arguments, "className");
                    const auto limit = std::clamp<int64_t>(optional_integer(arguments, "limit", 100), 1, 500);
                    data = run_on_game_thread([this, class_name, limit] {
                        return find_objects(class_name, static_cast<size_t>(limit));
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
                return tool_result(name, "succeeded", std::move(data), std::nullopt);
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
            if (status == "succeeded" &&
                (target == "ue4ss_lua_execute" || target == "ue4ss_lua_mod_reload" || target == "ue4ss_console_execute" || target == "ue4ss_hook_register" ||
                 target == "ue4ss_hook_unregister" || target == "ue4ss_property_write" || target == "ue4ss_function_call"))
            {
                Json change = json_object();
                change["kind"] = target == "ue4ss_lua_execute"       ? "lua_execution"
                                 : target == "ue4ss_lua_mod_reload"  ? "lua_mod_reload"
                                 : target == "ue4ss_console_execute" ? "console_execution"
                                 : target == "ue4ss_hook_register"   ? "hook_registration"
                                 : target == "ue4ss_hook_unregister" ? "hook_unregistration"
                                 : target == "ue4ss_property_write"  ? "property_write"
                                                                     : "function_call";
                change["applied"] = true;
                operation["actualChanges"].get<JsonArray>().emplace_back(std::move(change));
            }
            operation["restartRequired"] = false;
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
            return copy_task_state(task);
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
        }

        auto execute_console(const std::string& command) -> Json
        {
            auto* viewport = static_cast<Unreal::UGameViewportClient*>(Unreal::UObjectGlobals::FindFirstOf(std::string_view{"GameViewportClient"}));
            if (!viewport)
            {
                throw McpError{"lifecycle.viewport_not_ready", "No live GameViewportClient was found"};
            }

            Unreal::FOutputDevice output_device{};
            const auto wide_command = ensure_str(command);
            const auto handled = viewport->ProcessConsoleExec(wide_command.c_str(), output_device, nullptr);
            Json result = json_object();
            result["command"] = command;
            result["handled"] = handled;
            result["outputResource"] = "ue4ss://logs";
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

            const bool has_started_lua_mod = std::ranges::any_of(m_program.m_mods, [](const auto& mod) {
                const auto* lua_mod = dynamic_cast<const LuaMod*>(mod.get());
                return lua_mod && lua_mod->is_started() && lua_mod->m_hook_lua;
            });
            add("lua.execute",
                m_development_mode && has_started_lua_mod && m_game_thread_callback_id != Unreal::Hook::ERROR_ID ? "available" : "degraded",
                m_development_mode ? "development mode enabled" : "development mode disabled",
                has_started_lua_mod ? "UEngine::Tick game-thread callback" : "DevelopmentMode=1 and a started Lua mod hook state");
            add("lua.reload", m_development_mode ? "available" : "unavailable", "UE4SS Lua reinstall path", "DevelopmentMode=1");
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
                m_game_thread_callback_id != Unreal::Hook::ERROR_ID ? "UEngine::Tick game-thread callback is registered"
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

        auto find_objects(const std::string& class_name, size_t limit) -> Json
        {
            if (!Unreal::UObjectArray::GetGUObjectArrayAddress())
            {
                throw McpError{"lifecycle.runtime_not_ready", "GUObjectArray is not initialized"};
            }
            (void)world_snapshot();

            const Unreal::FName class_to_find{ensure_str(class_name)};
            JsonArray objects{};
            objects.reserve(limit);
            uint64_t matched{};
            bool truncated{};
            Unreal::UObjectGlobals::ForEachUObject([&](Unreal::UObject* object, ...) {
                if (!object || object->IsUnreachable())
                {
                    return LoopAction::Continue;
                }
                auto* object_class = object->GetClassPrivate();
                if (!object_class)
                {
                    return LoopAction::Continue;
                }

                bool class_matches = object_class->GetNamePrivate().Equals(class_to_find);
                if (!class_matches)
                {
                    for (Unreal::UStruct* super_struct : Unreal::TSuperStructRange(object_class))
                    {
                        if (super_struct->GetNamePrivate().Equals(class_to_find))
                        {
                            class_matches = true;
                            break;
                        }
                    }
                }
                if (!class_matches)
                {
                    return LoopAction::Continue;
                }

                ++matched;
                if (objects.size() >= limit)
                {
                    truncated = true;
                    return LoopAction::Break;
                }

                const auto index = object->GetInternalIndex();
                auto* object_item = Unreal::UObjectArray::IndexToObject(index);
                if (!object_item || !object_item->IsValid(false) || object_item->GetUObject() != object)
                {
                    return LoopAction::Continue;
                }

                const auto serial_number = Unreal::UObjectArray::AllocateSerialNumber(index);
                Json item = json_object();
                item["handle"] = fmt::format("{}:{}:{}:{}", m_session_id, m_world_generation, index, serial_number);
                item["name"] = to_string(object->GetName());
                item["fullName"] = to_string(object->GetFullName());
                item["path"] = to_string(object->GetPathName());
                item["class"] = to_string(object_class->GetName());
                item["address"] = fmt::format("0x{:X}", reinterpret_cast<uintptr_t>(object));
                item["worldGeneration"] = m_world_generation;
                item["objectIndex"] = index;
                item["serialNumber"] = serial_number;
                item["lifecycle"] = "valid";
                objects.emplace_back(std::move(item));
                return LoopAction::Continue;
            });

            Json result = json_object();
            result["className"] = class_name;
            result["objects"] = std::move(objects);
            result["returned"] = static_cast<uint64_t>(result["objects"].get<JsonArray>().size());
            result["matchedBeforeLimit"] = matched;
            result["truncated"] = truncated;
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
            if (!reloaded_mod)
            {
                throw McpError{"mod.reload_failed", fmt::format("Lua mod '{}' disappeared during reload", mod_name)};
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
