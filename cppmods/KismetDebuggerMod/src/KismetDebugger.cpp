#include <KismetDebugger.hpp>

#include <vector>
#include <atomic>
#include <condition_variable>
#include <unordered_map>
#include <iostream>
#include <thread>
#include <ranges>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/Script.hpp>
#include <Unreal/ReflectedFunction.hpp>
#include <Unreal/Signatures.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <glaze/glaze.hpp>

#include "Profiler/Profiler.hpp"

#include <UE4SSProgram.hpp>

namespace RC::GUI::KismetDebuggerMod
{

    using DebugJson = glz::generic;
    using DebugJsonArray = glz::generic::array_t;
    using DebugJsonObject = glz::generic::object_t;

    static auto debug_json_object() -> DebugJson
    {
        return DebugJsonObject{};
    }

    static auto serialize_debug_json(const DebugJson& value) -> std::string
    {
        if (auto serialized = glz::write_json(value))
        {
            return std::move(serialized.value());
        }
        return "{}";
    }

    using namespace RC::Unreal;

    FNativeFuncPtr GNativesOriginal[EExprToken::EX_Max];
    std::atomic<bool> is_hooked{false};
    std::atomic<bool> should_pause{false};
    std::atomic<bool> is_paused{false};
    std::optional<PausedContext> context;
    std::mutex context_mutex;
    std::mutex pause_mutex;
    std::mutex hook_lifecycle_mutex;
    std::condition_variable pause_cv;
    std::condition_variable hooks_drained_cv;
    std::atomic<bool> resume_requested{false};
    std::atomic<uint32_t> active_hooks{0};
    enum class StepMode : uint8_t
    {
        None,
        Into,
        Over,
        Out,
    };
    std::atomic<StepMode> step_mode{StepMode::None};
    std::atomic<size_t> step_depth{0};

    BreakpointStore g_breakpoints;

    static auto call_depth(FFrame& stack) -> size_t
    {
        size_t depth{};
        for (auto* frame = &stack; frame; frame = frame->PreviousFrame())
        {
            ++depth;
        }
        return depth;
    }

    void hook_expr_internal(UObject* Context, FFrame& Stack, void* RESULT_DECL, EExprToken N)
    {
        active_hooks.fetch_add(1);
        const auto hook_exit = std::unique_ptr<void, void (*)(void*)>{reinterpret_cast<void*>(1), [](void*) {
                                                                          if (active_hooks.fetch_sub(1) == 1)
                                                                          {
                                                                              hooks_drained_cv.notify_all();
                                                                          }
                                                                      }};

        UFunction* fn = Stack.Node();
        StringType name = fn->GetFullName();
        ProfilerTransientScopeNamed(scope, to_string(name).c_str(), true);

        const size_t index = Stack.Code() - fn->GetScript().GetData() - 1;
        const size_t depth = call_depth(Stack);
        const auto mode = step_mode.load();
        const bool step_match =
                mode == StepMode::Into || (mode == StepMode::Over && depth <= step_depth.load()) || (mode == StepMode::Out && depth < step_depth.load());
        if (should_pause.exchange(false) || step_match || g_breakpoints.has_breakpoint(fn, index))
        {
            step_mode.store(StepMode::None);
            step_depth.store(depth);
            {
                std::lock_guard<std::mutex> context_lock(context_mutex);
                context = PausedContext{.expr = N, .context = Context, .stack = &Stack};
            }
            is_paused.store(true);

            std::unique_lock<std::mutex> pause_lock(pause_mutex);
            pause_cv.wait(pause_lock, [] {
                return resume_requested.exchange(false);
            });
            {
                std::lock_guard<std::mutex> context_lock(context_mutex);
                context.reset();
            }
            is_paused.store(false);
        }

        GNativesOriginal[N](Context, Stack, RESULT_DECL);
    }

    template <unsigned N>
    void hook_expr(UObject* Context, FFrame& Stack, void* RESULT_DECL)
    {
        hook_expr_internal(Context, Stack, RESULT_DECL, static_cast<EExprToken>(N));
    }

    template <unsigned N>
    void hook_all()
    {
        GNativesOriginal[N - 1] = GNatives_Internal[N - 1];
        GNatives_Internal[N - 1] = &hook_expr<N - 1>;
        hook_all<N - 1>();
    }
    template <>
    void hook_all<0>()
    {
    }

    typedef std::unordered_map<std::string, std::unordered_set<size_t>> JsonBreakpoints;

    BreakpointStore::BreakpointStore()
    {
    }
    BreakpointStore::~BreakpointStore()
    {
    }
    auto BreakpointStore::load(std::filesystem::path& path) -> void
    {
        JsonBreakpoints breakpoints{};
        if (const auto error = glz::read_file_json(breakpoints, path.string(), std::string{}); error)
        {
            return;
        }
        std::lock_guard lock{m_mutex};
        m_breakpoints_by_function.clear();
        m_breakpoints_by_name.clear();
        for (const auto& [function_name, offsets] : breakpoints)
        {
            m_breakpoints_by_name.emplace(ensure_str(function_name), std::make_shared<FunctionBreakpoints>(offsets));
        }
    }

    auto BreakpointStore::save() -> void
    {
        JsonBreakpoints breakpoints{};
        {
            std::lock_guard lock{m_mutex};
            for (const auto& [function_name, offsets] : m_breakpoints_by_name)
            {
                if (offsets && !offsets->empty())
                {
                    breakpoints[to_string(function_name)] = *offsets;
                }
            }
        }
        (void)glz::write_file_json(breakpoints, Debugger::m_save_path.string(), std::string{});
    }

    auto BreakpointStore::has_breakpoint(UFunction* fn, size_t index) -> bool
    {
        std::lock_guard lock{m_mutex};
        auto& offsets = m_breakpoints_by_function[fn];
        if (!offsets)
        {
            const auto by_name = m_breakpoints_by_name.find(fn->GetFullName());
            if (by_name != m_breakpoints_by_name.end())
            {
                offsets = by_name->second;
            }
        }
        return offsets && offsets->contains(index);
    }

