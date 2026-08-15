#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Unreal/FFrame.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>

namespace RC::GUI::KismetDebuggerMod
{
    using namespace RC::Unreal;

    auto expr_to_string(EExprToken expr) -> const char*;
    inline constexpr uint32_t MCP_API_VERSION = 1;

    struct McpApi
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
        bool (*add_breakpoint)(const char* function_full_name, uint64_t bytecode_offset);
        bool (*remove_breakpoint)(const char* function_full_name, uint64_t bytecode_offset);
        const char* (*last_error)();
    };

    struct PausedContext
    {
        EExprToken expr{};
        UObject* context{};
        FFrame* stack{};
    };

    class BreakpointStore
    {
      public:
        BreakpointStore();
        ~BreakpointStore();

        auto load(std::filesystem::path& path) -> void;
        auto save() -> void;

        auto has_breakpoint(UFunction* fn, size_t index) -> bool;
        auto add_breakpoint(UFunction* fn, size_t index) -> void;
        auto add_breakpoint(const StringType& fn, size_t index) -> void;
        auto remove_breakpoint(UFunction* fn, size_t index) -> void;
        auto remove_breakpoint(const StringType& fn, size_t index) -> void;
        auto breakpoints_json() const -> std::string;

      private:
        typedef std::unordered_set<size_t> FunctionBreakpoints;

        std::unordered_map<UFunction*, std::shared_ptr<FunctionBreakpoints>> m_breakpoints_by_function{};
        std::unordered_map<StringType, std::shared_ptr<FunctionBreakpoints>> m_breakpoints_by_name{};
        mutable std::mutex m_mutex;
    };

    class Debugger
    {
      public:
        Debugger();
        ~Debugger();

        auto enable() -> bool;
        auto disable() -> void;
        auto is_enabled() const -> bool;
        auto pause_execution() -> bool;
        auto continue_execution() -> void;
        auto step_into() -> void;
        auto step_over() -> void;
        auto step_out() -> void;
        auto snapshot_json() const -> std::string;
        auto breakpoints_json() const -> std::string;
        auto add_breakpoint(const std::string& function_full_name, size_t bytecode_offset) -> bool;
        auto remove_breakpoint(const std::string& function_full_name, size_t bytecode_offset) -> bool;
        auto last_error() const -> std::string;

        auto render() -> void;
        auto render_nav_bar(float width) -> void;

        auto nav_to_function(UFunction* fn) -> void;
        auto nav_to_function(std::string full_name) -> void;

      private:
        bool m_paused{};

        // std::string because it's used primarily with ImGui
        std::unordered_map<std::string, UFunction*> m_function_name_map{};
        std::vector<std::string> m_nav_history{};
        int m_nav_history_index{-1};
        std::string m_nav_function{""};

        // layout
        float m_split_right{400.0};
        float m_split_left{400.0};

        uint8_t* m_last_code{nullptr}; // pointer to last stack instruction, used to know if it's advanced since last frame
        BreakpointStore& m_breakpoints;
        mutable std::mutex m_error_mutex;
        std::string m_last_error;

      public:
        static inline std::filesystem::path m_save_path;
    };

    class ScriptRenderContext
    {
      public:
        ScriptRenderContext(std::optional<PausedContext>& paused_context, UFunction* fn, bool scroll_to_active, BreakpointStore& breakpoints);
        ~ScriptRenderContext();

        auto render() -> void;

      private:
        auto render_property();
        auto render_expr() -> EExprToken;

        template <typename T>
        auto read() -> T
        {
            T t;
            memcpy(&t, &m_script[m_index], sizeof(T));
            m_index += sizeof(T);
            return t;
        }

        auto read_object() -> UObject*
        {
            return (UObject*)this->read<uint64_t>();
        }
        auto read_name() -> FName
        {
            FName n = read<FName>();
            read<uint32_t>(); // not sure what this is for
            return n;
        }

      private:
        std::optional<PausedContext>& m_paused_context;
        UFunction* m_fn{};

        bool m_scroll_to_active{};
        BreakpointStore& m_breakpoints;

        int m_indent{0};

        uint8_t* m_script{};
        int m_script_size{};
        int m_index{};
        int m_cur{}; // or -1
        EExprToken m_current_expr{};
    };
} // namespace RC::GUI::KismetDebuggerMod
