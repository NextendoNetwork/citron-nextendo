// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/hle/service/nfc/common/amiibo_crypto.h"
#include "core/hle/service/nfp/nfp_types.h"
#include "input_common/drivers/virtual_amiibo.h"

namespace InputCommon {
constexpr PadIdentifier virtual_amiibo_identifier = {
    .guid = Common::UUID{},
    .port = 0,
    .pad = 0,
};

VirtualAmiibo::VirtualAmiibo(std::string input_engine_) : InputEngine(std::move(input_engine_)) {}

VirtualAmiibo::~VirtualAmiibo() = default;

Common::Input::DriverResult VirtualAmiibo::SetPollingMode(
    [[maybe_unused]] const PadIdentifier& identifier_,
    const Common::Input::PollingMode polling_mode_) {
    polling_mode = polling_mode_;

    switch (polling_mode) {
    case Common::Input::PollingMode::NFC:
        state = State::Initialized;
        return Common::Input::DriverResult::Success;
    default:
        if (state == State::TagNearby) {
            CloseAmiibo();
        }
        state = State::Disabled;
        return Common::Input::DriverResult::NotSupported;
    }
}

Common::Input::NfcState VirtualAmiibo::SupportsNfc(
    [[maybe_unused]] const PadIdentifier& identifier_) const {
    return Common::Input::NfcState::Success;
}
Common::Input::NfcState VirtualAmiibo::StartNfcPolling(const PadIdentifier& identifier_) {
    if (state != State::Initialized) {
        return Common::Input::NfcState::WrongDeviceState;
    }
    state = State::WaitingForAmiibo;
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::StopNfcPolling(const PadIdentifier& identifier_) {
    if (state == State::Disabled) {
        return Common::Input::NfcState::WrongDeviceState;
    }
    if (state == State::TagNearby) {
        CloseAmiibo();
    }
    state = State::Initialized;
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::ReadAmiiboData(const PadIdentifier& identifier_,
                                                      std::vector<u8>& out_data) {
    if (state != State::TagNearby) {
        return Common::Input::NfcState::WrongDeviceState;
    }

    if (status.tag_type != 1U << 1) {
        return Common::Input::NfcState::InvalidTagType;
    }

    out_data.resize(nfc_data.size());
    memcpy(out_data.data(), nfc_data.data(), nfc_data.size());
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::WriteNfcData(
    [[maybe_unused]] const PadIdentifier& identifier_, const std::vector<u8>& data) {
    const Common::FS::IOFile nfc_file{file_path, Common::FS::FileAccessMode::ReadWrite,
                                      Common::FS::FileType::BinaryFile};

    if (!nfc_file.IsOpen()) {
        LOG_ERROR(Core, "Amiibo is already on use");
        return Common::Input::NfcState::WriteFailed;
    }

    if (!nfc_file.Write(data)) {
        LOG_ERROR(Service_NFP, "Error writing to file");
        return Common::Input::NfcState::WriteFailed;
    }

    nfc_data = data;

    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::ReadMifareData(const PadIdentifier& identifier_,
                                                      const Common::Input::MifareRequest& request,
                                                      Common::Input::MifareRequest& out_data) {
    if (state != State::TagNearby) {
        return Common::Input::NfcState::WrongDeviceState;
    }

    if (status.tag_type != 1U << 6) {
        return Common::Input::NfcState::InvalidTagType;
    }

    for (std::size_t i = 0; i < request.data.size(); i++) {
        if (request.data[i].command == 0) {
            continue;
        }
        out_data.data[i].command = request.data[i].command;
        out_data.data[i].sector = request.data[i].sector;

        const std::size_t sector_index =
            request.data[i].sector * sizeof(Common::Input::MifareData::data);

        if (nfc_data.size() < sector_index + sizeof(Common::Input::MifareData::data)) {
            return Common::Input::NfcState::WriteFailed;
        }

        // Ignore the sector key as we don't support it
        memcpy(out_data.data[i].data.data(), nfc_data.data() + sector_index,
               sizeof(Common::Input::MifareData::data));
    }

    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::WriteMifareData(
    const PadIdentifier& identifier_, const Common::Input::MifareRequest& request) {
    if (state != State::TagNearby) {
        return Common::Input::NfcState::WrongDeviceState;
    }

    if (status.tag_type != 1U << 6) {
        return Common::Input::NfcState::InvalidTagType;
    }

    for (std::size_t i = 0; i < request.data.size(); i++) {
        if (request.data[i].command == 0) {
            continue;
        }

        const std::size_t sector_index =
            request.data[i].sector * sizeof(Common::Input::MifareData::data);

        if (nfc_data.size() < sector_index + sizeof(Common::Input::MifareData::data)) {
            return Common::Input::NfcState::WriteFailed;
        }

        // Ignore the sector key as we don't support it
        memcpy(nfc_data.data() + sector_index, request.data[i].data.data(),
               sizeof(Common::Input::MifareData::data));
    }

    return Common::Input::NfcState::Success;
}

VirtualAmiibo::State VirtualAmiibo::GetCurrentState() const {
    return state;
}

VirtualAmiibo::Info VirtualAmiibo::LoadAmiibo(const std::string& filename) {
    const Common::FS::IOFile nfc_file{filename, Common::FS::FileAccessMode::Read,
                                      Common::FS::FileType::BinaryFile};
    std::vector<u8> data{};

    if (!nfc_file.IsOpen()) {
        return Info::UnableToLoad;
    }

    switch (nfc_file.GetSize()) {
    case AmiiboSize:
    case AmiiboSizeWithoutPassword:
    case AmiiboSizeWithSignature:
        data.resize(AmiiboSize);
        if (nfc_file.Read(data) < AmiiboSizeWithoutPassword) {
            return Info::NotAnAmiibo;
        }
        break;
    case MifareSize:
        data.resize(MifareSize);
        if (nfc_file.Read(data) < MifareSize) {
            return Info::NotAnAmiibo;
        }
        break;
    default:
        return Info::NotAnAmiibo;
    }

    file_path = filename;
    return LoadAmiibo(data);
}

VirtualAmiibo::Info VirtualAmiibo::LoadVirtualAmiibo(const std::string& amiibo_id,
                                                                  const std::string& amiibo_name) {
    if (state != State::WaitingForAmiibo) {
        return Info::WrongDeviceState;
    }
    if (amiibo_id.size() != 16 ||
        !std::all_of(amiibo_id.begin(), amiibo_id.end(),
                     [](unsigned char value) { return std::isxdigit(value) != 0; })) {
        return Info::NotAnAmiibo;
    }

    const auto parse_hex = [&amiibo_id](std::size_t offset, std::size_t count) {
        return static_cast<u16>(std::stoul(amiibo_id.substr(offset, count), nullptr, 16));
    };

    Service::NFP::NTAG215File tag{};
    std::random_device random;
    for (u8& value : tag.uid.part1) value = static_cast<u8>(random());
    for (u8& value : tag.uid.part2) value = static_cast<u8>(random());
    tag.uid.nintendo_id = static_cast<u8>(random());
    tag.uid.crc_check1 = 0x88 ^ tag.uid.part1[0] ^ tag.uid.part1[1] ^ tag.uid.part1[2];
    tag.uid_crc_check2 =
        tag.uid.part2[0] ^ tag.uid.part2[1] ^ tag.uid.part2[2] ^ tag.uid.nintendo_id;

    tag.internal_number = 0x48;
    tag.static_lock = 0xE00F;
    tag.compatibility_container = 0xEEFF10F1U;
    tag.constant_value = 0xA5;
    tag.amiibo_version = 1;
    tag.dynamic_lock = 0x0F0001;
    tag.CFG0 = 0x04000000;
    tag.CFG1 = 0x5F;

    const u16 character_id = parse_hex(0, 4);
    const std::array<u8, 2> character_id_bytes{
        static_cast<u8>(character_id >> 8), static_cast<u8>(character_id)};
    std::memcpy(&tag.model_info.character_id, character_id_bytes.data(), character_id_bytes.size());
    tag.model_info.character_variant = static_cast<u8>(parse_hex(4, 2));
    tag.model_info.amiibo_type = static_cast<Service::NFP::AmiiboType>(parse_hex(6, 2));
    tag.model_info.model_number = parse_hex(8, 4);
    tag.model_info.series = static_cast<Service::NFP::AmiiboSeries>(parse_hex(12, 2));
    tag.model_info.tag_type = Service::NFC::PackedTagType::Type2;

    // The NFP service fills in a default Mii when this virtual tag is first mounted.
    // Keep registration uninitialized in the stored image so this remains a valid plain tag.
    const Service::NFP::WriteDate date{.year = 2026, .month = 1, .day = 1};
    tag.settings.init_date.SetWriteDate(date);
    tag.settings.write_date.SetWriteDate(date);
    const std::u16string utf16_name = Common::UTF8ToUTF16(amiibo_name);
    for (std::size_t i = 0; i < std::min(utf16_name.size(), tag.settings.amiibo_name.size()); ++i) {
        tag.settings.amiibo_name[i] = static_cast<u16_be>(utf16_name[i]);
    }

    const auto amiibo_dir = Common::FS::GetCitronPath(Common::FS::CitronPath::AmiiboDir) / "virtual";
    std::error_code error;
    std::filesystem::create_directories(amiibo_dir, error);
    if (error) {
        return Info::UnableToLoad;
    }
    const auto path = amiibo_dir / (amiibo_id + ".bin");

    // Reuse the saved virtual tag so game data written to this amiibo is retained.
    if (std::filesystem::exists(path)) {
        virtual_amiibo_pending = true;
        const Info result = LoadAmiibo(path.string());
        virtual_amiibo_pending = false;
        return result;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Info::UnableToLoad;
    }
    output.write(reinterpret_cast<const char*>(&tag), sizeof(tag));
    output.close();
    if (!output) {
        return Info::UnableToLoad;
    }
    virtual_amiibo_pending = true;
    const Info result = LoadAmiibo(path.string());
    virtual_amiibo_pending = false;
    return result;
}

VirtualAmiibo::Info VirtualAmiibo::LoadAmiibo(std::span<u8> data) {
    if (state != State::WaitingForAmiibo) {
        return Info::WrongDeviceState;
    }

    switch (data.size_bytes()) {
    case AmiiboSize:
    case AmiiboSizeWithoutPassword:
    case AmiiboSizeWithSignature:
        nfc_data.resize(AmiiboSize);
        status.tag_type = 1U << 1;
        status.uuid_length = 7;
        break;
    case MifareSize:
        nfc_data.resize(MifareSize);
        status.tag_type = 1U << 6;
        status.uuid_length = 4;
        break;
    default:
        return Info::NotAnAmiibo;
    }

    status.uuid = {};
    status.protocol = 1;
    status.virtual_amiibo = virtual_amiibo_pending;
    state = State::TagNearby;
    status.state = Common::Input::NfcState::NewAmiibo,
    memcpy(nfc_data.data(), data.data(), data.size_bytes());
    if (nfc_data.size() == AmiiboSize) {
        Service::NFP::NTAG215File plain_tag{};
        std::memcpy(&plain_tag, nfc_data.data(), sizeof(plain_tag));
        if (Service::NFP::AmiiboCrypto::IsAmiiboValid(plain_tag)) {
            std::memcpy(status.uuid.data(), plain_tag.uid.part1.data(), plain_tag.uid.part1.size());
            std::memcpy(status.uuid.data() + plain_tag.uid.part1.size(), plain_tag.uid.part2.data(),
                        plain_tag.uid.part2.size());
            status.uuid[6] = plain_tag.uid.nintendo_id;
        } else {
            std::memcpy(status.uuid.data(), nfc_data.data(), status.uuid_length);
        }
    } else {
        std::memcpy(status.uuid.data(), nfc_data.data(), status.uuid_length);
    }
    SetNfc(virtual_amiibo_identifier, status);
    return Info::Success;
}

VirtualAmiibo::Info VirtualAmiibo::ReloadAmiibo() {
    if (state == State::TagNearby) {
        SetNfc(virtual_amiibo_identifier, status);
        return Info::Success;
    }

    return LoadAmiibo(file_path);
}

VirtualAmiibo::Info VirtualAmiibo::CloseAmiibo() {
    if (state != State::TagNearby) {
        return Info::Success;
    }

    state = State::WaitingForAmiibo;
    status.state = Common::Input::NfcState::AmiiboRemoved;
    SetNfc(virtual_amiibo_identifier, status);
    status.tag_type = 0;
    status.virtual_amiibo = false;
    virtual_amiibo_pending = false;
    return Info::Success;
}

std::string VirtualAmiibo::GetLastFilePath() const {
    return file_path;
}

} // namespace InputCommon