    auto BreakpointStore::add_breakpoint(UFunction* fn, size_t index) -> void
    {
        {
            std::lock_guard lock{m_mutex};
            auto& by_name = m_breakpoints_by_name[fn->GetFullName()];
            if (!by_name)
            {
                by_name = std::make_shared<FunctionBreakpoints>();
            }
            by_name->insert(index);
            m_breakpoints_by_function[fn] = by_name;
        }
        save();
    }

    auto BreakpointStore::add_breakpoint(const StringType& fn, size_t index) -> void
    {
        {
            std::lock_guard lock{m_mutex};
            auto& offsets = m_breakpoints_by_name[fn];
            if (!offsets)
            {
                offsets = std::make_shared<FunctionBreakpoints>();
            }
            offsets->insert(index);
        }
        save();
    }

    auto BreakpointStore::remove_breakpoint(UFunction* fn, size_t index) -> void
    {
        remove_breakpoint(fn->GetFullName(), index);
    }

    auto BreakpointStore::remove_breakpoint(const StringType& fn, size_t index) -> void
    {
        {
            std::lock_guard lock{m_mutex};
            if (const auto found = m_breakpoints_by_name.find(fn); found != m_breakpoints_by_name.end() && found->second)
            {
                found->second->erase(index);
            }
        }
        save();
    }

    auto BreakpointStore::breakpoints_json() const -> std::string
    {
        JsonBreakpoints breakpoints{};
        {
            std::lock_guard lock{m_mutex};
            for (const auto& [function_name, offsets] : m_breakpoints_by_name)
            {
                if (offsets && !offsets->empty())
                {
                    breakpoints[to_string(function_name)] = *offsets;
                }
            }
        }
        if (auto serialized = glz::write_json(breakpoints))
        {
            return std::move(serialized.value());
        }
        return "{}";
    }

    Debugger::Debugger() : m_breakpoints(g_breakpoints)
    {
        m_save_path = StringType{UE4SSProgram::get_program().get_working_directory()} + fmt::format(STR("\\Mods\\KismetDebugger\\config\\breakpoints.json"));
    }
    Debugger::~Debugger()
    {
        disable();
    }

    auto ImGui_Splitter(bool split_vertically,
                        float thickness,
                        float* size1,
                        float* size2,
                        float min_size1,
                        float min_size2,
                        float splitter_long_axis_size,
                        const char* id_str = "##Splitter") -> bool
    {
        using namespace ImGui;
        ImGuiContext& g = *GImGui;
        ImGuiWindow* window = g.CurrentWindow;
        ImGuiID id = window->GetID(id_str);
        ImRect bb;
        bb.Min = window->DC.CursorPos + (split_vertically ? ImVec2(*size1, 0.0f) : ImVec2(0.0f, *size1));
        bb.Max = bb.Min + CalcItemSize(split_vertically ? ImVec2(thickness, splitter_long_axis_size) : ImVec2(splitter_long_axis_size, thickness), 0.0f, 0.0f);
        return SplitterBehavior(bb, id, split_vertically ? ImGuiAxis_X : ImGuiAxis_Y, size1, size2, min_size1, min_size2, 0.0f);
    }

    auto Debugger::enable() -> bool
    {
        std::scoped_lock lifecycle_lock{hook_lifecycle_mutex};
        if (is_hooked.load())
        {
            return true;
        }

        try
        {
            m_breakpoints.load(m_save_path);
        }
        catch (const std::exception& error)
        {
            Output::send<LogLevel::Warning>(STR("[KismetDebugger]: Failed to load breakpoints: {}\n"), ensure_str(error.what()));
        }

        if (!GNatives_Internal)
        {
            const std::string error = "GNatives not found";
            {
                std::lock_guard lock{m_error_mutex};
                m_last_error = error;
            }
            Output::send<LogLevel::Error>(STR("[KismetDebugger]: {}.\n"), ensure_str(error));
            return false;
        }

        resume_requested.store(false);
        hook_all<EExprToken::EX_Max>();
        is_hooked.store(true);
        return true;
    }

    auto Debugger::disable() -> void
    {
        std::scoped_lock lifecycle_lock{hook_lifecycle_mutex};
        if (!is_hooked.exchange(false))
        {
            return;
        }
        for (int i = 0; i < EExprToken::EX_Max; ++i)
        {
            GNatives_Internal[i] = GNativesOriginal[i];
        }
        should_pause.store(false);
        step_mode.store(StepMode::None);
        resume_requested.store(true);
        pause_cv.notify_all();

        std::unique_lock lock{pause_mutex};
        hooks_drained_cv.wait(lock, [] {
            return active_hooks.load() == 0;
        });
    }

    auto Debugger::is_enabled() const -> bool
    {
        return is_hooked.load();
    }

    auto Debugger::pause_execution() -> bool
    {
        if (!is_hooked.load())
        {
            return false;
        }
        should_pause.store(true);
        return true;
    }
    auto Debugger::continue_execution() -> void
    {
        if (is_paused.load())
        {
            step_mode.store(StepMode::None);
            resume_requested.store(true);
            pause_cv.notify_all();
        }
    }

    auto Debugger::step_into() -> void
    {
        if (is_paused.load())
        {
            step_mode.store(StepMode::Into);
            resume_requested.store(true);
            pause_cv.notify_all();
        }
    }

    auto Debugger::step_over() -> void
    {
        if (is_paused.load())
        {
            step_mode.store(StepMode::Over);
            resume_requested.store(true);
            pause_cv.notify_all();
        }
    }

    auto Debugger::step_out() -> void
    {
        if (is_paused.load())
        {
            step_mode.store(StepMode::Out);
            resume_requested.store(true);
            pause_cv.notify_all();
        }
    }

