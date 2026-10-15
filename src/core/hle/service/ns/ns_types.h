// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>

#include "common/common_funcs.h"
#include "common/uuid.h"
#include "core/file_sys/romfs_factory.h"

namespace Service::NS {

enum class ApplicationEvent : u8 {
    None = 0,
    Installing = 2,
    Installed = 3,
    GameCardNotInserted = 5,
    Archived = 11,
    GameCard = 16,
};

enum ApplicationViewFlags : u32 {
    ApplicationViewFlags_Installed = 1 << 0,
    ApplicationViewFlags_Visible = 1 << 1,
    ApplicationViewFlags_Initialized = 1 << 2,
    ApplicationViewFlags_Accessible = 1 << 3,
};

enum class ApplicationControlSource : u8 {
    CacheOnly = 0,
    Storage = 1,
    StorageOnly = 2,
};

constexpr u32 DefaultApplicationViewFlags = 0x401f17;
constexpr u32 DefaultApplicationVersion = 0x70000;

enum class ApplicationDownloadState : u8 {
    Runnable = 0,
    Suspended,
    NotEnoughSpace,
    Fatal,
    Finished,
    SystemUpdateRequired
};

enum class BackgroundNetworkUpdateState : u8 {
    None,
    InProgress,
    Ready,
};

struct ApplicationRecord {
    u64 application_id;
    ApplicationEvent last_event;
    u8 attributes;
    INSERT_PADDING_BYTES_NOINIT(0x6);
    s64 last_updated;
};
static_assert(sizeof(ApplicationRecord) == 0x18, "ApplicationRecord has incorrect size.");

struct ApplicationContentMetaStatus {
    u8 type;
    FileSys::StorageId storage_id;
    u8 rights_check; // NotChecked: inventory does not grant rights.
    u8 reserved;
    u32 version;
    u64 title_id;
};
static_assert(sizeof(ApplicationContentMetaStatus) == 0x10);

/// ApplicationDownloadProgress
struct ApplicationDownloadProgress {
    u64 downloaded_size;
    u64 total_size;
    u32 last_result;
    ApplicationDownloadState state;
    INSERT_PADDING_BYTES(0x3);
};
static_assert(sizeof(ApplicationDownloadProgress) == 0x18);

struct ApplicationApplyDeltaProgress {
    u64 applied_size;
    u64 total_size;
    u32 last_result;
    u8 state = 4; // NoTask; zero would mean Applying.
    INSERT_PADDING_BYTES(0x3);
};
static_assert(sizeof(ApplicationApplyDeltaProgress) == 0x18);

struct ApplicationRightsOnClient {
    u64 application_id;
    Common::UUID uid;
    u8 flags;
    u8 flags2;
    INSERT_PADDING_BYTES_NOINIT(0x6);
};
static_assert(sizeof(ApplicationRightsOnClient) == 0x20,
              "ApplicationRightsOnClient has incorrect size.");

/// NsPromotionInfo
struct PromotionInfo {
    u64 start_timestamp; ///< POSIX timestamp for the promotion start.
    u64 end_timestamp;   ///< POSIX timestamp for the promotion end.
    s64 remaining_time;  ///< Remaining time until the promotion ends, in nanoseconds
                         ///< ({end_timestamp - current_time} converted to nanoseconds).
    INSERT_PADDING_BYTES_NOINIT(0x4);
    u8 flags; ///< Flags. Bit0: whether the PromotionInfo is valid (including bit1). Bit1 clear:
              ///< remaining_time is set.
    INSERT_PADDING_BYTES_NOINIT(0x3);
};
static_assert(sizeof(PromotionInfo) == 0x20, "PromotionInfo has incorrect size.");

struct ApplicationViewV19 {
    u64 application_id;
    u32 version;
    u32 flags;
    ApplicationDownloadProgress download_state;
    ApplicationApplyDeltaProgress download_progress;
};
static_assert(sizeof(ApplicationViewV19) == 0x40);
static_assert(offsetof(ApplicationViewV19, download_progress) == 0x28);

struct ApplicationViewLegacy {
    u64 application_id;
    u32 version;
    u32 flags;
    ApplicationDownloadProgress download_state;
    INSERT_PADDING_BYTES(0x8);
    ApplicationApplyDeltaProgress download_progress;
    INSERT_PADDING_BYTES(0x8);
};
static_assert(sizeof(ApplicationViewLegacy) == 0x50);

struct ApplicationViewV20 {
    u64 application_id;
    u32 version;
    u32 flags;
    INSERT_PADDING_BYTES(0x8);
    ApplicationDownloadProgress download_state;
    INSERT_PADDING_BYTES(0x8);
    ApplicationApplyDeltaProgress download_progress;
    INSERT_PADDING_BYTES(0x8);
};
static_assert(sizeof(ApplicationViewV20) == 0x58);
static_assert(offsetof(ApplicationViewV20, download_state) == 0x18);
static_assert(offsetof(ApplicationViewV20, download_progress) == 0x38);

struct ApplicationViewData {
    u64 application_id{};
    u32 version{};
    u32 flags{};
    ApplicationDownloadProgress download_state{};
    ApplicationApplyDeltaProgress download_progress{};
};

inline size_t WriteApplicationView(void* dst, size_t dst_size, const ApplicationViewData& data,
                                   bool is_fw20) {
    if (is_fw20) {
        if (dst_size < sizeof(ApplicationViewV20)) return 0;
        auto* out = reinterpret_cast<ApplicationViewV20*>(dst);
        *out = {};
        out->application_id = data.application_id;
        out->version = data.version;
        out->flags = data.flags;
        out->download_state = data.download_state;
        out->download_progress = data.download_progress;
        return sizeof(ApplicationViewV20);
    } else {
        if (dst_size < sizeof(ApplicationViewV19)) return 0;
        auto* out = reinterpret_cast<ApplicationViewV19*>(dst);
        *out = {};
        out->application_id = data.application_id;
        out->version = data.version;
        out->flags = data.flags;
        out->download_state = data.download_state;
        out->download_progress = data.download_progress;
        return sizeof(ApplicationViewV19);
    }
}

struct ApplicationViewWithPromotionData {
    ApplicationViewData view;
    PromotionInfo promotion;
};

inline size_t WriteApplicationViewWithPromotion(void* dst, size_t dst_size,
                                                const ApplicationViewWithPromotionData& data,
                                                bool extended_view, size_t entry_size) {
    const bool legacy_view = extended_view && entry_size == 0x70;
    const size_t view_size = legacy_view ? sizeof(ApplicationViewLegacy)
        : extended_view ? sizeof(ApplicationViewV20) : sizeof(ApplicationViewV19);
    if (entry_size < view_size + sizeof(PromotionInfo) || dst_size < entry_size) return 0;
    // Firmware 20 extends ApplicationView itself to 0x58; the promotion follows
    // at 0x58, giving the 0x78 stride requested by HOME's SDK.
    std::memset(dst, 0, entry_size);
    size_t view_written{};
    if (legacy_view) {
        ApplicationViewLegacy view{};
        view.application_id = data.view.application_id;
        view.version = data.view.version;
        view.flags = data.view.flags;
        view.download_state = data.view.download_state;
        view.download_progress = data.view.download_progress;
        std::memcpy(dst, &view, sizeof(view));
        view_written = sizeof(view);
    } else {
        view_written = WriteApplicationView(dst, dst_size, data.view, extended_view);
    }
    if (view_written == 0) return 0;
    const size_t remaining = dst_size - view_written;
    if (remaining < sizeof(PromotionInfo)) return 0;
    auto* promo_dst = reinterpret_cast<u8*>(dst) + view_written;
    std::memcpy(promo_dst, &data.promotion, sizeof(PromotionInfo));
    return entry_size;
}

struct ApplicationOccupiedSizeEntity {
    FileSys::StorageId storage_id;
    u64 app_size;
    u64 patch_size;
    u64 aoc_size;
};
static_assert(sizeof(ApplicationOccupiedSizeEntity) == 0x20,
              "ApplicationOccupiedSizeEntity has incorrect size.");

struct ApplicationOccupiedSize {
    std::array<ApplicationOccupiedSizeEntity, 4> entities;
};
static_assert(sizeof(ApplicationOccupiedSize) == 0x80,
              "ApplicationOccupiedSize has incorrect size.");

struct ContentPath {
    u8 file_system_proxy_type;
    u64 program_id;
};
static_assert(sizeof(ContentPath) == 0x10, "ContentPath has incorrect size.");

struct Uid {
    alignas(8) Common::UUID uuid;
};
static_assert(sizeof(Uid) == 0x10, "Uid has incorrect size.");

struct ApplicationDisplayData {
    std::array<char, 0x200> application_name;
    std::array<char, 0x100> developer_name;
};
static_assert(sizeof(ApplicationDisplayData) == 0x300, "ApplicationDisplayData has incorrect size.");

struct LogoPath {
    std::array<char, 0x300> path;
};
static_assert(std::is_trivially_copyable_v<LogoPath>, "LogoPath must be trivially copyable.");

} // namespace Service::NS
