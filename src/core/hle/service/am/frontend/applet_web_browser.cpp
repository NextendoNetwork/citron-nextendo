// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/fs_filesystem.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/system_archive/system_archive.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/frontend/applets/error.h"
#include "core/frontend/applets/web_browser.h"
#include "core/hle/result.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/frontend/applet_web_browser.h"
#include "core/hle/service/am/service/storage.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/ns/platform_service_manager.h"
#include "core/loader/loader.h"

// [UNITY-FIX] winbase.h A/W macros shadow C++ method names.
#undef DeleteFile
#undef CreateFile
#undef CopyFile
#undef MoveFile
#undef MoveFileEx
#undef CreateDirectory
#undef RemoveDirectory

namespace Service::AM::Frontend {

namespace {

template <typename T>
void ParseRawValue(T& value, const std::vector<u8>& data) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "It's undefined behavior to use memcpy with non-trivially copyable objects");
    std::memcpy(&value, data.data(), data.size());
}

template <typename T>
T ParseRawValue(const std::vector<u8>& data) {
    T value;
    ParseRawValue(value, data);
    return value;
}

std::string ParseStringValue(const std::vector<u8>& data) {
    return Common::StringFromFixedZeroTerminatedBuffer(reinterpret_cast<const char*>(data.data()),
                                                       data.size());
}

bool IsPokemonTermsTitle(u64 program_id) {
    return program_id == 0x01008F6008C5E000ULL || program_id == 0x0100A3D008C5C000ULL ||
           program_id == 0x0100F43008C44000ULL;
}

// Battle Stadium, competition entry (Scarlet/Violet, /scvi/) and Ranked Battles (Legends: Z-A,
// /plza/) open a terms page on battle-*.pokemon-home.com and wait for its callback; honor only a
// callback in the same page directory on the same origin.
bool IsPokemonTermsCallback(std::string_view initial_url, std::string_view callback_url) {
    constexpr std::string_view scheme = "https://";
    if (!initial_url.starts_with("https://battle-")) {
        return false;
    }
    const auto path_start = initial_url.find('/', scheme.size());
    if (path_start == std::string_view::npos ||
        !initial_url.substr(0, path_start).ends_with(".pokemon-home.com") ||
        !(initial_url.substr(path_start).starts_with("/scvi/") ||
          initial_url.substr(path_start).starts_with("/plza/"))) {
        return false;
    }
    const auto page = initial_url.substr(0, initial_url.find('?'));
    return callback_url == std::string{page.substr(0, page.rfind('/'))} + "/callback";
}

// Output TLVs, which 8.0.0+ Web and 3.0.0+ Share applets return instead of WebCommonReturnValue.
std::vector<u8> BuildWebOutputTlvs(ShimKind shim_kind, WebExitReason exit_reason,
                                   std::string_view last_url) {
    std::vector<u8> url(last_url.begin(), last_url.end());
    url.push_back(0); // The reported size includes the terminator.

    std::vector<u8> out;
    const auto append = [&out](const auto& value) {
        const auto* bytes = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(value));
    };
    const auto append_tlv = [&](WebArgOutputTLVType type, std::span<const u8> data) {
        WebArgOutputTLV tlv{};
        tlv.output_tlv_type = type;
        tlv.arg_data_size = static_cast<u16>(data.size());
        append(tlv);
        out.insert(out.end(), data.begin(), data.end());
    };

    WebArgHeader header{};
    header.total_tlv_entries = last_url.empty() ? 1 : 3;
    header.shim_kind = shim_kind;
    append(header);

    const auto reason = static_cast<u32>(exit_reason);
    append_tlv(WebArgOutputTLVType::ShareExitReason,
               {reinterpret_cast<const u8*>(&reason), sizeof(reason)});
    if (!last_url.empty()) {
        const u64 url_size = url.size();
        append_tlv(WebArgOutputTLVType::LastURL, url);
        append_tlv(WebArgOutputTLVType::LastURLSize,
                   {reinterpret_cast<const u8*>(&url_size), sizeof(url_size)});
    }
    // The SDK reads the output as one fixed 0x2000-byte storage.
    out.resize(std::max<size_t>(out.size(), 0x2000));
    return out;
}

// Ends a Pokémon terms page on its callback; accepting the terms goes to /agree.
std::vector<u8> BuildPokemonTermsResponse(std::string callback_url) {
    if (callback_url.ends_with("/terms/callback") ||
        callback_url.ends_with("/scvi/battle-terms/callback") ||
        callback_url.ends_with("/scvi/competition/callback")) {
        callback_url += "/agree";
    }
    return BuildWebOutputTlvs(ShimKind::Web, WebExitReason::CallbackURL, callback_url);
}

