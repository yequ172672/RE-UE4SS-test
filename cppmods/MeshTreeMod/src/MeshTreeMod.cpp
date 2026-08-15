// MeshTreeMod: UE4SS mod entry point and ImGui tab wiring.
//
// The mod scans the local player pawn's components on the game thread
// (UEngine::Tick pre-callback) and renders a component tree with per-row
// hide/show buttons in a UE4SS ImGui tab.

#include <MeshTreeMod.hpp>

#include <imgui.h>

#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UEngine.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UActorComponent.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <UE4SSProgram.hpp>
#include <Helpers/String.hpp>

namespace RC::MeshTreeMod
{
    MeshTreeMod::MeshTreeMod()
    {
        ModName = STR("MeshTreeMod");
        ModAuthors = STR("local");
        ModDescription = STR("Shows the local pawn's component tree with per-component visibility toggles.");
        ModVersion = STR("1.0.0");

        register_tab(STR("MeshTree"), [](CppUserModBase* mod) {
            UE4SS_ENABLE_IMGUI();
            static_cast<MeshTreeMod*>(mod)->render_window_contents();
        });
    }

    MeshTreeMod::~MeshTreeMod() = default;

    auto MeshTreeMod::on_unreal_init() -> void
    {
        Unreal::Hook::RegisterEngineTickPreCallback(
                [this](auto&, Unreal::UEngine*, float, bool) {
                    scan_on_game_thread();
                    consume_pending_toggles_on_game_thread();
                },
                {true, true, STR("MeshTreeMod"), STR("GameThreadScan")});
        m_unreal_ready.store(true);
    }

    auto MeshTreeMod::on_program_start() -> void
    {
    }

    auto MeshTreeMod::request_toggle(int32_t component_index, bool hide) -> void
    {
        std::lock_guard lock{m_mutex};
        m_pending_toggles.emplace_back(component_index, hide);
    }

    auto MeshTreeMod::refresh_now() -> void
    {
        // The scan runs on the game thread; just clear the throttle so the
        // next UEngine::Tick callback rescans immediately.
        m_last_scan = std::chrono::steady_clock::time_point{};
    }

    auto MeshTreeMod::snapshot(std::vector<ComponentEntry>& out_components, std::string& out_pawn_name) const -> void
    {
        std::lock_guard lock{m_mutex};
        out_components = m_components;
        out_pawn_name = m_pawn_name;
    }

    auto MeshTreeMod::scan_on_game_thread() -> void
    {
        if (!m_unreal_ready.load())
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - m_last_scan < std::chrono::milliseconds(500))
        {
            return;
        }
        m_last_scan = now;

        // Local player pawn: PlayerController::Pawn (reflected property; avoids
        // vtable calls, which some games break).
        auto* pc = Unreal::UObjectGlobals::FindFirstOf(std::string_view{"PlayerController"});
        if (!pc)
        {
            return;
        }
        auto* pawn_property = pc->GetPropertyByNameInChain(STR("Pawn"));
        auto* pawn_object_property = pawn_property ? CastField<Unreal::FObjectProperty>(pawn_property) : nullptr;
        Unreal::UObject* pawn = nullptr;
        if (pawn_object_property)
        {
            if (const auto* value_ptr = pawn_object_property->GetPropertyValuePtrInContainer(pc))
            {
                pawn = const_cast<Unreal::UObject*>(*value_ptr);
            }
        }
        if (!pawn)
        {
            std::lock_guard lock{m_mutex};
            m_components.clear();
            m_pawn_name.clear();
            return;
        }

        auto* outer_property = pc->GetPropertyByNameInChain(STR("Outer"));
        auto* outer_object_property = outer_property ? CastField<Unreal::FObjectProperty>(outer_property) : nullptr;

        struct RawComponent
        {
            Unreal::UObject* object{};
            Unreal::UObject* attach_parent{};
        };
        std::vector<RawComponent> raw_components;

        Unreal::UObjectGlobals::ForEachUObject([&](Unreal::UObject* object, int32_t, int32_t) {
            if (!object->IsA<Unreal::UActorComponent>())
            {
                return LoopAction::Continue;
            }
            // UObject::Outer is not a reflected property in UE; use the native member.
            if (object->GetOuterPrivate() != pawn)
            {
                return LoopAction::Continue;
            }

            RawComponent raw{};
            raw.object = object;
            if (object->IsA<Unreal::USceneComponent>())
            {
                if (auto* attach_property = object->GetPropertyByNameInChain(STR("AttachParent")))
                {
                    if (auto* attach_object_property = CastField<Unreal::FObjectProperty>(attach_property))
                    {
                        if (const auto* value_ptr = attach_object_property->GetPropertyValuePtrInContainer(object))
                        {
                            raw.attach_parent = const_cast<Unreal::UObject*>(*value_ptr);
                        }
                    }
                }
            }
            raw_components.emplace_back(raw);
            return LoopAction::Continue;
        });

