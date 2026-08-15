#include <string>
#include <atomic>

#include <GUI/GUITab.hpp>
#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>
#include <KismetDebugger.hpp>

class KismetDebuggerMod : public RC::CppUserModBase
{
  private:
    RC::GUI::KismetDebuggerMod::Debugger m_debugger{};

  public:
    KismetDebuggerMod() : CppUserModBase()
    {
        ModName = STR("KismetDebugger");
        ModVersion = STR("1.0");
        ModDescription = STR("Debugging interface for kismet bytecode");
        ModAuthors = STR("truman");

        register_tab(STR("Kismet Debugger"), [](CppUserModBase* mod) {
            UE4SS_ENABLE_IMGUI()
            dynamic_cast<KismetDebuggerMod*>(mod)->m_debugger.render();
        });
    }

    ~KismetDebuggerMod() override = default;
    auto debugger() -> RC::GUI::KismetDebuggerMod::Debugger&
    {
        return m_debugger;
    }
};
static std::atomic<KismetDebuggerMod*> g_instance{};

static auto debugger_instance() -> RC::GUI::KismetDebuggerMod::Debugger*
{
    if (auto* instance = g_instance.load())
    {
        return &instance->debugger();
    }
    return nullptr;
}

#define KISMET_DEBUGGER_MOD_API __declspec(dllexport)
extern "C"
{
    KISMET_DEBUGGER_MOD_API RC::CppUserModBase* start_mod()
    {
        auto* mod = new KismetDebuggerMod();
        g_instance.store(mod);
        return mod;
    }

    KISMET_DEBUGGER_MOD_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        g_instance.store(nullptr);
        delete mod;
    }

    KISMET_DEBUGGER_MOD_API const RC::GUI::KismetDebuggerMod::McpApi* ue4ss_get_kismet_debugger_api(uint32_t requested_version)
    {
        using Api = RC::GUI::KismetDebuggerMod::McpApi;
        static const Api api{
                .version = RC::GUI::KismetDebuggerMod::MCP_API_VERSION,
                .size = sizeof(Api),
                .enable =
                        [] {
                            auto* debugger = debugger_instance();
                            return debugger && debugger->enable();
                        },
                .disable =
                        [] {
                            if (auto* debugger = debugger_instance()) debugger->disable();
                        },
                .is_enabled =
                        [] {
                            auto* debugger = debugger_instance();
                            return debugger && debugger->is_enabled();
                        },
                .pause_execution =
                        [] {
                            auto* debugger = debugger_instance();
                            return debugger && debugger->pause_execution();
                        },
                .continue_execution =
                        [] {
                            if (auto* debugger = debugger_instance()) debugger->continue_execution();
                        },
                .step_into =
                        [] {
                            if (auto* debugger = debugger_instance()) debugger->step_into();
                        },
                .step_over =
                        [] {
                            if (auto* debugger = debugger_instance()) debugger->step_over();
                        },
                .step_out =
                        [] {
                            if (auto* debugger = debugger_instance()) debugger->step_out();
                        },
                .snapshot_json =
                        [] {
                            thread_local std::string value;
                            value = debugger_instance() ? debugger_instance()->snapshot_json() : R"({"enabled":false,"paused":false})";
                            return value.c_str();
                        },
                .breakpoints_json =
                        [] {
                            thread_local std::string value;
                            value = debugger_instance() ? debugger_instance()->breakpoints_json() : "{}";
                            return value.c_str();
                        },
                .add_breakpoint =
                        [](const char* function_full_name, uint64_t bytecode_offset) {
                            auto* debugger = debugger_instance();
                            return debugger && function_full_name && debugger->add_breakpoint(function_full_name, static_cast<size_t>(bytecode_offset));
                        },
                .remove_breakpoint =
                        [](const char* function_full_name, uint64_t bytecode_offset) {
                            auto* debugger = debugger_instance();
                            return debugger && function_full_name && debugger->remove_breakpoint(function_full_name, static_cast<size_t>(bytecode_offset));
                        },
                .last_error =
                        [] {
                            thread_local std::string value;
                            value = debugger_instance() ? debugger_instance()->last_error() : "Kismet debugger instance is unavailable";
                            return value.c_str();
                        },
        };
        return requested_version == api.version ? &api : nullptr;
    }
}