    auto Debugger::last_error() const -> std::string
    {
        std::lock_guard lock{m_error_mutex};
        return m_last_error;
    }
    auto Debugger::snapshot_json() const -> std::string
    {
        DebugJson result = debug_json_object();
        result["enabled"] = is_hooked.load();
        result["paused"] = is_paused.load();
        result["expression"] = "";
        result["functionFullName"] = "";
        result["bytecodeOffset"] = uint64_t{};
        result["contextFullName"] = "";
        result["callStack"] = DebugJsonArray{};

        std::lock_guard context_lock{context_mutex};
        if (!context)
        {
            return serialize_debug_json(result);
        }

        result["expression"] = expr_to_string(context->expr);
        if (context->context)
        {
            result["contextFullName"] = to_string(context->context->GetFullName());
        }
        if (context->stack && context->stack->Node())
        {
            auto* node = context->stack->Node();
            result["functionFullName"] = to_string(node->GetFullName());
            result["bytecodeOffset"] = static_cast<uint64_t>(context->stack->Code() - node->GetScript().GetData() - 1);
        }

        auto& call_stack = result["callStack"].get<DebugJsonArray>();
        for (auto* frame = context->stack; frame && frame->Node(); frame = frame->PreviousFrame())
        {
            DebugJson frame_json = debug_json_object();
            frame_json["functionFullName"] = to_string(frame->Node()->GetFullName());
            DebugJsonArray locals{};
            if (frame->Locals())
            {
                for (FProperty* property : TFieldRange<FProperty>(frame->Node(), EFieldIterationFlags::IncludeDeprecated))
                {
                    DebugJson local = debug_json_object();
                    local["name"] = to_string(property->GetName());
                    local["type"] = to_string(property->GetClass().GetName());
                    try
                    {
                        FString text{};
                        auto* value = property->ContainerPtrToValuePtr<void*>(frame->Locals());
                        property->ExportTextItem(text, value, value, frame->Object(), NULL);
                        local["value"] = to_string(*text);
                    }
                    catch (const std::exception& error)
                    {
                        local["valueError"] = error.what();
                    }
                    locals.emplace_back(std::move(local));
                }
            }
            frame_json["locals"] = std::move(locals);
            call_stack.emplace_back(std::move(frame_json));
        }
        return serialize_debug_json(result);
    }

    auto Debugger::breakpoints_json() const -> std::string
    {
        return m_breakpoints.breakpoints_json();
    }

    auto Debugger::add_breakpoint(const std::string& function_full_name, size_t bytecode_offset) -> bool
    {
        if (function_full_name.empty())
        {
            std::lock_guard lock{m_error_mutex};
            m_last_error = "Function full name is empty";
            return false;
        }
        m_breakpoints.add_breakpoint(ensure_str(function_full_name), bytecode_offset);
        return true;
    }

    auto Debugger::remove_breakpoint(const std::string& function_full_name, size_t bytecode_offset) -> bool
    {
        if (function_full_name.empty())
        {
            std::lock_guard lock{m_error_mutex};
            m_last_error = "Function full name is empty";
            return false;
        }
        m_breakpoints.remove_breakpoint(ensure_str(function_full_name), bytecode_offset);
        return true;
    }