        std::lock_guard lock{m_mutex};
        m_components.clear();
        m_components.reserve(raw_components.size());
        for (const auto& raw : raw_components)
        {
            ComponentEntry entry{};
            entry.name = to_string(raw.object->GetName());
            entry.class_name = to_string(raw.object->GetClassPrivate()->GetName());
            if (raw.attach_parent)
            {
                for (size_t i = 0; i < raw_components.size(); ++i)
                {
                    if (raw_components[i].object == raw.attach_parent)
                    {
                        entry.parent_index = static_cast<int32_t>(i);
                        break;
                    }
                }
            }
            if (auto* hidden_property = raw.object->GetPropertyByNameInChain(STR("bHiddenInGame")))
            {
                if (auto* bool_property = CastField<Unreal::FBoolProperty>(hidden_property))
                {
                    entry.hidden = bool_property->GetPropertyValueInContainer(raw.object);
                }
            }
            m_components.emplace_back(std::move(entry));
        }
        m_pawn_name = to_string(pawn->GetName());
    }

    auto MeshTreeMod::consume_pending_toggles_on_game_thread() -> void
    {
        std::vector<std::pair<int32_t, bool>> pending;
        {
            std::lock_guard lock{m_mutex};
            pending.swap(m_pending_toggles);
        }
        if (pending.empty())
        {
            return;
        }

        // Resolve the pawn again so the toggle applies to the live component.
        auto* pc = Unreal::UObjectGlobals::FindFirstOf(std::string_view{"PlayerController"});
        if (!pc)
        {
            return;
        }
        auto* pawn_property = pc->GetPropertyByNameInChain(STR("Pawn"));
        auto* pawn_object_property = pawn_property ? CastField<Unreal::FObjectProperty>(pawn_property) : nullptr;
        Unreal::UObject* pawn = nullptr;
        if (pawn_object_property)
        {
            if (const auto* value_ptr = pawn_object_property->GetPropertyValuePtrInContainer(pc))
            {
                pawn = const_cast<Unreal::UObject*>(*value_ptr);
            }
        }
        if (!pawn)
        {
            return;
        }

        auto* outer_property = pc->GetPropertyByNameInChain(STR("Outer"));
        auto* outer_object_property = outer_property ? CastField<Unreal::FObjectProperty>(outer_property) : nullptr;

        std::vector<Unreal::UObject*> components;
        Unreal::UObjectGlobals::ForEachUObject([&](Unreal::UObject* object, int32_t, int32_t) {
            if (!object->IsA<Unreal::UActorComponent>())
            {
                return LoopAction::Continue;
            }
            if (object->GetOuterPrivate() == pawn)
            {
                components.emplace_back(object);
            }
            return LoopAction::Continue;
        });

        for (const auto& [component_index, hide] : pending)
        {
            if (component_index < 0 || static_cast<size_t>(component_index) >= components.size())
            {
                continue;
            }
            auto* component = components[static_cast<size_t>(component_index)];
            if (auto* hidden_property = component->GetPropertyByNameInChain(STR("bHiddenInGame")))
            {
                if (auto* bool_property = CastField<Unreal::FBoolProperty>(hidden_property))
                {
                    bool_property->SetPropertyValueInContainer(component, hide);
                }
            }
        }
    }

    auto MeshTreeMod::render_window_contents() -> void
    {
        if (!m_unreal_ready.load())
        {
            ImGui::Text("Waiting for Unreal initialization...");
            return;
        }

        std::vector<ComponentEntry> components;
        std::string pawn_name;
        snapshot(components, pawn_name);

        ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("MeshTree", nullptr, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        if (ImGui::Button("Refresh"))
        {
            refresh_now();
        }
        ImGui::SameLine();
        ImGui::Text("Pawn: %s (%zu components)", pawn_name.c_str(), components.size());

        ImGui::Separator();

        if (components.empty())
        {
            ImGui::TextDisabled("No pawn or no components found.");
            ImGui::End();
            return;
        }

        std::vector<bool> expanded(components.size(), false);
        for (size_t i = 0; i < components.size(); ++i)
        {
            if (components[i].parent_index == -1)
            {
                render_component_node(static_cast<int32_t>(i), components, expanded);
            }
        }

        ImGui::End();
    }

    auto MeshTreeMod::render_component_node(int32_t index, const std::vector<ComponentEntry>& components, std::vector<bool>& expanded) -> void
    {
        const auto& entry = components[static_cast<size_t>(index)];

        ImGui::PushID(index);

        const bool has_children = [&]() {
            for (const auto& other : components)
            {
                if (other.parent_index == index)
                {
                    return true;
                }
            }
            return false;
        }();

        ImGui::AlignTextToFramePadding();
        bool node_open = false;
        if (has_children)
        {
            node_open = ImGui::TreeNodeEx("##node", ImGuiTreeNodeFlags_SpanAvailWidth);
        }
        else
        {
            ImGui::TreeNodeEx("##node", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
        }

        ImGui::SameLine();
        ImGui::TextDisabled("%s", entry.class_name.c_str());
        ImGui::SameLine();
        ImGui::Text("%s", entry.name.c_str());

        ImGui::SameLine();
        const bool hidden = entry.hidden;
        const char* label = hidden ? "Show" : "Hide";
        if (ImGui::SmallButton(label))
        {
            request_toggle(index, !hidden);
        }

        if (has_children && node_open)
        {
            for (size_t i = 0; i < components.size(); ++i)
            {
                if (components[i].parent_index == index)
                {
                    render_component_node(static_cast<int32_t>(i), components, expanded);
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
} // namespace RC::MeshTreeMod