std::string GetMainURL(const std::string& url) {
    const auto index = url.find('?');

    if (index == std::string::npos) {
        return url;
    }

    return url.substr(0, index);
}

std::string ResolveURL(const std::string& url) {
    const auto index = url.find_first_of('%');

    if (index == std::string::npos) {
        return url;
    }

    return url.substr(0, index) + "lp1" + url.substr(index + 1);
}

WebArgInputTLVMap ReadWebArgs(const std::vector<u8>& web_arg, WebArgHeader& web_arg_header) {
    std::memcpy(&web_arg_header, web_arg.data(), sizeof(WebArgHeader));

    if (web_arg.size() == sizeof(WebArgHeader)) {
        return {};
    }

    WebArgInputTLVMap input_tlv_map;

    u64 current_offset = sizeof(WebArgHeader);

    for (std::size_t i = 0; i < web_arg_header.total_tlv_entries; ++i) {
        if (web_arg.size() < current_offset + sizeof(WebArgInputTLV)) {
            return input_tlv_map;
        }

        WebArgInputTLV input_tlv;
        std::memcpy(&input_tlv, web_arg.data() + current_offset, sizeof(WebArgInputTLV));

        current_offset += sizeof(WebArgInputTLV);

        if (web_arg.size() < current_offset + input_tlv.arg_data_size) {
            return input_tlv_map;
        }

        std::vector<u8> data(input_tlv.arg_data_size);
        std::memcpy(data.data(), web_arg.data() + current_offset, input_tlv.arg_data_size);

        current_offset += input_tlv.arg_data_size;

        input_tlv_map.insert_or_assign(input_tlv.input_tlv_type, std::move(data));
    }

    return input_tlv_map;
}

FileSys::VirtualFile GetOfflineRomFS(Core::System& system, u64 title_id,
                                     FileSys::ContentRecordType nca_type) {
    if (nca_type == FileSys::ContentRecordType::Data) {
        const auto nca =
            system.GetFileSystemController().GetSystemNANDContents()->GetEntry(title_id, nca_type);

        if (nca == nullptr) {
            LOG_ERROR(Service_AM,
                      "NCA of type={} with title_id={:016X} is not found in the System NAND!",
                      nca_type, title_id);
            return FileSys::SystemArchive::SynthesizeSystemArchive(title_id);
        }

        return nca->GetRomFS();
    } else {
        const auto nca = system.GetContentProvider().GetEntry(title_id, nca_type);

        if (nca == nullptr) {
            if (nca_type == FileSys::ContentRecordType::HtmlDocument) {
                LOG_WARNING(Service_AM, "Falling back to AppLoader to get the RomFS.");
                FileSys::VirtualFile romfs;
                system.GetAppLoader().ReadManualRomFS(romfs);
                if (romfs != nullptr) {
                    return romfs;
                }
            }

            LOG_ERROR(Service_AM,
                      "NCA of type={} with title_id={:016X} is not found in the ContentProvider!",
                      nca_type, title_id);
            return nullptr;
        }

        const FileSys::PatchManager pm{title_id, system.GetFileSystemController(),
                                       system.GetContentProvider()};

        return pm.PatchRomFS(nca.get(), nca->GetRomFS(), nca_type);
    }
}

