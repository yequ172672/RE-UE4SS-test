#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace RC
{
    class UE4SSProgram;

    namespace Mcp
    {
        class Server
        {
          public:
            Server(UE4SSProgram& program, std::string bind_address, uint16_t port, int64_t operation_timeout_ms, bool development_mode);
            ~Server();

            Server(const Server&) = delete;
            Server(Server&&) = delete;
            auto operator=(const Server&) -> Server& = delete;
            auto operator=(Server&&) -> Server& = delete;

            auto start() -> bool;
            auto activate_runtime() -> void;
            auto stop() -> void;
            [[nodiscard]] auto port() const -> uint16_t;
            auto abandon_for_process_exit() -> void;

          private:
            class Impl;
            std::unique_ptr<Impl> m_impl;
        };
    } // namespace Mcp
} // namespace RC
