// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Service::AM {

struct Applet;
class EventObserver;
enum class AppletMessage : u32;
enum class SystemButtonType;

enum class ButtonPressDuration {
    ShortPressing,
    MiddlePressing,
    LongPressing,
};

using HomeMenuRequestCallback = std::function<void()>;

class WindowSystem {
public:
    explicit WindowSystem(Core::System& system);
    ~WindowSystem();

public:
    void SetEventObserver(EventObserver* event_observer);
    void Update();
    void RequestUpdate();

public:
    void TrackApplet(std::shared_ptr<Applet> applet, bool is_application);
    std::shared_ptr<Applet> GetByAppletResourceUserId(u64 aruid);
    std::shared_ptr<Applet> GetMainApplet();
    struct PresenceSnapshot {
        u64 program_id{};
        bool at_home{true};
    };
    // Frontend polling must never wait for AM while AM is dispatching frontend work.
    std::optional<PresenceSnapshot> TryGetPresenceSnapshot();
    u64 GetMainAppletResourceUserId() const {
        return m_main_applet_aruid.load(std::memory_order_relaxed);
    }
    std::shared_ptr<Applet> GetOverlayDisplayApplet();

    struct InvitationLaunchRequest {
        u64 program_id;
        std::vector<u8> storage;
    };
    bool RequestInvitationLaunch(u64 program_id, std::vector<u8> storage);
    std::optional<InvitationLaunchRequest> TakeInvitationLaunch();

public:
    bool RequestHomebrewMenu();
    void RequestHomeMenuToGetForeground();
    void RequestApplicationToGetForeground();
    void RequestLockHomeMenuIntoForeground();
    void RequestUnlockHomeMenuIntoForeground();
    void RequestAppletVisibilityState(Applet& applet, bool visible);

public:
    void OnOperationModeChanged();
    void OnExitRequested();
    void OnHomeButtonPressed(ButtonPressDuration type);
    void OnCaptureButtonPressed(ButtonPressDuration type);
    void OnPowerButtonPressed(ButtonPressDuration type) {}

public:
    void SetHomeMenuRequestCallback(HomeMenuRequestCallback callback);

private:
    struct DisplayParams {
        bool visible{};
        bool interactible{};
        s32 z_index{};
    };

    DisplayParams ComputeOverlayDisplayParams(Applet& applet) const;
    DisplayParams ComputeStandardDisplayParams(Applet& applet, bool is_foreground,
                                               bool input_intercepted, bool is_obscured) const;

    void PruneTerminatedAppletsLocked();
    bool RestartAppletProcessLocked(const std::shared_ptr<Applet>& applet);
    bool LockHomeMenuIntoForegroundLocked();
    void TerminateChildAppletsLocked(Applet* applet);
    void ReconcileAppletTreeLocked(Applet* applet, bool is_foreground, bool input_intercepted);
    void ApplyDisplayParams(Applet& applet, const DisplayParams& params);

    void BroadcastButtonMessage(AppletMessage message);

private:
    Core::System& m_system;

    EventObserver* m_event_observer{};

    std::mutex m_lock{};

    std::atomic_bool m_homebrew_requested{};
    bool m_homebrew_return_requested{};
    bool m_home_menu_foreground_locked{};
    Applet* m_foreground_requested_applet{};

    Applet* m_home_menu{};
    Applet* m_application{};
    Applet* m_overlay{};
    // The display compositor reads this while holding the VI container lock.
    // It must not acquire m_lock, since AM changes layer visibility under m_lock.
    std::atomic<u64> m_main_applet_aruid{};

    std::map<u64, std::shared_ptr<Applet>> m_applets{};

    HomeMenuRequestCallback m_home_menu_request_callback{};
    std::optional<InvitationLaunchRequest> m_invitation_launch;
};

} // namespace Service::AM