void ExtractSharedFonts(Core::System& system) {
    static constexpr std::array<const char*, 7> DECRYPTED_SHARED_FONTS{
        "FontStandard.ttf",
        "FontChineseSimplified.ttf",
        "FontExtendedChineseSimplified.ttf",
        "FontChineseTraditional.ttf",
        "FontKorean.ttf",
        "FontNintendoExtended.ttf",
        "FontNintendoExtended2.ttf",
    };

    const auto fonts_dir = Common::FS::GetCitronPath(Common::FS::CitronPath::CacheDir) / "fonts";

    for (std::size_t i = 0; i < NS::SHARED_FONTS.size(); ++i) {
        const auto font_file_path = fonts_dir / DECRYPTED_SHARED_FONTS[i];

        if (Common::FS::Exists(font_file_path)) {
            continue;
        }

        const auto font = NS::SHARED_FONTS[i];
        const auto font_title_id = static_cast<u64>(font.first);

        const auto nca = system.GetFileSystemController().GetSystemNANDContents()->GetEntry(
            font_title_id, FileSys::ContentRecordType::Data);

        FileSys::VirtualFile romfs;

        if (!nca) {
            romfs = FileSys::SystemArchive::SynthesizeSystemArchive(font_title_id);
        } else {
            romfs = nca->GetRomFS();
        }

        if (!romfs) {
            LOG_ERROR(Service_AM, "SharedFont RomFS with title_id={:016X} cannot be extracted!",
                      font_title_id);
            continue;
        }

        const auto extracted_romfs = FileSys::ExtractRomFS(romfs);

        if (!extracted_romfs) {
            LOG_ERROR(Service_AM, "SharedFont RomFS with title_id={:016X} failed to extract!",
                      font_title_id);
            continue;
        }

        const auto font_file = extracted_romfs->GetFile(font.second);

        if (!font_file) {
            LOG_ERROR(Service_AM, "SharedFont RomFS with title_id={:016X} has no font file \"{}\"!",
                      font_title_id, font.second);
            continue;
        }

        std::vector<u32> font_data_u32(font_file->GetSize() / sizeof(u32));
        font_file->ReadBytes<u32>(font_data_u32.data(), font_file->GetSize());

        std::transform(font_data_u32.begin(), font_data_u32.end(), font_data_u32.begin(),
                       Common::swap32);

        std::vector<u8> decrypted_data(font_file->GetSize() - 8);

        NS::DecryptSharedFontToTTF(font_data_u32, decrypted_data);

        FileSys::VirtualFile decrypted_font = std::make_shared<FileSys::VectorVfsFile>(
            std::move(decrypted_data), DECRYPTED_SHARED_FONTS[i]);

        const auto temp_dir = system.GetFilesystem()->CreateDirectory(
            Common::FS::PathToUTF8String(fonts_dir), FileSys::OpenMode::ReadWrite);

        const auto out_file = temp_dir->CreateFile(DECRYPTED_SHARED_FONTS[i]);

        FileSys::VfsRawCopy(decrypted_font, out_file);
    }
}

} // namespace

WebBrowser::WebBrowser(Core::System& system_, std::shared_ptr<Applet> applet_,
                       LibraryAppletMode applet_mode_,
                       const Core::Frontend::WebBrowserApplet& frontend_)
    : FrontendApplet{system_, applet_, applet_mode_}, frontend(frontend_) {}

WebBrowser::~WebBrowser() = default;

void WebBrowser::Initialize() {
    FrontendApplet::Initialize();

    LOG_INFO(Service_AM, "Initializing Web Browser Applet.");

    LOG_DEBUG(Service_AM,
              "Initializing Applet with common_args: arg_version={}, lib_version={}, "
              "play_startup_sound={}, size={}, system_tick={}, theme_color={}",
              common_args.arguments_version, common_args.library_version,
              common_args.play_startup_sound, common_args.size, common_args.system_tick,
              common_args.theme_color);

    web_applet_version = WebAppletVersion{common_args.library_version};

    const auto web_arg_storage = PopInData();
    ASSERT(web_arg_storage != nullptr);

    const auto& web_arg = web_arg_storage->GetData();
    ASSERT_OR_EXECUTE(web_arg.size() >= sizeof(WebArgHeader), { return; });

    web_arg_input_tlv_map = ReadWebArgs(web_arg, web_arg_header);

    LOG_DEBUG(Service_AM, "WebArgHeader: total_tlv_entries={}, shim_kind={}",
              web_arg_header.total_tlv_entries, web_arg_header.shim_kind);

    ExtractSharedFonts(system);

    switch (web_arg_header.shim_kind) {
    case ShimKind::Shop:
        InitializeShop();
        break;
    case ShimKind::Login:
        InitializeLogin();
        break;
    case ShimKind::Offline:
        InitializeOffline();
        break;
    case ShimKind::Share:
        InitializeShare();
        break;
    case ShimKind::Web:
        InitializeWeb();
        break;
    case ShimKind::Wifi:
        InitializeWifi();
        break;
    case ShimKind::Lobby:
        InitializeLobby();
        break;
    case ShimKind::Unknown8:
        LOG_INFO(Service_AM, "Nextendo Page: initialized MyNso web protocol (shim 8)");
        break;
    default:
        ASSERT_MSG(false, "Invalid ShimKind={}", web_arg_header.shim_kind);
        break;
    }
}

Result WebBrowser::GetStatus() const {
    return status;
}

void WebBrowser::ExecuteInteractive() {
    UNIMPLEMENTED_MSG("WebSession is not implemented");
}

