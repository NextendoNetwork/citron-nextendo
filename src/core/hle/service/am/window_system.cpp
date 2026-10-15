// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging.h"
#include "core/core.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/am_types.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/event_observer.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/am/process_creation.h"
#include "core/hle/service/am/service/library_applet_creator.h"
#include "core/hle/service/am/window_system.h"

namespace Service::AM {

WindowSystem::WindowSystem(Core::System& system) : m_system(system) {}

WindowSystem::~WindowSystem() {
    m_system.GetAppletManager().SetWindowSystem(nullptr);
}

void WindowSystem::SetEventObserver(EventObserver* observer) {
    m_event_observer = observer;
    m_system.GetAppletManager().SetWindowSystem(this);
}

void WindowSystem::RequestUpdate() {
    if (m_event_observer) {
        m_event_observer->RequestUpdate();
    }
}

bool WindowSystem::RequestInvitationLaunch(u64 program_id, std::vector<u8> storage) {
    {
        std::scoped_lock lk{m_lock};
        if (!m_home_menu || m_application || m_invitation_launch ||
            storage.size() < 0x10 || storage.size() > 0x410) {
            return false;
        }
        m_invitation_launch = InvitationLaunchRequest{program_id, std::move(storage)};
        std::scoped_lock applet_lk{m_home_menu->lock};
        m_home_menu->lifecycle_manager.PushUnorderedMessage(AppletMessage::LaunchApplicationRequested);
        m_foreground_requested_applet = m_home_menu;
    }
    RequestUpdate();
    return true;
}

std::optional<WindowSystem::InvitationLaunchRequest> WindowSystem::TakeInvitationLaunch() {
    std::scoped_lock lk{m_lock};
    auto request = std::move(m_invitation_launch);
    m_invitation_launch.reset();
    return request;
}

WindowSystem::DisplayParams WindowSystem::ComputeOverlayDisplayParams(Applet& applet) const {
    DisplayParams dp{};
    dp.visible = applet.window_visible;
    dp.interactible = applet.overlay_in_foreground;
    dp.z_index = applet.overlay_in_foreground ? 100000 : -1;
    return dp;
}

WindowSystem::DisplayParams WindowSystem::ComputeStandardDisplayParams(
    Applet& applet, bool is_foreground, bool input_intercepted, bool is_obscured) const {
    DisplayParams dp{};

    const bool active = is_foreground;

    dp.visible = active && applet.window_visible;
    dp.interactible = active && applet.window_visible && !input_intercepted;

    // An applet obscured by one of its own foreground children must drop below it, or the
    // two end up tied and the compositor can pick either one to show on top.
    if (active && !is_obscured) {
        dp.z_index = 2;
    } else if (active) {
        dp.z_index = 1;
    }

    return dp;
}

void WindowSystem::ApplyDisplayParams(Applet& applet, const DisplayParams& params) {
    applet.display_layer_manager.SetWindowVisibility(params.visible);
    applet.SetInteractibleLocked(params.interactible);
    applet.display_layer_manager.SetOverlayZIndex(params.z_index);
}

bool WindowSystem::RequestHomebrewMenu() {
    {
        std::scoped_lock lk{m_lock};
        if (!m_home_menu || m_foreground_requested_applet != m_home_menu) return false;
        if (!m_home_menu->child_applets.empty()) {
            // The same controller shortcut opens hbmenu and returns from it.
            if (!m_home_menu->child_applets.front()->is_homebrew_applet) return false;
            m_homebrew_return_requested = true;
        } else if (m_homebrew_requested.exchange(true)) {
            return false;
        }
    }
    RequestUpdate();
    return true;
}

void WindowSystem::Update() {
    if (m_homebrew_requested.exchange(false)) {
        std::shared_ptr<Applet> home;
        {
            std::scoped_lock lk{m_lock};
            if (m_home_menu && m_foreground_requested_applet == m_home_menu &&
                m_home_menu->child_applets.empty()) home = m_applets.at(m_home_menu->aruid.pid);
        }
        if (home && !LaunchHomebrewMenu(m_system, *this, home)) {
            LOG_ERROR(Service_AM, "Homebrew Menu shortcut failed to initialize hbloader");
        }
    }
    std::scoped_lock lk{m_lock};

    if (m_homebrew_return_requested) {
        m_homebrew_return_requested = false;
        if (m_home_menu) {
            // Run termination on the AM observer, never the input/Qt thread.
            for (const auto& child : m_home_menu->child_applets) {
                if (!child->is_homebrew_applet) continue;
                std::scoped_lock child_lock{child->lock};
                child->process->Terminate(true);
                LOG_INFO(Service_AM, "Homebrew return requested: pid={}", child->aruid.pid);
            }
            m_foreground_requested_applet = m_home_menu;
        }
    }
    this->PruneTerminatedAppletsLocked();

    if (this->LockHomeMenuIntoForegroundLocked()) {
        return;
    }

    // Determine whether the overlay is capturing input focus.
    bool overlay_captures_input = false;
    if (m_overlay) {
        std::scoped_lock lk_ov{m_overlay->lock};
        overlay_captures_input = m_overlay->overlay_in_foreground;

        auto dp = ComputeOverlayDisplayParams(*m_overlay);
        ApplyDisplayParams(*m_overlay, dp);

        // Overlay lifecycle: always treat as foreground-visible when window is shown.
        const auto desired = m_overlay->window_visible ? ActivityState::ForegroundVisible
                                                       : ActivityState::BackgroundVisible;
        if (m_overlay->lifecycle_manager.GetActivityState() != desired) {
            m_overlay->lifecycle_manager.SetActivityState(desired);
            m_overlay->UpdateSuspensionStateLocked(true);
        }
    }

    this->ReconcileAppletTreeLocked(m_home_menu,
                                    m_foreground_requested_applet == m_home_menu,
                                    overlay_captures_input);
    this->ReconcileAppletTreeLocked(m_application,
                                    m_foreground_requested_applet == m_application,
                                    overlay_captures_input);
}

void WindowSystem::TrackApplet(std::shared_ptr<Applet> applet, bool is_application) {
    std::scoped_lock lk{m_lock};

    if (applet->applet_id == AppletId::QLaunch) {
        ASSERT(m_home_menu == nullptr);
        m_home_menu = applet.get();
    } else if (applet->applet_id == AppletId::OverlayDisplay) {
        m_overlay = applet.get();
    } else if (is_application) {
        ASSERT(m_application == nullptr);
        m_application = applet.get();
    }

    m_main_applet_aruid.store(m_application ? m_application->aruid.pid
                                           : m_home_menu ? m_home_menu->aruid.pid : 0,
                             std::memory_order_relaxed);
    m_event_observer->TrackAppletProcess(applet);
    m_applets.emplace(applet->aruid.pid, std::move(applet));
}

std::shared_ptr<Applet> WindowSystem::GetByAppletResourceUserId(u64 aruid) {
    std::scoped_lock lk{m_lock};

    const auto it = m_applets.find(aruid);
    if (it == m_applets.end()) {
        return nullptr;
    }

    return it->second;
}

std::shared_ptr<Applet> WindowSystem::GetMainApplet() {
    std::scoped_lock lk{m_lock};

    if (m_application) {
        return m_applets.at(m_application->aruid.pid);
    }

    if (m_home_menu) {
        return m_applets.at(m_home_menu->aruid.pid);
    }

    return nullptr;
}

std::optional<WindowSystem::PresenceSnapshot> WindowSystem::TryGetPresenceSnapshot() {
    std::unique_lock window_lock{m_lock, std::try_to_lock};
    if (!window_lock.owns_lock()) {
        return std::nullopt;
    }
    if (!m_application) {
        return PresenceSnapshot{};
    }
    std::unique_lock applet_lock{m_application->lock, std::try_to_lock};
    if (!applet_lock.owns_lock()) {
        return std::nullopt;
    }
    return PresenceSnapshot{m_application->program_id,
                            m_foreground_requested_applet != m_application ||
                                !m_application->window_visible};
}

void WindowSystem::RequestHomeMenuToGetForeground() {
    {
        std::scoped_lock lk{m_lock};
        m_foreground_requested_applet = m_home_menu;
    }

    m_event_observer->RequestUpdate();
}

void WindowSystem::RequestApplicationToGetForeground() {
    {
        std::scoped_lock lk{m_lock};
        m_foreground_requested_applet = m_application;
    }

    m_event_observer->RequestUpdate();
}

void WindowSystem::RequestLockHomeMenuIntoForeground() {
    {
        std::scoped_lock lk{m_lock};
        m_home_menu_foreground_locked = true;
    }

    m_event_observer->RequestUpdate();
}

void WindowSystem::RequestUnlockHomeMenuIntoForeground() {
    {
        std::scoped_lock lk{m_lock};
        m_home_menu_foreground_locked = false;
    }

    m_event_observer->RequestUpdate();
}

void WindowSystem::RequestAppletVisibilityState(Applet& applet, bool visible) {
    {
        std::scoped_lock lk{applet.lock};
        applet.window_visible = visible;
    }

    m_event_observer->RequestUpdate();
}

void WindowSystem::OnOperationModeChanged() {
    std::scoped_lock lk{m_lock};

    for (const auto& [aruid, applet] : m_applets) {
        std::scoped_lock lk2{applet->lock};
        applet->lifecycle_manager.OnOperationAndPerformanceModeChanged();
    }
}

void WindowSystem::OnExitRequested() {
    std::scoped_lock lk{m_lock};

    for (const auto& [aruid, applet] : m_applets) {
        std::scoped_lock lk2{applet->lock};
        applet->lifecycle_manager.RequestExit();
    }
}

void WindowSystem::BroadcastButtonMessage(AppletMessage message) {
    Applet* targets[] = {m_home_menu, m_overlay, m_application};
    for (auto* target : targets) {
        if (target) {
            std::scoped_lock lk{target->lock};
            target->lifecycle_manager.PushUnorderedMessage(message);
        }
    }

    if (m_event_observer) {
        m_event_observer->RequestUpdate();
    }
}

void WindowSystem::OnHomeButtonPressed(ButtonPressDuration type) {
    std::scoped_lock lk{m_lock};

    // Without a home menu, delegate to the frontend to create one.
    if (!m_home_menu) {
        if (m_home_menu_request_callback && type == ButtonPressDuration::ShortPressing) {
            m_home_menu_request_callback();
        }
        return;
    }

    switch (type) {
    case ButtonPressDuration::ShortPressing:
        // qlaunch cannot dismiss a frontend-created accessor it never received.
        // Finish the override first; broadcasting HOME alone leaves its child on top.
        if (!m_home_menu->child_applets.empty() &&
            m_home_menu->child_applets.front()->is_homebrew_applet) {
            m_homebrew_return_requested = true;
            RequestUpdate();
            break;
        }
        BroadcastButtonMessage(AppletMessage::DetectShortPressingHomeButton);
        break;
    case ButtonPressDuration::MiddlePressing:
    case ButtonPressDuration::LongPressing:
        if (m_overlay) {
            std::scoped_lock lk_ov{m_overlay->lock};
            m_overlay->overlay_in_foreground = !m_overlay->overlay_in_foreground;
        }
        BroadcastButtonMessage(AppletMessage::DetectLongPressingHomeButton);
        break;
    }
}

void WindowSystem::OnCaptureButtonPressed(ButtonPressDuration type) {
    std::scoped_lock lk{m_lock};

    switch (type) {
    case ButtonPressDuration::ShortPressing:
        BroadcastButtonMessage(AppletMessage::DetectShortPressingCaptureButton);
        break;
    case ButtonPressDuration::LongPressing:
        BroadcastButtonMessage(AppletMessage::DetectShortPressingCaptureButton);
        break;
    default:
        break;
    }
}

void WindowSystem::PruneTerminatedAppletsLocked() {
    for (auto it = m_applets.begin(); it != m_applets.end(); /* ... */) {
        const auto& [aruid, applet] = *it;

        std::scoped_lock lk{applet->lock};

        // Frontend applets have no kernel process to reach the Terminated state.
        const bool frontend_finished = !applet->process->IsInitialized() && applet->is_completed;
        if (!applet->process->IsTerminated() && !frontend_finished) {
            it = std::next(it);
            continue;
        }

        // A winding applet has had its own process killed but is kept alive as a transparent slot
        // while the reserved applet running in its place finishes (see WindAndDoReserved()).
        if (applet->is_winding) {
            if (!applet->child_applets.empty()) {
                it = std::next(it);
                continue;
            }

            const bool unwind = applet->unwind_after_reserved;
            applet->is_winding = false;
            applet->unwind_after_reserved = false;

            if (unwind && this->RestartAppletProcessLocked(applet)) {
                const u64 new_aruid = applet->aruid.pid;
                const auto next = std::next(it);

                if (new_aruid != aruid) {
                    auto node = m_applets.extract(it);
                    node.key() = new_aruid;
                    m_applets.insert(std::move(node));
                }

                applet->process->Run();
                m_event_observer->RequestUpdate();
                it = next;
                continue;
            }

            applet->reserved_applet.reset();
        }

        if (!applet->child_applets.empty()) {
            this->TerminateChildAppletsLocked(applet.get());
            it = std::next(it);
            continue;
        }

        if (auto caller_applet = applet->caller_applet.lock(); caller_applet) {
            std::scoped_lock lk2{caller_applet->lock};
            std::erase(caller_applet->child_applets, applet);
            applet->caller_applet.reset();
        }

        if (applet.get() == m_foreground_requested_applet) {
            m_foreground_requested_applet = nullptr;
        }

        if (applet.get() == m_home_menu) {
            m_home_menu = nullptr;
            m_foreground_requested_applet = m_application;
        }

        if (applet.get() == m_application) {
            m_application = nullptr;
            m_foreground_requested_applet = m_home_menu;

            if (m_home_menu) {
                m_home_menu->lifecycle_manager.PushUnorderedMessage(
                    AppletMessage::ApplicationExited);
            }
        }

        if (applet.get() == m_overlay) {
            m_overlay = nullptr;
        }

        m_main_applet_aruid.store(m_application ? m_application->aruid.pid
                                               : m_home_menu ? m_home_menu->aruid.pid : 0,
                                 std::memory_order_relaxed);
        applet->OnProcessTerminatedLocked();
        LOG_INFO(Service_AM, "Applet return completed: applet_id={} pid={} caller_cleared={}",
                 static_cast<u32>(applet->applet_id), aruid, applet->caller_applet.expired());

        m_event_observer->RequestUpdate();

        it = m_applets.erase(it);
    }

    if (m_applets.empty()) {
        m_system.Exit();
    }
}

bool WindowSystem::RestartAppletProcessLocked(const std::shared_ptr<Applet>& applet) {
    if (!ReinitializeProcess(m_system, *applet->process, applet->program_id)) {
        LOG_ERROR(Service_AM, "Failed to restart winding applet_id={}",
                  static_cast<u32>(applet->applet_id));
        return false;
    }

    applet->aruid.pid = applet->process->GetProcessId();
    applet->is_process_running = false;
    applet->is_completed = false;

    applet->hid_registration.RegisterCurrentProcess();

    applet->lifecycle_manager.ResetForRelaunch();
    applet->is_activity_runnable = false;

    applet->launch_reason.flag = 1;

    m_event_observer->TrackAppletProcess(applet);
    return true;
}

bool WindowSystem::LockHomeMenuIntoForegroundLocked() {
    if (m_home_menu == nullptr || !m_home_menu_foreground_locked) {
        m_home_menu_foreground_locked = false;
        return false;
    }

    std::scoped_lock lk{m_home_menu->lock};

    this->TerminateChildAppletsLocked(m_home_menu);

    if (m_home_menu->child_applets.empty()) {
        m_home_menu->window_visible = true;
        m_foreground_requested_applet = m_home_menu;
        return false;
    }

    return true;
}

void WindowSystem::TerminateChildAppletsLocked(Applet* applet) {
    auto child_applets = applet->child_applets;

    applet->lock.unlock();
    for (const auto& child_applet : child_applets) {
        // RequestExit may lock the applet itself, so call it before taking its lock.
        if (child_applet->frontend) {
            child_applet->frontend->RequestExit();
        }
        std::scoped_lock lk{child_applet->lock};
        child_applet->process->Terminate(true);
        child_applet->terminate_result = AM::ResultLibraryAppletTerminated;
        if (!child_applet->process->IsInitialized()) {
            child_applet->OnProcessTerminatedLocked();
            m_event_observer->RequestUpdate();
        }
    }
    applet->lock.lock();
}

void WindowSystem::ReconcileAppletTreeLocked(Applet* applet, bool is_foreground,
                                             bool input_intercepted) {
    if (!applet) {
        return;
    }

    std::scoped_lock lk{applet->lock};

    bool child_obscures = false;
    for (const auto& child : applet->child_applets) {
        std::scoped_lock lk2{child->lock};
        if (child->is_winding) {
            child_obscures = true;
            break;
        }
        const auto mode = child->library_applet_mode;
        if (child->is_process_running && child->window_visible &&
            (mode == LibraryAppletMode::AllForeground ||
             mode == LibraryAppletMode::AllForegroundInitiallyHidden)) {
            child_obscures = true;
            break;
        }
    }

    const bool is_obscured = child_obscures || !applet->window_visible;

    auto dp = ComputeStandardDisplayParams(*applet, is_foreground, input_intercepted, is_obscured);
    ApplyDisplayParams(*applet, dp);

    const bool inherited_foreground = applet->is_process_running && is_foreground;
    const auto visible_state =
        inherited_foreground ? ActivityState::ForegroundVisible : ActivityState::BackgroundVisible;
    const auto obscured_state = inherited_foreground ? ActivityState::ForegroundObscured
                                                     : ActivityState::BackgroundObscured;

    const auto current_state = applet->lifecycle_manager.GetActivityState();

    if (is_obscured && current_state != obscured_state) {
        LOG_INFO(Service_AM, "Applet activity: id={} pid={} state={} -> {}",
                 static_cast<u32>(applet->applet_id), applet->aruid.pid,
                 static_cast<u32>(current_state), static_cast<u32>(obscured_state));
        applet->lifecycle_manager.SetActivityState(obscured_state);
        applet->UpdateSuspensionStateLocked(true);
    } else if (!is_obscured && current_state != visible_state) {
        LOG_INFO(Service_AM, "Applet activity: id={} pid={} state={} -> {}",
                 static_cast<u32>(applet->applet_id), applet->aruid.pid,
                 static_cast<u32>(current_state), static_cast<u32>(visible_state));
        applet->lifecycle_manager.SetActivityState(visible_state);
        applet->UpdateSuspensionStateLocked(true);
    }

    for (const auto& child : applet->child_applets) {
        this->ReconcileAppletTreeLocked(child.get(), is_foreground, input_intercepted);
    }
}

void WindowSystem::SetHomeMenuRequestCallback(HomeMenuRequestCallback callback) {
    std::scoped_lock lk{m_lock};
    m_home_menu_request_callback = std::move(callback);
}

std::shared_ptr<Applet> WindowSystem::GetOverlayDisplayApplet() {
    std::scoped_lock lk{m_lock};

    if (m_overlay) {
        auto it = m_applets.find(m_overlay->aruid.pid);
        if (it != m_applets.end()) {
            return it->second;
        }
    }

    return nullptr;
}

} // namespace Service::AM
