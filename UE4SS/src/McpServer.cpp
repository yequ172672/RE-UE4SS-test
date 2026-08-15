#define NOMINMAX

#include <McpServer.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <DynamicOutput/DynamicOutput.hpp>
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
#include <Unreal/World.hpp>
#include <Unreal/UGameViewportClient.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <Unreal/UnrealVersion.hpp>
#include <fmt/format.h>
#include <glaze/glaze.hpp>

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
            static constexpr std::string_view definitions = R"json(
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
    }
  ]
}
)json";
            return parse_json(definitions);
        }
    } // namespace

    class Server::Impl
    {
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

            m_thread = std::jthread{[this] {
                serve();
            }};
            Output::send(STR("MCP: Listening at http://{}:{}/mcp (session {}).\n"), ensure_str(m_bind_address), m_bound_port, ensure_str(m_session_id));
            return true;
        }

        auto activate_runtime() -> void
        {
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
            const auto listen_socket = m_listen_socket.exchange(INVALID_SOCKET);
            if (listen_socket != INVALID_SOCKET)
            {
                shutdown(listen_socket, SD_BOTH);
                closesocket(listen_socket);
            }
            const auto client_socket = m_client_socket.exchange(INVALID_SOCKET);
            if (client_socket != INVALID_SOCKET)
            {
                shutdown(client_socket, SD_BOTH);
                closesocket(client_socket);
            }
            if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id())
            {
                m_thread.join();
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
            const auto client_socket = m_client_socket.exchange(INVALID_SOCKET);
            if (client_socket != INVALID_SOCKET)
            {
                closesocket(client_socket);
            }
            if (m_thread.joinable())
            {
                m_thread.detach();
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
                m_client_socket = client;
                handle_client(client);
                if (m_client_socket.exchange(INVALID_SOCKET) == client)
                {
                    shutdown(client, SD_BOTH);
                    closesocket(client);
                }
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
                    if (rpc_method == "notifications/initialized" || rpc_method.starts_with("notifications/"))
                    {
                        send_http(client, 202, {});
                        return;
                    }
                    send_http(client, 202, {});
                    return;
                }

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
            if (status == "succeeded" && (target == "ue4ss_lua_execute" || target == "ue4ss_lua_mod_reload"))
            {
                Json change = json_object();
                change["kind"] = target == "ue4ss_lua_execute" ? "lua_execution" : "lua_mod_reload";
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

            Json result = json_object();
            result["structuredContent"] = operation;
            Json text_content = json_object();
            text_content["type"] = "text";
            text_content["text"] = serialize_json(operation);
            result["content"] = JsonArray{std::move(text_content)};
            result["isError"] = error.has_value();
            return result;
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
            if (future.wait_for(m_operation_timeout) == std::future_status::ready)
            {
                return future.get();
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
                    m_last_world = current_world;
                    ++m_world_generation;
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
                    lua_settop(lua, base);
                    throw McpError{"lua.compile_failed", message};
                }
                if (lua_pcall(lua, 0, LUA_MULTRET, 0) != LUA_OK)
                {
                    const char* message = lua_tostring(lua, -1);
                    luaL_traceback(lua, lua, message ? message : "Lua execution failed", 1);
                    const std::string traceback = lua_tostring(lua, -1) ? lua_tostring(lua, -1) : "Lua execution failed";
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

      private:
        UE4SSProgram& m_program;
        std::string m_bind_address;
        uint16_t m_requested_port{};
        std::chrono::milliseconds m_operation_timeout{};
        bool m_development_mode{};
        std::string m_session_id;
        std::atomic_uint64_t m_next_operation_id{1};
        std::atomic_bool m_running{};
        bool m_winsock_initialized{};
        std::atomic<SOCKET> m_listen_socket{INVALID_SOCKET};
        std::atomic<SOCKET> m_client_socket{INVALID_SOCKET};
        uint16_t m_bound_port{};
        std::jthread m_thread{};
        Unreal::Hook::GlobalCallbackId m_game_thread_callback_id{Unreal::Hook::ERROR_ID};
        std::mutex m_game_thread_queue_mutex{};
        std::vector<std::function<void()>> m_game_thread_queue{};
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