void WebBrowser::Execute() {
    switch (web_arg_header.shim_kind) {
    case ShimKind::Shop:
        ExecuteShop();
        break;
    case ShimKind::Login:
        ExecuteLogin();
        break;
    case ShimKind::Offline:
        ExecuteOffline();
        break;
    case ShimKind::Share:
        ExecuteShare();
        break;
    case ShimKind::Web:
        ExecuteWeb();
        break;
    case ShimKind::Wifi:
        ExecuteWifi();
        break;
    case ShimKind::Lobby:
        ExecuteLobby();
        break;
    case ShimKind::Unknown8:
        frontend.OpenExternalWebPage("https://nextendo.network/compte",
            [this](WebExitReason reason, std::string) { WebBrowserExit(reason); });
        break;
    default:
        ASSERT_MSG(false, "Invalid ShimKind={}", web_arg_header.shim_kind);
        WebBrowserExit(WebExitReason::EndButtonPressed);
        break;
    }
}

void WebBrowser::ExtractOfflineRomFS() {
    LOG_DEBUG(Service_AM, "Extracting RomFS to {}",
              Common::FS::PathToUTF8String(offline_cache_dir));

    const auto extracted_romfs_dir = FileSys::ExtractRomFS(offline_romfs);

    const auto temp_dir = system.GetFilesystem()->CreateDirectory(
        Common::FS::PathToUTF8String(offline_cache_dir), FileSys::OpenMode::ReadWrite);

    FileSys::VfsRawCopyD(extracted_romfs_dir, temp_dir);
}

void WebBrowser::WebBrowserExit(WebExitReason exit_reason, std::string last_url) {
    if ((web_arg_header.shim_kind == ShimKind::Share &&
         web_applet_version >= WebAppletVersion::Version196608) ||
        web_arg_header.shim_kind == ShimKind::Unknown8 ||
        (web_arg_header.shim_kind == ShimKind::Web &&
         web_applet_version >= WebAppletVersion::Version524288)) {
        // These versions pop output TLVs; a WebCommonReturnValue here aborts the SDK (2162-0001).
        complete = true;
        LOG_INFO(Service_AM, "Web applet return: shim={} TLV storage=8192 reason={}",
                 web_arg_header.shim_kind, exit_reason);
        PushOutData(std::make_shared<IStorage>(
            system, BuildWebOutputTlvs(web_arg_header.shim_kind, exit_reason, last_url)));
        Exit();
        return;
    }

    WebCommonReturnValue web_common_return_value;

    web_common_return_value.exit_reason = exit_reason;
    std::memcpy(&web_common_return_value.last_url, last_url.data(), last_url.size());
    web_common_return_value.last_url_size = last_url.size();

    LOG_DEBUG(Service_AM, "WebCommonReturnValue: exit_reason={}, last_url={}, last_url_size={}",
              exit_reason, last_url, last_url.size());

    complete = true;
    std::vector<u8> out_data(sizeof(WebCommonReturnValue));
    std::memcpy(out_data.data(), &web_common_return_value, out_data.size());
    PushOutData(std::make_shared<IStorage>(system, std::move(out_data)));
    Exit();
}

Result WebBrowser::RequestExit() {
    frontend.Close();
    R_SUCCEED();
}

bool WebBrowser::InputTLVExistsInMap(WebArgInputTLVType input_tlv_type) const {
    return web_arg_input_tlv_map.find(input_tlv_type) != web_arg_input_tlv_map.end();
}

std::optional<std::vector<u8>> WebBrowser::GetInputTLVData(WebArgInputTLVType input_tlv_type) {
    const auto map_it = web_arg_input_tlv_map.find(input_tlv_type);

    if (map_it == web_arg_input_tlv_map.end()) {
        return std::nullopt;
    }

    return map_it->second;
}

void WebBrowser::InitializeShop() {}

void WebBrowser::InitializeLogin() {}

void WebBrowser::InitializeOffline() {
    const auto document_path =
        ParseStringValue(GetInputTLVData(WebArgInputTLVType::DocumentPath).value());

    const auto document_kind =
        ParseRawValue<DocumentKind>(GetInputTLVData(WebArgInputTLVType::DocumentKind).value());

    std::string additional_paths;

    switch (document_kind) {
    case DocumentKind::OfflineHtmlPage:
    default:
        title_id = system.GetApplicationProcessProgramID();
        nca_type = FileSys::ContentRecordType::HtmlDocument;
        additional_paths = "html-document";
        break;
    case DocumentKind::ApplicationLegalInformation:
        title_id = ParseRawValue<u64>(GetInputTLVData(WebArgInputTLVType::ApplicationID).value());
        nca_type = FileSys::ContentRecordType::LegalInformation;
        break;
    case DocumentKind::SystemDataPage:
        title_id = ParseRawValue<u64>(GetInputTLVData(WebArgInputTLVType::SystemDataID).value());
        nca_type = FileSys::ContentRecordType::Data;
        break;
    }

    static constexpr std::array<const char*, 3> RESOURCE_TYPES{
        "manual",
        "legal_information",
        "system_data",
    };

    offline_cache_dir = Common::FS::GetCitronPath(Common::FS::CitronPath::CacheDir) /
                        fmt::format("offline_web_applet_{}/{:016X}",
                                    RESOURCE_TYPES[static_cast<u32>(document_kind) - 1], title_id);

    offline_document = Common::FS::ConcatPathSafe(
        offline_cache_dir, fmt::format("{}/{}", additional_paths, document_path));
}