    auto get_object_address(FProperty* property, EExprToken expr, auto in_context) -> void*
    {
        void* container{};
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(in_context)>, std::optional<PausedContext>>)
        {
            if (!in_context.has_value())
            {
                return nullptr;
            };
            const auto& context = in_context.value();
            if (expr == EX_InstanceVariable)
            {
                container = context.context;
            }
            else if (expr == EX_LocalVariable)
            {
                container = context.stack->Locals();
            }
            else
            {
                return nullptr;
            }
        }
        else
        {
            container = in_context;
        }
        return *property->ContainerPtrToValuePtr<void*>(container);
    }

    static auto try_rendering_property_context_menu(UFunction* fn, int index, FProperty* property, EExprToken expr, auto context) -> void
    {
        if (property->IsA<FObjectProperty>())
        {
            if (auto data_ptr = get_object_address(property, expr, context); data_ptr)
            {
                auto popup_context_name = to_string(fmt::format(STR("{}-{}-{}"), static_cast<void*>(fn), index, static_cast<void*>(property)));
                if (ImGui::BeginPopupContextItem(popup_context_name.c_str()))
                {
                    if (ImGui::MenuItem("Copy address"))
                    {
                        ImGui::SetClipboardText(fmt::format("{}", data_ptr).c_str());
                    }
                    ImGui::EndPopup();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("0x%p", data_ptr);
                    ImGui::EndTooltip();
                }
            }
        }
    }

    auto Debugger::render() -> void
    {
        std::scoped_lock lock(context_mutex);

        bool position_updated = context && m_last_code != context->stack->Code();
        if (position_updated) nav_to_function(context->stack->Node());

        UFunction* current_fn = nullptr;
        if (m_nav_history_index >= 0)
        {
            auto name = m_nav_history[m_nav_history_index];
            if (auto it = m_function_name_map.find(name); it != m_function_name_map.end()) current_fn = it->second;
        }

        float m_split_right = (ImGui::GetContentRegionMax().x - m_split_left);

        ImGui_Splitter(true, 4.0f, &m_split_left, &m_split_right, 200.0f, 200.0f, -24.0f, "KismetDebugger_split");

        ImGui::BeginChild("KismetDebugger_Controls", {m_split_left - 2, -24.0f}, true);

        if (ImGui::Button(is_hooked ? "disable" : "enable"))
        {
            if (is_hooked)
            {
                disable();
            }
            else
            {
                enable();
            }
        }
        if (is_hooked)
        {
            if (auto ctx = context)
            {
                if (ImGui::Button("continue"))
                {
                    continue_execution();
                }
                ImGui::SameLine();
                if (ImGui::Button("next"))
                {
                    step_into();
                }

                UFunction* node = ctx->stack->Node();
                size_t index = ctx->stack->Code() - node->GetScript().GetData() - 1;
                ImGui::Text("paused @ %s", expr_to_string(ctx->expr));
                ImGui::Text("index @ %zi", index);
                ImGui::Text("object context = %s", to_string(ctx->context->GetFullName()).c_str());

                if (ImGui::CollapsingHeader("Call stack", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    int lines = 0;
                    FFrame* current = ctx->stack;
                    while (current != nullptr)
                    {
                        current = current->PreviousFrame();
                        lines++;
                    }
                    ImGui::BeginChild("KismetDebugger_CallStack",
                                      {0, ImGui::GetStyle().ScrollbarSize + lines * ImGui::GetTextLineHeightWithSpacing()},
                                      false,
                                      ImGuiWindowFlags_HorizontalScrollbar);
                    current = ctx->stack;
                    for (int i = 0; current != nullptr; ++i)
                    {
                        ImGui::Text("%i: %s", i, to_string(current->Node()->GetFullName()).c_str());
                        current = current->PreviousFrame();
                    }
                    ImGui::EndChild();
                }
            }
            else
            {
                if (ImGui::Button("pause"))
                {
                    pause_execution();
                }
            }
        }

        if (current_fn)
        {
            if (ImGui::CollapsingHeader("Locals", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::BeginChild("KismetDebugger_Locals", {0, 0}, false, ImGuiWindowFlags_HorizontalScrollbar);
                if (context && context->stack->Node() == current_fn)
                {
                    UFunction* node = context->stack->Node();
                    int property_count{};
                    for (FProperty* property : TFieldRange<FProperty>(current_fn, EFieldIterationFlags::IncludeDeprecated))
                    {
                        FString text{};
                        auto container_ptr = property->ContainerPtrToValuePtr<void*>(context->stack->Locals());
                        property->ExportTextItem(text, container_ptr, container_ptr, static_cast<UObject*>(node), NULL);

                        ImGui::Text("%s = %S", to_string(property->GetName()).c_str(), *text);
                        try_rendering_property_context_menu(node, property_count, property, EX_Nothing, context->stack->Locals());
                        ++property_count;
                    }
                }
                else
                {
                    for (FProperty* property : TFieldRange<FProperty>(current_fn, EFieldIterationFlags::IncludeDeprecated))
                    {
                        ImGui::Text("%s", to_string(property->GetName()).c_str());
                    }
                }
                ImGui::EndChild();
            }
        }
        ImGui::EndChild();

        // left pane
        ImGui::SameLine(0, 8);

        ImGui::BeginGroup();

        render_nav_bar(m_split_right - 20);

        ImGui::BeginChild("KismetDebugger_Disassembly", {m_split_right - 20, -24.0f}, true);

        if (current_fn)
        {
            ScriptRenderContext render_ctx{context, current_fn, position_updated, m_breakpoints};
            render_ctx.render();
        }

        ImGui::EndChild();

        ImGui::EndGroup(); // left pane

        if (is_hooked)
        {
            if (auto ctx = context)
            {
                m_last_code = ctx->stack->Code();
            }
        }

        if (!is_hooked || !context) m_last_code = nullptr;
    }

    auto Debugger::render_nav_bar(float width) -> void
    {
        float start = ImGui::GetCursorPosX();

        bool disable_back = m_nav_history_index < 0;
        if (disable_back) ImGui::BeginDisabled();
        if (ImGui::Button("back"))
        {
            m_nav_history_index--;
            m_nav_function = m_nav_history_index < 0 ? "" : m_nav_history[m_nav_history_index];
        }
        if (disable_back) ImGui::EndDisabled();

        ImGui::SameLine();
        bool disable_forward = m_nav_history_index + 1 >= m_nav_history.size();
        if (disable_forward) ImGui::BeginDisabled();
        if (ImGui::Button("forward"))
        {
            m_nav_history_index++;
            m_nav_function = m_nav_history[m_nav_history_index];
        }
        if (disable_forward) ImGui::EndDisabled();

        // https://github.com/ocornut/imgui/issues/718#issuecomment-1249822993
        ImGui::SameLine();
        ImGui::SetNextItemWidth(width - (ImGui::GetCursorPosX() - start));
        const bool is_input_text_enter_pressed = ImGui::InputText("##input", &m_nav_function, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool is_input_text_active = ImGui::IsItemActive();
        const bool is_input_text_activated = ImGui::IsItemActivated();

        if (is_input_text_activated) ImGui::OpenPopup("##popup");

        {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y));
            // ImGui::SetNextWindowSize({ ImGui::GetItemRectSize().x, 0 });
            if (ImGui::BeginPopup("##popup", ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_ChildWindow))
            {
                int n = 0;

                UObjectGlobals::ForEachUObject([&](UObject* object, ...) {
                    if (object->IsA<UFunction>())
                    {
                        std::string full_name = to_string(object->GetFullName());

                        auto it = std::search(full_name.begin(), full_name.end(), m_nav_function.begin(), m_nav_function.end(), [](auto ch1, auto ch2) {
                            return std::toupper(ch1) == std::toupper(ch2);
                        });

                        if (it != full_name.end())
                        {
                            const char* name_c_str = full_name.c_str();

                            if (ImGui::Selectable(name_c_str))
                            {
                                ImGui::ClearActiveID();
                                m_function_name_map[full_name] = static_cast<UFunction*>(object);
                                m_nav_function = full_name;
                            }

                            if (n++ > 10)
                            {
                                ImGui::Text("...");
                                return LoopAction::Break;
                            }
                        }
                    }
                    return LoopAction::Continue;
                });

                if (!n) ImGui::Text("[nothing]");

                if (is_input_text_enter_pressed || (!is_input_text_active && !ImGui::IsWindowFocused()))
                {
                    ImGui::CloseCurrentPopup();
                    // TODO check valid

                    nav_to_function(m_nav_function);
                }

                ImGui::EndPopup();
            }
        }
    }

    auto Debugger::nav_to_function(UFunction* fn) -> void
    {
        std::string full_name = to_string(fn->GetFullName());

        m_function_name_map[full_name] = fn;

        nav_to_function(full_name);
    }

    // function name must already be in name map
    auto Debugger::nav_to_function(std::string full_name) -> void
    {
        if (m_nav_history_index >= 0 && m_nav_history[m_nav_history_index] == full_name) return; // already at fn, do not add another entry to the stack

        m_nav_function = full_name;

        m_nav_history_index++;
        if (m_nav_history_index >= m_nav_history.size())
            m_nav_history.push_back(m_nav_function);
        else
            m_nav_history[m_nav_history_index] = m_nav_function;
    }

    auto expr_to_string(EExprToken expr) -> const char*
    {
        switch (expr)
        {
        case EX_LocalVariable:
            return "EX_LocalVariable";
        case EX_InstanceVariable:
            return "EX_InstanceVariable";
        case EX_DefaultVariable:
            return "EX_DefaultVariable";
        case EX_Return:
            return "EX_Return";
        case EX_Jump:
            return "EX_Jump";
        case EX_JumpIfNot:
            return "EX_JumpIfNot";
        case EX_Assert:
            return "EX_Assert";
        case EX_Nothing:
            return "EX_Nothing";
        case EX_NothingInt32:
            return "EX_NothingInt32";
        case EX_Let:
            return "EX_Let";
        case EX_BitFieldConst:
            return "EX_BitFieldConst";
        case EX_ClassContext:
            return "EX_ClassContext";
        case EX_MetaCast:
            return "EX_MetaCast";
        case EX_LetBool:
            return "EX_LetBool";
        case EX_EndParmValue:
            return "EX_EndParmValue";
        case EX_EndFunctionParms:
            return "EX_EndFunctionParms";
        case EX_Self:
            return "EX_Self";
        case EX_Skip:
            return "EX_Skip";
        case EX_Context:
            return "EX_Context";
        case EX_Context_FailSilent:
            return "EX_Context_FailSilent";
        case EX_VirtualFunction:
            return "EX_VirtualFunction";
        case EX_FinalFunction:
            return "EX_FinalFunction";
        case EX_IntConst:
            return "EX_IntConst";
        case EX_FloatConst:
            return "EX_FloatConst";
        case EX_StringConst:
            return "EX_StringConst";
        case EX_ObjectConst:
            return "EX_ObjectConst";
        case EX_NameConst:
            return "EX_NameConst";
        case EX_RotationConst:
            return "EX_RotationConst";
        case EX_VectorConst:
            return "EX_VectorConst";
        case EX_Vector3fConst:
            return "EX_Vector3fConst";
        case EX_ByteConst:
            return "EX_ByteConst";
        case EX_IntZero:
            return "EX_IntZero";
        case EX_IntOne:
            return "EX_IntOne";
        case EX_True:
            return "EX_True";
        case EX_False:
            return "EX_False";
        case EX_TextConst:
            return "EX_TextConst";
        case EX_NoObject:
            return "EX_NoObject";
        case EX_TransformConst:
            return "EX_TransformConst";
        case EX_IntConstByte:
            return "EX_IntConstByte";
        case EX_NoInterface:
            return "EX_NoInterface";
        case EX_DynamicCast:
            return "EX_DynamicCast";
        case EX_StructConst:
            return "EX_StructConst";
        case EX_EndStructConst:
            return "EX_EndStructConst";
        case EX_SetArray:
            return "EX_SetArray";
        case EX_EndArray:
            return "EX_EndArray";
        case EX_PropertyConst:
            return "EX_PropertyConst";
        case EX_UnicodeStringConst:
            return "EX_UnicodeStringConst";
        case EX_Int64Const:
            return "EX_Int64Const";
        case EX_UInt64Const:
            return "EX_UInt64Const";
        case EX_DoubleConst:
            return "EX_DoubleConst";
        case EX_Cast: // EX_PrimitiveCast in 4.27
            return "EX_Cast";
        case EX_SetSet:
            return "EX_SetSet";
        case EX_EndSet:
            return "EX_EndSet";
        case EX_SetMap:
            return "EX_SetMap";
        case EX_EndMap:
            return "EX_EndMap";
        case EX_SetConst:
            return "EX_SetConst";
        case EX_EndSetConst:
            return "EX_EndSetConst";
        case EX_MapConst:
            return "EX_MapConst";
        case EX_EndMapConst:
            return "EX_EndMapConst";
        case EX_StructMemberContext:
            return "EX_StructMemberContext";
        case EX_LetMulticastDelegate:
            return "EX_LetMulticastDelegate";
        case EX_LetDelegate:
            return "EX_LetDelegate";
        case EX_LocalVirtualFunction:
            return "EX_LocalVirtualFunction";
        case EX_LocalFinalFunction:
            return "EX_LocalFinalFunction";
        case EX_LocalOutVariable:
            return "EX_LocalOutVariable";
        case EX_DeprecatedOp4A:
            return "EX_DeprecatedOp4A";
        case EX_InstanceDelegate:
            return "EX_InstanceDelegate";
        case EX_PushExecutionFlow:
            return "EX_PushExecutionFlow";
        case EX_PopExecutionFlow:
            return "EX_PopExecutionFlow";
        case EX_ComputedJump:
            return "EX_ComputedJump";
        case EX_PopExecutionFlowIfNot:
            return "EX_PopExecutionFlowIfNot";
        case EX_Breakpoint:
            return "EX_Breakpoint";
        case EX_InterfaceContext:
            return "EX_InterfaceContext";
        case EX_ObjToInterfaceCast:
            return "EX_ObjToInterfaceCast";
        case EX_EndOfScript:
            return "EX_EndOfScript";
        case EX_CrossInterfaceCast:
            return "EX_CrossInterfaceCast";
        case EX_InterfaceToObjCast:
            return "EX_InterfaceToObjCast";
        case EX_WireTracepoint:
            return "EX_WireTracepoint";
        case EX_SkipOffsetConst:
            return "EX_SkipOffsetConst";
        case EX_AddMulticastDelegate:
            return "EX_AddMulticastDelegate";
        case EX_ClearMulticastDelegate:
            return "EX_ClearMulticastDelegate";
        case EX_Tracepoint:
            return "EX_Tracepoint";
        case EX_LetObj:
            return "EX_LetObj";
        case EX_LetWeakObjPtr:
            return "EX_LetWeakObjPtr";
        case EX_BindDelegate:
            return "EX_BindDelegate";
        case EX_RemoveMulticastDelegate:
            return "EX_RemoveMulticastDelegate";
        case EX_CallMulticastDelegate:
            return "EX_CallMulticastDelegate";
        case EX_LetValueOnPersistentFrame:
            return "EX_LetValueOnPersistentFrame";
        case EX_ArrayConst:
            return "EX_ArrayConst";
        case EX_EndArrayConst:
            return "EX_EndArrayConst";
        case EX_SoftObjectConst:
            return "EX_SoftObjectConst";
        case EX_CallMath:
            return "EX_CallMath";
        case EX_SwitchValue:
            return "EX_SwitchValue";
        case EX_InstrumentationEvent:
            return "EX_InstrumentationEvent";
        case EX_ArrayGetByRef:
            return "EX_ArrayGetByRef";
        case EX_ClassSparseDataVariable:
            return "EX_ClassSparseDataVariable";
        case EX_FieldPathConst:
            return "EX_FieldPathConst";
        case EX_AutoRtfmTransact:
            return "EX_AutoRtfmTransact";
        case EX_AutoRtfmStopTransact:
            return "EX_AutoRtfmStopTransact";
        case EX_AutoRtfmAbortIfNot:
            return "EX_AutoRtfmAbortIfNot";
        case EX_Max:
            return "EX_Max";
        }
        return "unknown";
    };

    ScriptRenderContext::ScriptRenderContext(std::optional<PausedContext>& paused_context, UFunction* fn, bool scroll_to_active, BreakpointStore& breakpoints)
        : m_paused_context(paused_context), m_fn(fn), m_scroll_to_active(scroll_to_active), m_breakpoints(breakpoints),

          m_script(fn->GetScript().GetData()), m_script_size(fn->GetScript().Num()),
          m_cur((paused_context && paused_context->stack->Node() == fn) ? paused_context->stack->Code() - paused_context->stack->Node()->GetScript().GetData() - 1
                                                                        : -1)
    {
    }
    ScriptRenderContext::~ScriptRenderContext()
    {
    }

    auto ScriptRenderContext::render() -> void
    {
        while (m_index < m_script_size)
            render_expr();
    }

    auto ScriptRenderContext::render_property()
    {
        FProperty* property = (FProperty*)read_object();
        if (property)
        {
            ImGui::Text("%s", to_string(property->GetName()).c_str());
            if (m_paused_context.has_value())
            {
                try_rendering_property_context_menu(m_fn, m_index, property, m_current_expr, m_paused_context);
            }
            /*
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();

                // TODO: this depends on context and isn't always a local
                FString text{};
                auto container_ptr = property->ContainerPtrToValuePtr<void*>(stack->Locals());
                property->ExportTextItem(text, container_ptr, container_ptr, static_cast<UObject*>(stack->Node()), NULL);

                ImGui::Text("= %S", *text);

                ImGui::EndTooltip();
            }
            */
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
            ImGui::Text("NULL property");
            ImGui::PopStyleColor();
        }
    }
    auto ScriptRenderContext::render_expr() -> EExprToken
    {
        bool active = m_index == m_cur;
        size_t expr_index = m_index;

        m_current_expr = static_cast<EExprToken>(read<uint8>());

        // std::cout << "rendering (" << std::hex << unsigned(m_current_expr) << std::dec << ") @ " << (index - 1) << " " << expr_to_string(m_current_expr) << std::endl;

        ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x);
        auto label = std::format("{}", expr_index);
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(1.0f, 0.5f));
        bool is_breakpoint = m_breakpoints.has_breakpoint(m_fn, expr_index);
        if (ImGui::Selectable(label.c_str(), is_breakpoint, 0, {30, 0}))
        {
            if (is_breakpoint)
                m_breakpoints.remove_breakpoint(m_fn, expr_index);
            else
                m_breakpoints.add_breakpoint(m_fn, expr_index);
        }
        ImGui::PopStyleVar();
        ImGui::SameLine();
        ImGui::SetCursorPosX(50 + m_indent * 20.0f);

        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
            if (m_scroll_to_active) ImGui::SetScrollHereY();
        }
        ImGui::Text("%s", expr_to_string(m_current_expr));
        if (active) ImGui::PopStyleColor();

        m_indent++;
        // Note: we use the ImGui::Dummy() lines below to avoid a warning related to setting cursor pos without following with an imgui item
        ImGui::SetCursorPosX(50 + m_indent * 20.0f);

        switch (m_current_expr)
        {
        case EX_Cast: {
            ImGui::Dummy(ImVec2(0, 0));
            read<uint8>();
            render_expr();
            break;
        }
        case EX_ObjToInterfaceCast:
        case EX_CrossInterfaceCast:
        case EX_InterfaceToObjCast: {
            ImGui::Dummy(ImVec2(0, 0));
            (UClass*)read_object();
            render_expr();
            break;
        }
        case EX_Let: {
            render_property();
        }
        case EX_LetObj:
        case EX_LetWeakObjPtr:
        case EX_LetBool:
        case EX_LetDelegate:
        case EX_LetMulticastDelegate: {
            ImGui::Dummy(ImVec2(0, 0));
            render_expr();
            render_expr();
            break;
        }
        case EX_LetValueOnPersistentFrame: {
            render_property();
            render_expr();
            break;
        }
        case EX_StructMemberContext: {
            ImGui::Dummy(ImVec2(0, 0));
            (FProperty*)read_object(); // struct member expr.
            render_expr();
            break;
        }
        case EX_Jump: {
            ImGui::Dummy(ImVec2(0, 0));
            read<uint32>();
            break;
        }
        case EX_ComputedJump: {
            ImGui::Dummy(ImVec2(0, 0));
            render_expr();
            break;
        }
        case EX_LocalVariable:
        case EX_InstanceVariable:
        case EX_DefaultVariable:
        case EX_LocalOutVariable:
        case EX_ClassSparseDataVariable:
        case EX_PropertyConst: {
            this->render_property();
            break;
        }
        case EX_InterfaceContext: {
            ImGui::Dummy(ImVec2(0, 0));
            render_expr();
            break;
        }
        case EX_PushExecutionFlow: {
            ImGui::Dummy(ImVec2(0, 0));
            read<uint32>();
            break;
        }
        case EX_NothingInt32: {
            ImGui::Dummy(ImVec2(0, 0));
            read<int32>();
            break;
        }
        case EX_Nothing:
        case EX_EndOfScript:
        case EX_EndFunctionParms:
        case EX_EndStructConst:
        case EX_EndArray:
        case EX_EndArrayConst:
        case EX_EndSet:
        case EX_EndMap:
        case EX_EndSetConst:
        case EX_EndMapConst:
        case EX_IntZero:
        case EX_IntOne:
        case EX_True:
        case EX_False:
        case EX_NoObject:
        case EX_NoInterface:
        case EX_Self:
        case EX_EndParmValue:
        case EX_PopExecutionFlow:
        case EX_DeprecatedOp4A: {
            ImGui::Dummy(ImVec2(0, 0));
            break;
        }
        case EX_WireTracepoint:
        case EX_Tracepoint: {
            ImGui::Dummy(ImVec2(0, 0));
            break;
        }
        case EX_Breakpoint: {
            ImGui::Dummy(ImVec2(0, 0));
            break;
        }
        /*
        case EX_InstrumentationEvent: TODO ??????
        {
            if (Script[iCode] == EScriptInstrumentation::InlineEvent)
            {
                iCode += sizeof(FScriptName);
            }
            iCode += sizeof(uint8);
            break;
        }
        */
        case EX_Return: {
            ImGui::Dummy(ImVec2(0, 0));
            render_expr();
            break;
        }
        case EX_CallMath:
        case EX_LocalFinalFunction:
        case EX_FinalFunction: {
            ImGui::Dummy(ImVec2(0, 0));
            (UFunction*)read_object();
            while (render_expr() != EX_EndFunctionParms)
                ;
            break;
        }
        case EX_LocalVirtualFunction:
        case EX_VirtualFunction: {
            // std::cout << "reading " << sizeof(FName) << " bytes @ " << index << std::endl;

            /*
            for (int i = 0; i < script_size; ++i)
                std::cout << script[i] << " ";
            std::cout << std::endl;
            */

            /*
            for (int i = 0; i < script_size; ++i)
                std::cout << std::hex << std::setfill('0') << std::setw(2) << unsigned(script[i]) << " ";
            std::cout << std::endl;
            */

            ImGui::Dummy(ImVec2(0, 0));
            FName n = read_name();
            while (render_expr() != EX_EndFunctionParms)
                ; // Parms.
            break;
        }
        case EX_CallMulticastDelegate: {
            ImGui::Dummy(ImVec2(0, 0));
            (UFunction*)read_object();
            while (render_expr() != EX_EndFunctionParms)
                ; // Parms.
            break;
        }
        case EX_BitFieldConst: {
            ImGui::Dummy(ImVec2(0, 0));
            (FProperty*)read_object();
            read<uint8>();
            break;
        }
        case EX_ClassContext:
        case EX_Context:
        case EX_Context_FailSilent: {
            ImGui::Dummy(ImVec2(0, 0));
            render_expr(); // Object expression.
            read<uint32>();
            (FField*)read_object(); // Property corresponding to the r-value data, in case the l-value needs to be mem-zero'd
            render_expr();          // Context expression.
            break;
        }
        case EX_AddMulticastDelegate:
        case EX_RemoveMulticastDelegate: {
            ImGui::Dummy(ImVec2(0, 0));
            render_expr(); // Delegate property to assign to
            render_expr(); // Delegate to add to the MC delegate for broadcast
            break;
        }
        case EX_ClearMulticastDelegate: {
            ImGui::Dummy(ImVec2(0, 0));
            render_expr(); // Delegate property to clear
            break;
        }
        case EX_IntConst: {
            ImGui::Text("%d", read<int32>());
            break;
        }
        case EX_Int64Const: {
            ImGui::Text("%lld", read<int64_t>());
            break;
        }
        case EX_UInt64Const: {
            ImGui::Text("%llu", read<uint64_t>());
            break;
        }
        case EX_DoubleConst: {
            ImGui::Text("%f", read<double>());
            break;
        }
        case EX_SkipOffsetConst: {
            ImGui::Text("%u", read<uint32>()); // TODO jump
            break;
        }
        case EX_FloatConst: {
            ImGui::Text("%f", read<float>());
            break;
        }
        case EX_StringConst: {
            ImGui::Dummy(ImVec2(0, 0));
            while (read<uint8>())
                ;
            break;
        }
        case EX_UnicodeStringConst: {
            ImGui::Dummy(ImVec2(0, 0));
            while (read<uint16>())
                ;
            break;
        }
        case EX_TextConst: {
            ImGui::Dummy(ImVec2(0, 0));
            switch (read<uint8>())
            {
            case 0: // Empty
                break;
            case 1: // LocalizedText
                render_expr();
                render_expr();
                render_expr();
                break;
            case 2: // InvariantText
                render_expr();
                break;
            case 3: // LiteralString
                render_expr();
                break;
            case 4: // StringTableEntry
                read_object();
                render_expr();
                render_expr();
                break;
            }
            break;
        }
        case EX_ObjectConst: {
            ImGui::Dummy(ImVec2(0, 0));
            read_object();
            break;
        }
        case EX_SoftObjectConst: {
            ImGui::Dummy(ImVec2(0, 0));
            render_expr();
            break;
        }
        case EX_FieldPathConst: {
            ImGui::Dummy(ImVec2(0, 0));
            render_expr();
            break;
        }
        case EX_NameConst: {
            ImGui::Dummy(ImVec2(0, 0));
            FName n = read_name();
            break;
        }
        case EX_RotationConst: {
            ImGui::Dummy(ImVec2(0, 0));
            read<int32>();
            read<int32>();
            read<int32>();
            break;
        }
        case EX_Vector3fConst:
        case EX_VectorConst: {
            ImGui::Dummy(ImVec2(0, 0));
            read<float>();
            read<float>();
            read<float>();
            break;
        }
        case EX_TransformConst: {
            ImGui::Dummy(ImVec2(0, 0));
            // Rotation
            read<float>();
            read<float>();
            read<float>();
            read<float>();
            // Translation
            read<float>();
            read<float>();
            read<float>();
            // Scale
            read<float>();
            read<float>();
            read<float>();
            break;
        }
        case EX_StructConst: {
            ImGui::Dummy(ImVec2(0, 0));
            (UScriptStruct*)read_object(); // Struct.
            read<int32>();
            while (render_expr() != EX_EndStructConst)
                ;
            break;
        }
        case EX_SetArray: {
            ImGui::Dummy(ImVec2(0, 0));
            // If not loading, or its a newer version
            // if((!GetLinker()) || !Ar.IsLoading() || (Ar.UE4Ver() >= VER_UE4_CHANGE_SETARRAY_BYTECODE))
            //{
            // Array property to assign to
            EExprToken TargetToken = render_expr();
            //}
            // else
            //{
            // Array Inner Prop
            //(FProperty*)read_object();
            //}

            while (render_expr() != EX_EndArray)
                ;
            break;
        }
        case EX_SetSet: {
            ImGui::Dummy(ImVec2(0, 0));
            render_expr(); // set property
            read<int32>();
            while (render_expr() != EX_EndSet)
                ;
            break;
        }
        case EX_SetMap: {
            ImGui::Dummy(ImVec2(0, 0));
            render_expr(); // map property
            read<int32>();
            while (render_expr() != EX_EndMap)
                ;
            break;
        }
        case EX_ArrayConst: {
            ImGui::Dummy(ImVec2(0, 0));
            (FProperty*)read_object(); // Inner property
            read<int32>();
            while (render_expr() != EX_EndArrayConst)
                ;
            break;
        }
        case EX_SetConst: {
            ImGui::Dummy(ImVec2(0, 0));
            (FProperty*)read_object(); // Inner property
            read<int32>();
            while (render_expr() != EX_EndSetConst)
                ;
            break;
        }
        case EX_MapConst: {
            ImGui::Dummy(ImVec2(0, 0));
            (FProperty*)read_object(); // Key property
            (FProperty*)read_object(); // Val property
            read<int32>();
            while (render_expr() != EX_EndMapConst)
                ;
            break;
        }
        case EX_ByteConst:
        case EX_IntConstByte: {
            ImGui::Dummy(ImVec2(0, 0));
            read<int8>();
            break;
        }
        case EX_MetaCast: {
            ImGui::Dummy(ImVec2(0, 0));
            (UClass*)read_object();
            render_expr();
            break;
        }
        case EX_DynamicCast: {
            ImGui::Dummy(ImVec2(0, 0));
            (UClass*)read_object();
            render_expr();
            break;
        }
        case EX_JumpIfNot: {
            ImGui::Dummy(ImVec2(0, 0));
            read<uint32>();
            render_expr(); // Boolean expr.
            break;
        }
        case EX_PopExecutionFlowIfNot: {
            ImGui::Dummy(ImVec2(0, 0));
            render_expr(); // Boolean expr.
            break;
        }
        case EX_Assert: {
            ImGui::Dummy(ImVec2(0, 0));
            read<uint16>();
            read<uint8>();
            render_expr(); // Assert expr.
            break;
        }
        case EX_Skip: {
            ImGui::Dummy(ImVec2(0, 0));
            read<uint32>();
            render_expr(); // Expression to possibly skip.
            break;
        }
        case EX_InstanceDelegate: {
            ImGui::Dummy(ImVec2(0, 0));
            FName n = read_name();
            break;
        }
        case EX_BindDelegate: {
            ImGui::Dummy(ImVec2(0, 0));
            FName n = read_name();
            render_expr(); // Delegate property to assign to
            render_expr();
            break;
        }
        case EX_SwitchValue: {
            ImGui::Dummy(ImVec2(0, 0));
            auto cases = read<uint16>(); // number of cases, without default one
            auto end = read<uint32>();   // Code offset, go to it, when done.
            render_expr();               // index term

            for (uint16 i = 0; i < cases; ++i)
            {
                render_expr();                   // case index value term
                auto next_case = read<uint32>(); // offset to the next case
                render_expr();                   // case term
            }

            render_expr(); // default term
            break;
        }
        case EX_ArrayGetByRef: {
            ImGui::Dummy(ImVec2(0, 0));
            render_expr();
            render_expr();
            break;
        }
        case EX_AutoRtfmTransact: {
            ImGui::Dummy(ImVec2(0, 0));
            read<int32>();
            read<uint32>();
            while (render_expr() != EX_AutoRtfmStopTransact)
                ;
            break;
        }
        case EX_AutoRtfmStopTransact: {
            ImGui::Dummy(ImVec2(0, 0));
            read<int32>();
            read<uint8>();
            break;
        }
        case EX_AutoRtfmAbortIfNot: {
            ImGui::Dummy(ImVec2(0, 0));
            render_expr();
            break;
        }
        default: {
            // This should never occur.
            // UE_LOG(LogScriptSerialization, Warning, TEXT("Error: Unknown bytecode 0x%02X; ignoring it"), (uint8)Expr );
            std::cout << "unknown expr (" << unsigned(m_current_expr) << ") " << expr_to_string(m_current_expr) << std::endl;
            ImGui::Text("unknown expr (%i) %s", unsigned(m_current_expr), expr_to_string(m_current_expr));
            break;
        }
        }
        m_indent--;
        return m_current_expr;
    }

} // namespace RC::GUI::KismetDebuggerMod
