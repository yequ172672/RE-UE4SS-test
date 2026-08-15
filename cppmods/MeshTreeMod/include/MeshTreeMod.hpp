// MeshTreeMod: UE4SS mod that draws an ImGui component tree for the local
// player's pawn and lets you toggle per-component visibility.
//
// Threading: the scan and the visibility writes run on the game thread
// (UEngine::Tick pre-callback); the overlay renders on the game render
// thread (swapchain Present) and only reads a mutex-protected snapshot.
//
// The overlay is a standalone ImGui context rendered directly over the game
// window (MeshOverlay), independent of the UE4SS debug GUI. F9 toggles it.

#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <Mod/CppUserModBase.hpp>

namespace RC::MeshTreeMod
{
    struct ComponentEntry
    {
        std::string name;
        std::string class_name;
        int32_t parent_index{-1};
        bool hidden{false};
    };

    class MeshTreeMod : public CppUserModBase
    {
      public:
        MeshTreeMod();
        ~MeshTreeMod() override;

        auto on_unreal_init() -> void override;
        auto on_program_start() -> void override;

        auto request_toggle(int32_t component_index, bool hide) -> void;
        auto refresh_now() -> void;
        auto snapshot(std::vector<ComponentEntry>& out_components, std::string& out_pawn_name) const -> void;

      private:
        auto render_window_contents() -> void;
        auto render_component_node(int32_t index, const std::vector<ComponentEntry>& components, std::vector<bool>& expanded) -> void;
        auto scan_on_game_thread() -> void;
        auto consume_pending_toggles_on_game_thread() -> void;

        mutable std::mutex m_mutex;
        std::vector<ComponentEntry> m_components;
        std::string m_pawn_name;
        std::vector<std::pair<int32_t, bool>> m_pending_toggles;
        std::chrono::steady_clock::time_point m_last_scan{};
        std::atomic<bool> m_unreal_ready{false};
    };
} // namespace RC::MeshTreeMod