void WebBrowser::InitializeShare() {}

void WebBrowser::InitializeWeb() {
    external_url = ParseStringValue(GetInputTLVData(WebArgInputTLVType::InitialURL).value());

    // Resolve Nintendo CDN URLs.
    external_url = ResolveURL(external_url);
}

void WebBrowser::InitializeWifi() {}

void WebBrowser::InitializeLobby() {}

void WebBrowser::ExecuteShop() {
    LOG_WARNING(Service_AM, "(STUBBED) called, Shop Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}

void WebBrowser::ExecuteLogin() {
    LOG_WARNING(Service_AM, "(STUBBED) called, Login Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}

void WebBrowser::ExecuteOffline() {
    // TODO (Morph): This is a hack for WebSession foreground web applets such as those used by
    //               Super Mario 3D All-Stars.
    // TODO (Morph): Implement WebSession.
    if (applet_mode == LibraryAppletMode::AllForegroundInitiallyHidden) {
        LOG_WARNING(Service_AM, "WebSession is not implemented");
        return;
    }

    const auto main_url = GetMainURL(Common::FS::PathToUTF8String(offline_document));

    if (!Common::FS::Exists(main_url)) {
        offline_romfs = GetOfflineRomFS(system, title_id, nca_type);

        if (offline_romfs == nullptr) {
            LOG_ERROR(Service_AM,
                      "RomFS with title_id={:016X} and nca_type={} cannot be extracted!", title_id,
                      nca_type);
            WebBrowserExit(WebExitReason::WindowClosed);
            return;
        }
    }

    LOG_INFO(Service_AM, "Opening offline document at {}",
             Common::FS::PathToUTF8String(offline_document));

    frontend.OpenLocalWebPage(
        Common::FS::PathToUTF8String(offline_document), [this] { ExtractOfflineRomFS(); },
        [this](WebExitReason exit_reason, std::string last_url) {
            WebBrowserExit(exit_reason, last_url);
        });
}

void WebBrowser::ExecuteShare() {
    LOG_WARNING(Service_AM, "(STUBBED) called, Share Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}

void WebBrowser::ExecuteWeb() {
    if (web_applet_version >= WebAppletVersion::Version524288 &&
        IsPokemonTermsTitle(system.GetApplicationProcessProgramID())) {
        const auto initial_url = ParseStringValue(
            GetInputTLVData(WebArgInputTLVType::InitialURL).value_or(std::vector<u8>{}));
        const auto callback_url = ParseStringValue(
            GetInputTLVData(WebArgInputTLVType::CallbackURL).value_or(std::vector<u8>{}));
        if (IsPokemonTermsCallback(initial_url, callback_url)) {
            LOG_INFO(Service_AM, "Pokémon terms page answered with its callback");
            complete = true;
            PushOutData(
                std::make_shared<IStorage>(system, BuildPokemonTermsResponse(callback_url)));
            Exit();
            return;
        }
    }

    LOG_INFO(Service_AM, "Opening external URL at {}", external_url);

    frontend.OpenExternalWebPage(external_url,
                                 [this](WebExitReason exit_reason, std::string last_url) {
                                     WebBrowserExit(exit_reason, last_url);
                                 });
}

void WebBrowser::ExecuteWifi() {
    LOG_WARNING(Service_AM, "(STUBBED) called, Wifi Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}

void WebBrowser::ExecuteLobby() {
    // [Nextendo] NSO voice-chat lounge page; needs the phone app, so tell the player why.
    LOG_WARNING(Service_AM, "(STUBBED) called, Lobby Applet is not implemented");
    if (const auto& error = system.GetFrontendAppletHolder().GetFrontendAppletSet().error) {
        error->ShowCustomErrorText(
            ResultSuccess,
            "Voice chat isn't supported here — it requires the Nintendo Switch Online phone app.",
            "Turn OFF voice chat in this game's settings, then host or join again.", [] {});
    }
    WebBrowserExit(WebExitReason::EndButtonPressed);
}
} // namespace Service::AM::Frontend
