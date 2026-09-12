// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/nextendo_mii.h"

#include <cstring>
#include <optional>

#include "common/logging.h"
#include "core/hle/service/mii/mii_database.h"
#include "core/hle/service/mii/mii_manager.h"
#include "core/hle/service/mii/mii_result.h"
#include "core/hle/service/mii/mii_util.h"
#include "core/hle/service/mii/types/raw_data.h"
#include "core/hle/service/mii/types/store_data.h"

namespace Nextendo::Mii {

namespace {

using Service::Mii::DatabaseSessionMetadata;
using Service::Mii::MiiManager;
using Service::Mii::StoreData;

std::optional<StoreData> Parse(std::span<const u8> bytes) {
    if (bytes.size() != sizeof(StoreData)) {
        return std::nullopt;
    }

    StoreData store_data{};
    std::memcpy(&store_data, bytes.data(), sizeof(StoreData));

    // The account bytes carry the device id of whichever install created the Mii; recompute
    // both CRCs for this one (Ryujinx does the same) so cross-device Miis stay valid.
    store_data.SetChecksum();
    if (store_data.IsValid() != Service::Mii::ValidationResult::NoErrors) {
        return std::nullopt;
    }
    return store_data;
}

// The Mii database is the same NAND file the emulated mii service uses, and MiiManager mounts
// it in Initialize(), so this works with no guest running; the UI keeps a game from running
// during a sync so the two never write at once.
void Initialize(MiiManager& manager, DatabaseSessionMetadata& metadata) {
    metadata = DatabaseSessionMetadata{};
    metadata.magic = Service::Mii::MiiMagic;
    manager.Initialize(metadata);
}

} // Anonymous namespace

Result Apply(std::span<const u8> store_data) {
    const auto parsed = Parse(store_data);
    if (!parsed) {
        return Result::Invalid;
    }

    MiiManager manager;
    DatabaseSessionMetadata metadata{};
    Initialize(manager, metadata);

    const auto result = manager.AddOrReplace(metadata, *parsed);
    if (result.IsFailure()) {
        LOG_WARNING(Frontend, "Nextendo Mii apply failed ({:#x})", result.raw);
        return Result::Failed;
    }

    LOG_INFO(Frontend, "Nextendo Mii applied");
    return Result::Applied;
}

Result Remove(std::span<const u8> store_data) {
    const auto parsed = Parse(store_data);
    if (!parsed) {
        return Result::Invalid;
    }

    MiiManager manager;
    DatabaseSessionMetadata metadata{};
    Initialize(manager, metadata);

    const auto result = manager.Delete(metadata, parsed->GetCreateId());
    if (result == Service::Mii::ResultNotFound) {
        return Result::NotFound;
    }
    if (result.IsFailure()) {
        LOG_WARNING(Frontend, "Nextendo Mii remove failed ({:#x})", result.raw);
        return Result::Failed;
    }

    LOG_INFO(Frontend, "Nextendo Mii removed");
    return Result::Removed;
}

std::vector<u8> Create() {
    StoreData store_data{};
    const u32 index = Service::Mii::MiiUtil::GetRandomValue<u32>(
        0, static_cast<u32>(Service::Mii::RawData::DefaultMii.size() - 1));
    store_data.BuildDefault(index);

    std::vector<u8> out(sizeof(StoreData));
    std::memcpy(out.data(), &store_data, sizeof(StoreData));
    return out;
}

} // namespace Nextendo::Mii
