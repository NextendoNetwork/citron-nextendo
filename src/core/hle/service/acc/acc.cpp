// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include "common/common_types.h"
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/nextendo_account.h"
#include "common/nextendo_avatar.h"
#include "common/nextendo_compatible_titles.h"
#include "common/settings.h"
#include <ranges>
#include "common/stb.h"
#include "common/string_util.h"
#include "common/swap.h"
#include "core/constants.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/service/acc/acc.h"
#include "core/hle/service/acc/acc_aa.h"
#include "core/hle/service/acc/acc_e.h"
#include "core/hle/service/acc/acc_e_u1.h"
#include "core/hle/service/acc/acc_e_u2.h"
#include "core/hle/service/acc/acc_su.h"
#include "core/hle/service/acc/acc_u0.h"
#include "core/hle/service/acc/acc_u1.h"
#include "core/hle/service/acc/async_context.h"
#include "core/hle/service/acc/dauth_0.h"
#include "core/hle/service/acc/errors.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/glue/glue_manager.h"
#include "core/hle/service/ns/ns_types.h"
#include "core/hle/service/server_manager.h"
#include "core/loader/loader.h"

namespace Service::Account {

namespace {

// The BAAS id_token read through LoadIdTokenCache. NEX parses it before logging in, so it must be
// a real RS256 JWT. Key comes from NEXTENDO_BAAS_SIGNING_KEY or nextendo_baas.pem, else generated.
constexpr const char* BaasIssuer = "https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";
constexpr const char* BaasJku =
    "https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com/1.0.0/certificates";
constexpr const char* BaasAudience = "ed9e2f05d286f7b8";
constexpr const char* BaasKeyId = "nextendo-baas-key-1";

std::string Base64UrlEncode(std::span<const u8> data) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    for (std::size_t i = 0; i < data.size(); i += 3) {
        const u32 remaining = static_cast<u32>(data.size() - i);
        const u32 triple = (static_cast<u32>(data[i]) << 16) |
                           (remaining > 1 ? static_cast<u32>(data[i + 1]) << 8 : 0) |
                           (remaining > 2 ? static_cast<u32>(data[i + 2]) : 0);

        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
        if (remaining > 1) {
            out += alphabet[(triple >> 6) & 0x3F];
        }
        if (remaining > 2) {
            out += alphabet[triple & 0x3F];
        }
    }

    return out;
}

std::string Base64UrlEncode(std::string_view text) {
    return Base64UrlEncode(
        std::span{reinterpret_cast<const u8*>(text.data()), text.size()});
}

std::string RandomHex(std::size_t bytes) {
    std::vector<u8> buffer(bytes);
    if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            buffer[i] = static_cast<u8>(std::rand());
        }
    }
    return Common::HexToString(buffer, /*upper=*/false);
}

EVP_PKEY* GetBaasSigningKey() {
    static EVP_PKEY* key = []() -> EVP_PKEY* {
        std::string pem;

        if (const char* env = std::getenv("NEXTENDO_BAAS_SIGNING_KEY"); env && *env) {
            pem = env;
        } else if (const auto file = Common::FS::ReadStringFromFile(
                       std::filesystem::path{"nextendo_baas.pem"}, Common::FS::FileType::TextFile);
                   !file.empty()) {
            pem = file;
        }

        if (pem.find("BEGIN") != std::string::npos) {
            BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
            if (bio) {
                EVP_PKEY* loaded = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
                BIO_free(bio);
                if (loaded) {
                    LOG_INFO(Service_ACC, "[Nextendo] Using the supplied BAAS signing key");
                    return loaded;
                }
                LOG_WARNING(Service_ACC,
                            "[Nextendo] BAAS signing key could not be parsed; generating one");
            }
        }

        // No key supplied: reuse a persisted auto-generated one so the identity (and its
        // public JWK) stays stable across launches instead of a fresh key every process start.
        const auto auto_key_path =
            Common::FS::GetCitronPath(Common::FS::CitronPath::KeysDir) / "nextendo_baas_auto.pem";
        if (const auto existing =
                Common::FS::ReadStringFromFile(auto_key_path, Common::FS::FileType::TextFile);
            existing.find("BEGIN") != std::string::npos) {
            BIO* bio = BIO_new_mem_buf(existing.data(), static_cast<int>(existing.size()));
            if (bio) {
                EVP_PKEY* loaded = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
                BIO_free(bio);
                if (loaded) {
                    LOG_DEBUG(Service_ACC, "[Nextendo] Using the persisted auto-generated BAAS signing key");
                    return loaded;
                }
            }
        }

        EVP_PKEY* generated = EVP_RSA_gen(2048);
        if (!generated) {
            LOG_ERROR(Service_ACC, "[Nextendo] Failed to generate a BAAS signing key");
            return generated;
        }

        BIO* out_bio = BIO_new(BIO_s_mem());
        if (out_bio && PEM_write_bio_PrivateKey(out_bio, generated, nullptr, nullptr, 0, nullptr,
                                                nullptr)) {
            char* data = nullptr;
            const long len = BIO_get_mem_data(out_bio, &data);
            if (len > 0 && data) {
                const auto written = Common::FS::WriteStringToFile(
                    auto_key_path, Common::FS::FileType::TextFile,
                    std::string_view{data, static_cast<size_t>(len)});
                if (written > 0) {
                    LOG_DEBUG(Service_ACC,
                             "[Nextendo] Generated and persisted a new BAAS signing key at {}",
                             auto_key_path.string());
                }
            }
        }
        if (out_bio) {
            BIO_free(out_bio);
        }
        return generated;
    }();

    return key;
}

std::string SignRs256(std::string_view signing_input) {
    EVP_PKEY* key = GetBaasSigningKey();
    if (!key) {
        return {};
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return {};
    }

    std::string signature;
    std::size_t length = 0;

    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
        EVP_DigestSign(ctx, nullptr, &length,
                       reinterpret_cast<const u8*>(signing_input.data()),
                       signing_input.size()) == 1) {
        std::vector<u8> raw(length);
        if (EVP_DigestSign(ctx, raw.data(), &length,
                           reinterpret_cast<const u8*>(signing_input.data()),
                           signing_input.size()) == 1) {
            raw.resize(length);
            signature = Base64UrlEncode(raw);
        }
    }

    if (signature.empty()) {
        LOG_ERROR(Service_ACC, "[Nextendo] Failed to sign the BAAS id_token");
    }

    EVP_MD_CTX_free(ctx);
    return signature;
}

std::string GetInstalledTitleVersion(Core::System& system) {
    const u64 program_id = system.GetApplicationProcessProgramID();
    if (program_id == 0) {
        return {};
    }
    const FileSys::PatchManager pm{program_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto metadata = pm.GetControlMetadata();
    return metadata.first != nullptr ? metadata.first->GetVersionString() : std::string{};
}

std::string BuildIdToken(const std::string& installed_version) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    const std::string header =
        fmt::format(R"({{"alg":"RS256","kid":"{}","typ":"id_token","jku":"{}"}})", BaasKeyId,
                    BaasJku);

    // [Nextendo] Ride the signed nx2 token in the "nnex" claim so the auth server can
    // cryptographically bind this NEX login to the account (anti-impersonation).
    std::string nnex_claim;
    if (Common::NextendoAccount::IsLinked()) {
        const std::string tok = Common::NextendoAccount::GetToken();
        if (!tok.empty()) {
            nnex_claim = fmt::format(R"("nnex":"{}",)", tok);
        }
    }

    // [Nextendo] The console's own nnAccount module expects a "nintendo" claim dictionary
    // with device metadata (dt, pc, di, sn, ist) -- without it, nnAccount's local schema
    // check rejects an otherwise well-formed, correctly-signed token even though the server
    // side has nothing to complain about. Matches Ryujinx-Nextendo's own fix for the exact
    // same shape of failure (their commit message: "the console receives HTTP 200 but
    // rejects the body because the JWT schema doesn't match what nnAccount expects").
    // Reuses the same device id for both the top-level "di" and the nested one, matching
    // Ryujinx-Nextendo's own token (a single deviceId feeding both places).
    const std::string device_id = RandomHex(0x10);
    const std::string nintendo_claim = fmt::format(
        R"("nintendo":{{"dt":"NX Prod 1","pc":"HAC","di":"{}","sn":"XAW10000000000","ist":false}},)",
        device_id);

    std::string tv_claim;
    if (!installed_version.empty()) {
        tv_claim = fmt::format(R"("tv":"{}",)", installed_version);
    }

    const std::string payload = fmt::format(
        R"({{"sub":"{}","aud":"{}","iss":"{}","typ":"id_token","iat":{},"exp":{},"jku":"{}",)"
        R"("jti":"{}","di":"{}","sn":"XAW10000000000","bs:did":"{}",{}{}{}"hm":true}})",
        RandomHex(0x10), BaasAudience, BaasIssuer, now, now + 3 * 60 * 60, BaasJku,
        Common::UUID::MakeRandom().FormattedString(), device_id, RandomHex(0x10),
        nintendo_claim, nnex_claim, tv_claim);

    const std::string signing_input =
        Base64UrlEncode(header) + "." + Base64UrlEncode(payload);

    return signing_input + "." + SignRs256(signing_input);
}

std::vector<u8> GetIdTokenBytes(Core::System& system) {
    static std::mutex mutex;
    static std::vector<u8> cached;
    static std::chrono::steady_clock::time_point expiry{};
    static u64 cached_generation = 0;
    static std::string cached_version;

    std::lock_guard lock{mutex};

    const u64 generation = Common::NextendoAccount::GetGeneration();
    const std::string installed_version = GetInstalledTitleVersion(system);
    const auto now = std::chrono::steady_clock::now();
    if (cached.empty() || now >= expiry || generation != cached_generation ||
        installed_version != cached_version) {
        const std::string token = BuildIdToken(installed_version);
        cached.assign(token.begin(), token.end());
        expiry = now + std::chrono::hours{2};
        cached_generation = generation;
        cached_version = installed_version;
        LOG_INFO(Service_ACC, "[Nextendo] Issued a signed BAAS id_token ({} bytes)", cached.size());
    }

    return cached;
}

} // Anonymous namespace

// Thumbnails are hard coded to be at least this size
constexpr std::size_t THUMBNAIL_SIZE = 0x24000;

static std::filesystem::path GetImagePath(const Common::UUID& uuid) {
    return Common::FS::GetCitronPath(Common::FS::CitronPath::NANDDir) /
           fmt::format("system/save/8000000000000010/su/avators/{}.jpg", uuid.FormattedString());
}

static void SeedImageFromNextendoIfMissing(const Common::UUID& uuid) {
    const auto path = GetImagePath(uuid);
    if (Common::FS::Exists(path)) {
        return;
    }
    if (!Common::NextendoAccount::IsLinked()) {
        return;
    }
    const auto jpeg = Common::NextendoAvatar::GetSelfJPEG();
    if (jpeg.empty()) {
        return;
    }
    void(Common::FS::CreateParentDirs(path));
    Common::FS::IOFile out(path, Common::FS::FileAccessMode::Write, Common::FS::FileType::BinaryFile);
    if (out.IsOpen()) {
        void(out.Write(jpeg));
    }
}

static void JPGToMemory(void* context, void* data, int len) {
    std::vector<u8>* jpg_image = static_cast<std::vector<u8>*>(context);
    unsigned char* jpg = static_cast<unsigned char*>(data);
    jpg_image->insert(jpg_image->end(), jpg, jpg + len);
}

static void SanitizeJPEGImageSize(std::vector<u8>& image) {
    constexpr std::size_t max_jpeg_image_size = 0x20000;
    constexpr int profile_dimensions = 256;
    int original_width, original_height, color_channels;

    const auto plain_image =
        stbi_load_from_memory(image.data(), static_cast<int>(image.size()), &original_width,
                              &original_height, &color_channels, STBI_rgb);

    // Resize image to match 256*256
    if (original_width != profile_dimensions || original_height != profile_dimensions) {
        // Use vector instead of array to avoid overflowing the stack
        std::vector<u8> out_image(profile_dimensions * profile_dimensions * STBI_rgb);
        stbir_resize_uint8_srgb(plain_image, original_width, original_height, 0, out_image.data(),
                                profile_dimensions, profile_dimensions, 0, STBI_rgb, 0,
                                STBIR_FILTER_BOX);
        image.clear();
        if (!stbi_write_jpg_to_func(JPGToMemory, &image, profile_dimensions, profile_dimensions,
                                    STBI_rgb, out_image.data(), 0)) {
            LOG_ERROR(Service_ACC, "Failed to resize the user provided image.");
        }
    }

    image.resize(std::min(image.size(), max_jpeg_image_size));
}

class IManagerForSystemService final : public ServiceFramework<IManagerForSystemService> {
public:
    explicit IManagerForSystemService(Core::System& system_, Common::UUID uuid)
        : ServiceFramework{system_, "IManagerForSystemService"}, account_id{uuid} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&IManagerForSystemService::CheckAvailability>, "CheckAvailability"},
            {1, D<&IManagerForSystemService::GetAccountId>, "GetAccountId"},
            {2, nullptr, "EnsureIdTokenCacheAsync"},
            {3, D<&IManagerForSystemService::LoadIdTokenCacheDeprecated>, "LoadIdTokenCacheDeprecated"}, // 19.0.0+
            {4, D<&IManagerForSystemService::LoadIdTokenCache>, "LoadIdTokenCache"}, // 19.0.0+
            {100, nullptr, "SetSystemProgramIdentification"},
            {101, nullptr, "RefreshNotificationTokenAsync"}, // 7.0.0+
            {110, nullptr, "GetServiceEntryRequirementCache"}, // 4.0.0+
            {111, nullptr, "InvalidateServiceEntryRequirementCache"}, // 4.0.0+
            {112, nullptr, "InvalidateTokenCache"}, // 4.0.0 - 6.2.0
            {113, nullptr, "GetServiceEntryRequirementCacheForOnlinePlay"}, // 6.1.0+
            {120, nullptr, "GetNintendoAccountId"},
            {121, nullptr, "CalculateNintendoAccountAuthenticationFingerprint"}, // 9.0.0+
            {130, &IManagerForSystemService::GetNintendoAccountUserResourceCache, "GetNintendoAccountUserResourceCache"},
            {131, nullptr, "RefreshNintendoAccountUserResourceCacheAsync"},
            {132, nullptr, "RefreshNintendoAccountUserResourceCacheAsyncIfSecondsElapsed"},
            {133, nullptr, "GetNintendoAccountVerificationUrlCache"}, // 9.0.0+
            {134, nullptr, "RefreshNintendoAccountVerificationUrlCache"}, // 9.0.0+
            {135, nullptr, "RefreshNintendoAccountVerificationUrlCacheAsyncIfSecondsElapsed"}, // 9.0.0+
            {136, &IManagerForSystemService::GetNintendoAccountUserResourceCache, "GetNintendoAccountUserResourceCache"}, // 19.0.0+
            {140, nullptr, "GetNetworkServiceLicenseCache"}, // 5.0.0+
            {141, nullptr, "RefreshNetworkServiceLicenseCacheAsync"}, // 5.0.0+
            {142, nullptr, "RefreshNetworkServiceLicenseCacheAsyncIfSecondsElapsed"}, // 5.0.0+
            {150, nullptr, "CreateAuthorizationRequest"},
            {160, nullptr, "RequiresUpdateNetworkServiceAccountIdTokenCache"},
            {161, nullptr, "RequireReauthenticationOfNetworkServiceAccount"},
            {143, D<&IManagerForSystemService::GetNetworkServiceLicenseCacheEx>, "GetNetworkServiceLicenseCacheEx"}, // 15.0.0+
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    Result CheckAvailability() {
        LOG_WARNING(Service_ACC, "(STUBBED) called");
        R_SUCCEED();
    }

    Result GetAccountId(Out<u64> out_account_id) {
        LOG_WARNING(Service_ACC, "(STUBBED) called");
        *out_account_id = account_id.Hash();
        R_SUCCEED();
    }

    Result LoadIdTokenCacheDeprecated() {
        LOG_WARNING(Service_ACC, "(STUBBED) called");
        R_SUCCEED();
    }

    Result LoadIdTokenCache() {
        LOG_WARNING(Service_ACC, "(STUBBED) called");
        R_SUCCEED();
    }

    void GetNintendoAccountUserResourceCache(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "[Nextendo] GetNintendoAccountUserResourceCache called");

        const u64 pid = account_id.Hash();
        std::vector<u8> nas_user_base(0x68, 0);
        std::memcpy(nas_user_base.data(), &pid, sizeof(pid));
        ctx.WriteBuffer(nas_user_base, 0);

        if (ctx.CanWriteBuffer(1)) {
            std::vector<u8> unknown_out_buffer(ctx.GetWriteBufferSize(1), 0);
            ctx.WriteBuffer(unknown_out_buffer, 1);
        }

        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.PushRaw<u64>(pid);
    }

    Result GetNetworkServiceLicenseCacheEx(Out<u32> out_license, Out<s64> out_expiration) {
        LOG_INFO(Service_ACC, "called");

        *out_license = 0;
        *out_expiration = 0;

        R_SUCCEED();
    }

    Common::UUID account_id;
};

// 3.0.0+
class IFloatingRegistrationRequest final : public ServiceFramework<IFloatingRegistrationRequest> {
public:
    explicit IFloatingRegistrationRequest(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "IFloatingRegistrationRequest"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetSessionId"},
            {12, nullptr, "GetAccountId"},
            {13, nullptr, "GetLinkedNintendoAccountId"},
            {14, nullptr, "GetNickname"},
            {15, nullptr, "GetProfileImage"},
            {21, nullptr, "LoadIdTokenCache"},
            {100, nullptr, "RegisterUser"}, // [1.0.0-3.0.2] RegisterAsync
            {101, nullptr, "RegisterUserWithUid"}, // [1.0.0-3.0.2] RegisterWithUidAsync
            {102, nullptr, "RegisterNetworkServiceAccountAsync"}, // 4.0.0+
            {103, nullptr, "RegisterNetworkServiceAccountWithUidAsync"}, // 4.0.0+
            {110, nullptr, "SetSystemProgramIdentification"},
            {111, nullptr, "EnsureIdTokenCacheAsync"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class IAdministrator final : public ServiceFramework<IAdministrator> {
public:
    explicit IAdministrator(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "IAdministrator"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "CheckAvailability"},
            {1, nullptr, "GetAccountId"},
            {2, nullptr, "EnsureIdTokenCacheAsync"},
            {3, nullptr, "LoadIdTokenCache"},
            {100, nullptr, "SetSystemProgramIdentification"},
            {101, nullptr, "RefreshNotificationTokenAsync"}, // 7.0.0+
            {110, nullptr, "GetServiceEntryRequirementCache"}, // 4.0.0+
            {111, nullptr, "InvalidateServiceEntryRequirementCache"}, // 4.0.0+
            {112, nullptr, "InvalidateTokenCache"}, // 4.0.0 - 6.2.0
            {113, nullptr, "GetServiceEntryRequirementCacheForOnlinePlay"}, // 6.1.0+
            {120, nullptr, "GetNintendoAccountId"},
            {121, nullptr, "CalculateNintendoAccountAuthenticationFingerprint"}, // 9.0.0+
            {130, nullptr, "GetNintendoAccountUserResourceCache"},
            {131, nullptr, "RefreshNintendoAccountUserResourceCacheAsync"},
            {132, nullptr, "RefreshNintendoAccountUserResourceCacheAsyncIfSecondsElapsed"},
            {133, nullptr, "GetNintendoAccountVerificationUrlCache"}, // 9.0.0+
            {134, nullptr, "RefreshNintendoAccountVerificationUrlCacheAsync"}, // 9.0.0+
            {135, nullptr, "RefreshNintendoAccountVerificationUrlCacheAsyncIfSecondsElapsed"}, // 9.0.0+
            {140, nullptr, "GetNetworkServiceLicenseCache"}, // 5.0.0+
            {141, nullptr, "RefreshNetworkServiceLicenseCacheAsync"}, // 5.0.0+
            {142, nullptr, "RefreshNetworkServiceLicenseCacheAsyncIfSecondsElapsed"}, // 5.0.0+
            {143, nullptr, "GetNetworkServiceLicenseCacheEx"},
            {150, nullptr, "CreateAuthorizationRequest"},
            {160, nullptr, "RequiresUpdateNetworkServiceAccountIdTokenCache"},
            {161, nullptr, "RequireReauthenticationOfNetworkServiceAccount"},
            {200, nullptr, "IsRegistered"},
            {201, nullptr, "RegisterAsync"},
            {202, nullptr, "UnregisterAsync"},
            {203, nullptr, "DeleteRegistrationInfoLocally"},
            {220, nullptr, "SynchronizeProfileAsync"},
            {221, nullptr, "UploadProfileAsync"},
            {222, nullptr, "SynchronizaProfileAsyncIfSecondsElapsed"},
            {250, nullptr, "IsLinkedWithNintendoAccount"},
            {251, nullptr, "CreateProcedureToLinkWithNintendoAccount"},
            {252, nullptr, "ResumeProcedureToLinkWithNintendoAccount"},
            {255, nullptr, "CreateProcedureToUpdateLinkageStateOfNintendoAccount"},
            {256, nullptr, "ResumeProcedureToUpdateLinkageStateOfNintendoAccount"},
            {260, nullptr, "CreateProcedureToLinkNnidWithNintendoAccount"}, // 3.0.0+
            {261, nullptr, "ResumeProcedureToLinkNnidWithNintendoAccount"}, // 3.0.0+
            {280, nullptr, "ProxyProcedureToAcquireApplicationAuthorizationForNintendoAccount"},
            {290, nullptr, "GetRequestForNintendoAccountUserResourceView"}, // 8.0.0+
            {300, nullptr, "TryRecoverNintendoAccountUserStateAsync"}, // 6.0.0+
            {400, nullptr, "IsServiceEntryRequirementCacheRefreshRequiredForOnlinePlay"}, // 6.1.0+
            {401, nullptr, "RefreshServiceEntryRequirementCacheForOnlinePlayAsync"}, // 6.1.0+
            {900, nullptr, "GetAuthenticationInfoForWin"}, // 9.0.0+
            {901, nullptr, "ImportAsyncForWin"}, // 9.0.0+
            {997, nullptr, "DebugUnlinkNintendoAccountAsync"},
            {998, nullptr, "DebugSetAvailabilityErrorDetail"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class IAuthorizationRequest final : public ServiceFramework<IAuthorizationRequest> {
public:
    explicit IAuthorizationRequest(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "IAuthorizationRequest"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetSessionId"},
            {10, nullptr, "InvokeWithoutInteractionAsync"},
            {19, nullptr, "IsAuthorized"},
            {20, nullptr, "GetAuthorizationCode"},
            {21, nullptr, "GetIdToken"},
            {22, nullptr, "GetState"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class IOAuthProcedure final : public ServiceFramework<IOAuthProcedure> {
public:
    explicit IOAuthProcedure(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "IOAuthProcedure"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "PrepareAsync"},
            {1, nullptr, "GetRequest"},
            {2, nullptr, "ApplyResponse"},
            {3, nullptr, "ApplyResponseAsync"},
            {10, nullptr, "Suspend"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

// 3.0.0+
class IOAuthProcedureForExternalNsa final : public ServiceFramework<IOAuthProcedureForExternalNsa> {
public:
    explicit IOAuthProcedureForExternalNsa(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "IOAuthProcedureForExternalNsa"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "PrepareAsync"},
            {1, nullptr, "GetRequest"},
            {2, nullptr, "ApplyResponse"},
            {3, nullptr, "ApplyResponseAsync"},
            {10, nullptr, "Suspend"},
            {100, nullptr, "GetAccountId"},
            {101, nullptr, "GetLinkedNintendoAccountId"},
            {102, nullptr, "GetNickname"},
            {103, nullptr, "GetProfileImage"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class IOAuthProcedureForNintendoAccountLinkage final
    : public ServiceFramework<IOAuthProcedureForNintendoAccountLinkage> {
public:
    explicit IOAuthProcedureForNintendoAccountLinkage(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "IOAuthProcedureForNintendoAccountLinkage"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "PrepareAsync"},
            {1, nullptr, "GetRequest"},
            {2, nullptr, "ApplyResponse"},
            {3, nullptr, "ApplyResponseAsync"},
            {10, nullptr, "Suspend"},
            {100, nullptr, "GetRequestWithTheme"},
            {101, nullptr, "IsNetworkServiceAccountReplaced"},
            {199, nullptr, "GetUrlForIntroductionOfExtraMembership"}, // 2.0.0 - 5.1.0
            {200, nullptr, "ApplyAsyncWithAuthorizedToken"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class INotifier final : public ServiceFramework<INotifier> {
public:
    explicit INotifier(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "INotifier"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetSystemEvent"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class IProfileCommon : public ServiceFramework<IProfileCommon> {
public:
    explicit IProfileCommon(Core::System& system_, const char* name, bool editor_commands,
                            Common::UUID user_id_, ProfileManager& profile_manager_)
        : ServiceFramework{system_, name}, profile_manager{profile_manager_}, user_id{user_id_} {
        static const FunctionInfo functions[] = {
            {0, &IProfileCommon::Get, "Get"},
            {1, &IProfileCommon::GetBase, "GetBase"},
            {10, &IProfileCommon::GetImageSize, "GetImageSize"},
            {11, &IProfileCommon::LoadImage, "LoadImage"},
            {20, &IProfileCommon::Unknown20, "Unknown20"},
            {21, &IProfileCommon::Unknown21, "Unknown21"},
            {30, &IProfileCommon::Unknown30, "Unknown30"},
        };

        RegisterHandlers(functions);

        if (editor_commands) {
            static const FunctionInfo editor_functions[] = {
                {100, &IProfileCommon::Store, "Store"},
                {101, &IProfileCommon::StoreWithImage, "StoreWithImage"},
                {110, &IProfileCommon::Unknown110, "Unknown110"},
            };

            RegisterHandlers(editor_functions);
        }
    }

protected:
    void Get(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called user_id=0x{}", user_id.RawString());
        ProfileBase profile_base{};
        UserData data{};
        if (profile_manager.GetProfileBaseAndData(user_id, profile_base, data)) {
            ctx.WriteBuffer(data);
            IPC::ResponseBuilder rb{ctx, 16};
            rb.Push(ResultSuccess);
            rb.PushRaw(profile_base);
        } else {
            LOG_ERROR(Service_ACC, "Failed to get profile base and data for user=0x{}",
                      user_id.RawString());
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultUnknown); // TODO(ogniK): Get actual error code
        }
    }

    void GetBase(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called user_id=0x{}", user_id.RawString());
        ProfileBase profile_base{};
        if (profile_manager.GetProfileBase(user_id, profile_base)) {
            IPC::ResponseBuilder rb{ctx, 16};
            rb.Push(ResultSuccess);
            rb.PushRaw(profile_base);
        } else {
            LOG_ERROR(Service_ACC, "Failed to get profile base for user=0x{}", user_id.RawString());
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultUnknown); // TODO(ogniK): Get actual error code
        }
    }

    void LoadImage(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);

        SeedImageFromNextendoIfMissing(user_id);

        const Common::FS::IOFile image(GetImagePath(user_id), Common::FS::FileAccessMode::Read,
                                       Common::FS::FileType::BinaryFile);
        if (!image.IsOpen()) {
            LOG_WARNING(Service_ACC,
                        "Failed to load user provided image! Falling back to built-in backup...");
            ctx.WriteBuffer(Core::Constants::ACCOUNT_BACKUP_JPEG);
            rb.Push(static_cast<u32>(Core::Constants::ACCOUNT_BACKUP_JPEG.size()));
            return;
        }

        std::vector<u8> buffer(image.GetSize());

        if (image.Read(buffer) != buffer.size()) {
            LOG_ERROR(Service_ACC, "Failed to read all the bytes in the user provided image.");
        }

        SanitizeJPEGImageSize(buffer);

        ctx.WriteBuffer(buffer);
        rb.Push(static_cast<u32>(buffer.size()));
    }

    void GetImageSize(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);

        SeedImageFromNextendoIfMissing(user_id);

        const Common::FS::IOFile image(GetImagePath(user_id), Common::FS::FileAccessMode::Read,
                                       Common::FS::FileType::BinaryFile);

        if (!image.IsOpen()) {
            LOG_WARNING(Service_ACC,
                        "Failed to load user provided image! Falling back to built-in backup...");
            rb.Push(static_cast<u32>(Core::Constants::ACCOUNT_BACKUP_JPEG.size()));
            return;
        }

        std::vector<u8> buffer(image.GetSize());

        if (image.Read(buffer) != buffer.size()) {
            LOG_ERROR(Service_ACC, "Failed to read all the bytes in the user provided image.");
        }

        SanitizeJPEGImageSize(buffer);
        rb.Push(static_cast<u32>(buffer.size()));
    }

    void Unknown20(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "(STUBBED) called.");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void Unknown21(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "(STUBBED) called.");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void Unknown30(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "(STUBBED) called.");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void Unknown110(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "(STUBBED) called.");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void Store(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto base = rp.PopRaw<ProfileBase>();

        const auto user_data = ctx.ReadBuffer();

        LOG_DEBUG(Service_ACC, "called, username='{}', timestamp={:016X}, uuid=0x{}",
                  Common::StringFromFixedZeroTerminatedBuffer(
                      reinterpret_cast<const char*>(base.username.data()), base.username.size()),
                  base.timestamp, base.user_uuid.RawString());

        if (user_data.size() < sizeof(UserData)) {
            LOG_ERROR(Service_ACC, "UserData buffer too small!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(Account::ResultInvalidArrayLength);
            return;
        }

        UserData data;
        std::memcpy(&data, user_data.data(), sizeof(UserData));

        if (!profile_manager.SetProfileBaseAndData(user_id, base, data)) {
            LOG_ERROR(Service_ACC, "Failed to update user data and base!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(Account::ResultAccountUpdateFailed);
            return;
        }

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void StoreWithImage(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto base = rp.PopRaw<ProfileBase>();

        const auto image_data = ctx.ReadBufferA(0);
        const auto user_data = ctx.ReadBufferX(0);

        LOG_INFO(Service_ACC, "called, username='{}', timestamp={:016X}, uuid=0x{}",
                 Common::StringFromFixedZeroTerminatedBuffer(
                     reinterpret_cast<const char*>(base.username.data()), base.username.size()),
                 base.timestamp, base.user_uuid.RawString());

        if (user_data.size() < sizeof(UserData)) {
            LOG_ERROR(Service_ACC, "UserData buffer too small!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(Account::ResultInvalidArrayLength);
            return;
        }

        UserData data;
        std::memcpy(&data, user_data.data(), sizeof(UserData));

        Common::FS::IOFile image(GetImagePath(user_id), Common::FS::FileAccessMode::Write,
                                 Common::FS::FileType::BinaryFile);

        if (!image.IsOpen() || !image.SetSize(image_data.size()) ||
            image.Write(image_data) != image_data.size() ||
            !profile_manager.SetProfileBaseAndData(user_id, base, data)) {
            LOG_ERROR(Service_ACC, "Failed to update profile data, base, and image!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(Account::ResultAccountUpdateFailed);
            return;
        }

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    ProfileManager& profile_manager;
    Common::UUID user_id{}; ///< The user id this profile refers to.
};

class IProfile final : public IProfileCommon {
public:
    explicit IProfile(Core::System& system_, Common::UUID user_id_,
                      ProfileManager& profile_manager_)
        : IProfileCommon{system_, "IProfile", false, user_id_, profile_manager_} {}
};

class IProfileEditor final : public IProfileCommon {
public:
    explicit IProfileEditor(Core::System& system_, Common::UUID user_id_,
                            ProfileManager& profile_manager_)
        : IProfileCommon{system_, "IProfileEditor", true, user_id_, profile_manager_} {}
};

class ISessionObject final : public ServiceFramework<ISessionObject> {
public:
    explicit ISessionObject(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "ISessionObject"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {999, nullptr, "Dummy"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class IGuestLoginRequest final : public ServiceFramework<IGuestLoginRequest> {
public:
    explicit IGuestLoginRequest(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "IGuestLoginRequest"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetSessionId"},
            {11, nullptr, "Unknown"}, // 1.0.0 - 2.3.0 (the name is blank on Switchbrew)
            {12, nullptr, "GetAccountId"},
            {13, nullptr, "GetLinkedNintendoAccountId"},
            {14, nullptr, "GetNickname"},
            {15, nullptr, "GetProfileImage"},
            {21, nullptr, "LoadIdTokenCache"}, // 3.0.0+
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class EnsureTokenIdCacheAsyncInterface final : public IAsyncContext {
public:
    explicit EnsureTokenIdCacheAsyncInterface(Core::System& system_) : IAsyncContext{system_} {
        MarkComplete();
    }
    ~EnsureTokenIdCacheAsyncInterface() = default;

    void LoadIdTokenCache(HLERequestContext& ctx) {
        const std::vector<u8> token_bytes = GetIdTokenBytes(system);
        LOG_INFO(Service_ACC, "[Nextendo] Providing BAAS ID token in async interface ({} bytes)",
                 token_bytes.size());

        ctx.WriteBuffer(token_bytes);

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(static_cast<u32>(token_bytes.size()));
    }

protected:
    bool IsComplete() const override {
        return true;
    }

    void Cancel() override {}

    Result GetResult() const override {
        return ResultSuccess;
    }
};

class AuthenticateApplicationAsyncInterface final : public IAsyncContext {
public:
    explicit AuthenticateApplicationAsyncInterface(Core::System& system_) : IAsyncContext{system_} {
        MarkComplete();
    }
    ~AuthenticateApplicationAsyncInterface() = default;

protected:
    bool IsComplete() const override {
        return true;
    }

    void Cancel() override {}

    Result GetResult() const override {
        return ResultSuccess;
    }
};

class CheckNetworkServiceAvailabilityAsyncInterface final : public IAsyncContext {
public:
    explicit CheckNetworkServiceAvailabilityAsyncInterface(Core::System& system_)
        : IAsyncContext{system_} {
        MarkComplete();
    }
    ~CheckNetworkServiceAvailabilityAsyncInterface() = default;

protected:
    bool IsComplete() const override {
        return true;
    }

    void Cancel() override {}

    Result GetResult() const override {
        return ResultSuccess;
    }
};

class EnsureSignedDeviceIdentifierCacheAsyncInterface final : public IAsyncContext {
public:
    explicit EnsureSignedDeviceIdentifierCacheAsyncInterface(Core::System& system_)
        : IAsyncContext{system_} {
        MarkComplete();
    }
    ~EnsureSignedDeviceIdentifierCacheAsyncInterface() = default;

protected:
    bool IsComplete() const override {
        return true;
    }

    void Cancel() override {}

    Result GetResult() const override {
        return ResultSuccess;
    }
};

class AuthenticateServiceAsyncInterface final : public IAsyncContext {
public:
    explicit AuthenticateServiceAsyncInterface(Core::System& system_) : IAsyncContext{system_} {
        MarkComplete();
    }
    ~AuthenticateServiceAsyncInterface() = default;

protected:
    bool IsComplete() const override {
        return true;
    }

    void Cancel() override {}

    Result GetResult() const override {
        return ResultSuccess;
    }
};

class SynchronizeNetworkServiceAccountsSnapshotAsyncInterface final : public IAsyncContext {
public:
    explicit SynchronizeNetworkServiceAccountsSnapshotAsyncInterface(Core::System& system_)
        : IAsyncContext{system_} {
        MarkComplete();
    }
    ~SynchronizeNetworkServiceAccountsSnapshotAsyncInterface() = default;

protected:
    bool IsComplete() const override {
        return true;
    }

    void Cancel() override {}

    Result GetResult() const override {
        return ResultSuccess;
    }
};

// 6.0.0+
class IAsyncNetworkServiceLicenseKindContext final
    : public ServiceFramework<IAsyncNetworkServiceLicenseKindContext> {
public:
    explicit IAsyncNetworkServiceLicenseKindContext(Core::System& system_)
        : ServiceFramework{system_, "IAsyncNetworkServiceLicenseKindContext"},
          service_context{system_, "IAsyncNetworkServiceLicenseKindContext"} {
        static const FunctionInfo functions[] = {
            {0, &IAsyncNetworkServiceLicenseKindContext::GetSystemEvent, "GetSystemEvent"},
            {1, &IAsyncNetworkServiceLicenseKindContext::Cancel, "Cancel"},
            {2, &IAsyncNetworkServiceLicenseKindContext::HasDone, "HasDone"},
            {3, &IAsyncNetworkServiceLicenseKindContext::GetResult, "GetResult"},
            {4, &IAsyncNetworkServiceLicenseKindContext::GetNetworkServiceLicenseKind, "GetNetworkServiceLicenseKind"},
        };
        RegisterHandlers(functions);
        completion_event = service_context.CreateEvent("IAsyncNetworkServiceLicenseKindContext:CompletionEvent");
        completion_event->Signal();
    }

private:
    void GetSystemEvent(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(completion_event->GetReadableEvent());
    }

    void Cancel(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void HasDone(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(true);
    }

    void GetResult(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetNetworkServiceLicenseKind(HLERequestContext& ctx) {
        LOG_INFO(Service_ACC, "[Nextendo] GetNetworkServiceLicenseKind called -> returning Subscribed (2)");
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(2);
    }

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* completion_event;
};

class IManagerForApplication final : public ServiceFramework<IManagerForApplication> {
public:
    explicit IManagerForApplication(Core::System& system_,
                                    const std::shared_ptr<ProfileManager>& profile_manager_)
        : ServiceFramework{system_, "IManagerForApplication"},
          ensure_token_id{std::make_shared<EnsureTokenIdCacheAsyncInterface>(system)},
          profile_manager{profile_manager_} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IManagerForApplication::CheckAvailability, "CheckAvailability"},
            {1, &IManagerForApplication::GetAccountId, "GetAccountId"},
            {2, &IManagerForApplication::EnsureIdTokenCacheAsync, "EnsureIdTokenCacheAsync"},
            {3, &IManagerForApplication::LoadIdTokenCacheDeprecated, "LoadIdTokenCacheDeprecated"},
            {4, &IManagerForApplication::LoadIdTokenCache, "LoadIdTokenCache"},
            {130, &IManagerForApplication::GetNintendoAccountUserResourceCacheForApplication, "GetNintendoAccountUserResourceCacheForApplication"},
            {136, &IManagerForApplication::GetNintendoAccountUserResourceCacheForApplication, "GetNintendoAccountUserResourceCache"}, // 19.0.0+
            {150, nullptr, "CreateAuthorizationRequest"},
            {160, &IManagerForApplication::StoreOpenContext, "StoreOpenContext"},
            {170, &IManagerForApplication::LoadNetworkServiceLicenseKindAsync, "LoadNetworkServiceLicenseKindAsync"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    u64 GetEffectivePid() const {
        const u64 raw_pid = []() -> u64 {
            if (const u64 linked = Common::NextendoAccount::GetPid(); linked != 0) {
                return linked;
            }
            std::string pid_setting = Settings::values.nextendo_pid.GetValue();
            if (!pid_setting.empty()) {
                try {
                    return std::stoull(pid_setting);
                } catch (...) {}
            }
            const char* pid_env = std::getenv("NEXTENDO_PID");
            if (pid_env && *pid_env) {
                try {
                    return std::stoull(pid_env);
                } catch (...) {}
            }
            return 0;
        }();

        if (raw_pid == 0) {
            return 0xcafe;
        }

        // [Nextendo] Version gate: refuse to present a real PID for a title whose installed
        // version doesn't match what Nextendo's servers require, reusing the exact same
        // server-side refusal as "no account linked" (0xcafe is not a real registered
        // account, so the gated server rejects it) rather than inventing a new failure mode.
        // Keeps outdated clients off the servers entirely -- reduces load from clients that
        // can't speak the wire format the server expects, and keeps the playerbase from
        // fragmenting across versions.
        const u64 program_id = system.GetApplicationProcessProgramID();
        if (Nextendo::CompatibleTitles::Table().contains(program_id)) {
            const std::string installed_version = GetInstalledTitleVersion(system);
            if (!Nextendo::CompatibleTitles::IsVersionOk(program_id, installed_version)) {
                LOG_WARNING(Service_ACC,
                            "[Nextendo] Refusing online PID: installed version doesn't match "
                            "what this title requires for online play");
                return 0xcafe;
            }
        }

        return raw_pid;
    }

    void CheckAvailability(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetAccountId(HLERequestContext& ctx) {
        u64 account_id = GetEffectivePid();
        // The PID is accepted as a bare identity by the online service, so never log its value.
        LOG_DEBUG(Service_ACC, "[Nextendo] Returning the linked account's Network ID");

        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.PushRaw<u64>(account_id);
    }

    void EnsureIdTokenCacheAsync(HLERequestContext& ctx) {
        LOG_WARNING(Service_ACC, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface(ensure_token_id);
    }

    void LoadIdTokenCacheDeprecated(HLERequestContext& ctx) {
        LOG_WARNING(Service_ACC, "(STUBBED) called");

        ensure_token_id->LoadIdTokenCache(ctx);
    }

    void LoadIdTokenCache(HLERequestContext& ctx) {
        const std::vector<u8> token_bytes = GetIdTokenBytes(system);
        LOG_INFO(Service_ACC, "[Nextendo] Providing BAAS ID token ({} bytes)", token_bytes.size());

        ctx.WriteBuffer(token_bytes);

        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<u32>(static_cast<u32>(token_bytes.size()));
        rb.Push(0);
    }

    void GetNintendoAccountUserResourceCacheForApplication(HLERequestContext& ctx) {
        u64 account_id = GetEffectivePid();
        LOG_DEBUG(Service_ACC, "[Nextendo] GetNintendoAccountUserResourceCacheForApplication called");

        std::vector<u8> nas_user_base_for_application(0x68, 0);
        std::memcpy(nas_user_base_for_application.data(), &account_id, sizeof(account_id));
        ctx.WriteBuffer(nas_user_base_for_application, 0);

        if (ctx.CanWriteBuffer(1)) {
            std::vector<u8> unknown_out_buffer(ctx.GetWriteBufferSize(1), 0);
            ctx.WriteBuffer(unknown_out_buffer, 1);
        }

        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.PushRaw<u64>(account_id);
    }

    void StoreOpenContext(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");

        profile_manager->StoreOpenedUsers();

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void LoadNetworkServiceLicenseKindAsync(HLERequestContext& ctx) {
        LOG_INFO(Service_ACC, "[Nextendo] LoadNetworkServiceLicenseKindAsync called");

        auto async_context = std::make_shared<IAsyncNetworkServiceLicenseKindContext>(system);

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface(async_context);
    }

    std::shared_ptr<EnsureTokenIdCacheAsyncInterface> ensure_token_id{};
    std::shared_ptr<ProfileManager> profile_manager;
};

// 8.0.0+
class IOAuthProcedureForUserRegistration final
    : public ServiceFramework<IOAuthProcedureForUserRegistration> {
public:
    explicit IOAuthProcedureForUserRegistration(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "IOAuthProcedureForUserRegistration"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "PrepareAsync"},
            {1, nullptr, "GetRequest"},
            {2, nullptr, "ApplyResponse"},
            {3, nullptr, "ApplyResponseAsync"},
            {10, nullptr, "Suspend"},
            {100, nullptr, "GetAccountId"},
            {101, nullptr, "GetLinkedNintendoAccountId"},
            {102, nullptr, "GetNickname"},
            {103, nullptr, "GetProfileImage"},
            {110, nullptr, "RegisterUserAsync"},
            {111, nullptr, "GetUid"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class DAUTH_O final : public ServiceFramework<DAUTH_O> {
public:
    explicit DAUTH_O(Core::System& system_, Common::UUID) : ServiceFramework{system_, "dauth:o"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "EnsureAuthenticationTokenCacheAsync"},
            {1, nullptr, "LoadAuthenticationTokenCache"},
            {2, nullptr, "InvalidateAuthenticationTokenCache"},
            {3, nullptr, "IsDeviceAuthenticationTokenCacheAvailable"},
            {10, nullptr, "EnsureEdgeTokenCacheAsync"},
            {11, nullptr, "LoadEdgeTokenCache"},
            {12, nullptr, "InvalidateEdgeTokenCache"},
            {13, nullptr, "IsEdgeTokenCacheAvailable"},
            {20, nullptr, "EnsureApplicationAuthenticationCacheAsync"},
            {21, nullptr, "LoadApplicationAuthenticationTokenCache"},
            {22, nullptr, "LoadApplicationNetworkServiceClientConfigCache"},
            {23, nullptr, "IsApplicationAuthenticationCacheAvailable"},
            {24, nullptr, "InvalidateApplicationAuthenticationCache"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

// 6.0.0+
class IAsyncResult final : public ServiceFramework<IAsyncResult> {
public:
    explicit IAsyncResult(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "IAsyncResult"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetResult"},
            {1, nullptr, "Cancel"},
            {2, nullptr, "IsAvailable"},
            {3, nullptr, "GetSystemEvent"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

void Module::Interface::GetUserCount(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called, user_count={}", profile_manager->GetUserCount());
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(static_cast<u32>(profile_manager->GetUserCount()));
}

void Module::Interface::GetUserExistence(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    Common::UUID user_id = rp.PopRaw<Common::UUID>();
    const bool exists = profile_manager->UserExists(user_id);
    LOG_DEBUG(Service_ACC, "called user_id=0x{}, exists={}", user_id.RawString(), exists);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(exists);
}

void Module::Interface::ListAllUsers(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called, user_count={}", profile_manager->GetUserCount());
    ctx.WriteBuffer(profile_manager->GetAllUsers());
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::ListOpenUsers(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");
    ctx.WriteBuffer(profile_manager->GetOpenUsers());
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::GetLastOpenedUser(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called, uuid=0x{}", profile_manager->GetLastOpenedUser().RawString());
    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw<Common::UUID>(profile_manager->GetLastOpenedUser());
}

void Module::Interface::GetProfile(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    Common::UUID user_id = rp.PopRaw<Common::UUID>();
    LOG_DEBUG(Service_ACC, "called user_id=0x{}", user_id.RawString());

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IProfile>(system, user_id, *profile_manager);
}

void Module::Interface::GetProfileDigest(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    Common::UUID user_id = rp.PopRaw<Common::UUID>();
    LOG_DEBUG(Service_ACC, "called user_id=0x{}", user_id.RawString());

    // Return a dummy digest for now
    std::array<u8, 0x20> digest{};
    std::fill(digest.begin(), digest.end(), static_cast<u8>(0));

    ctx.WriteBuffer(digest);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::IsUserRegistrationRequestPermitted(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(profile_manager->CanSystemRegisterUser());
}

void Module::Interface::InitializeApplicationInfo(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(InitializeApplicationInfoBase());
}

void Module::Interface::InitializeApplicationInfoRestricted(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(Partial implementation) called");

    // TODO(ogniK): We require checking if the user actually owns the title and what not. As of
    // currently, we assume the user owns the title. InitializeApplicationInfoBase SHOULD be called
    // first then we do extra checks if the game is a digital copy.

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(InitializeApplicationInfoBase());
}

Result Module::Interface::InitializeApplicationInfoBase() {
    if (application_info) {
        LOG_ERROR(Service_ACC, "Application already initialized");
        return Account::ResultApplicationInfoAlreadyInitialized;
    }

    // TODO(ogniK): This should be changed to reflect the target process for when we have multiple
    // processes emulated. As we don't actually have pid support we should assume we're just using
    // our own process
    Glue::ApplicationLaunchProperty launch_property{};
    const auto result = system.GetARPManager().GetLaunchProperty(
        &launch_property, system.GetApplicationProcessProgramID());

    if (result != ResultSuccess) {
        LOG_ERROR(Service_ACC, "Failed to get launch property");
        return Account::ResultInvalidApplication;
    }

    switch (launch_property.base_game_storage_id) {
    case FileSys::StorageId::GameCard:
        application_info.application_type = ApplicationType::GameCard;
        break;
    case FileSys::StorageId::Host:
    case FileSys::StorageId::NandSystem:
    case FileSys::StorageId::NandUser:
    case FileSys::StorageId::SdCard:
    case FileSys::StorageId::None: // Citron specific, differs from hardware
        application_info.application_type = ApplicationType::Digital;
        break;
    default:
        LOG_ERROR(Service_ACC, "Invalid game storage ID! storage_id={}",
                  launch_property.base_game_storage_id);
        return Account::ResultInvalidApplication;
    }

    LOG_WARNING(Service_ACC, "ApplicationInfo init required");
    // TODO(ogniK): Actual initialization here

    return ResultSuccess;
}

void Module::Interface::GetBaasAccountManagerForApplication(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IManagerForApplication>(system, profile_manager);
}

void Module::Interface::AuthenticateApplicationAsync(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<AuthenticateApplicationAsyncInterface>(system);
}

void Module::Interface::CheckNetworkServiceAvailabilityAsync(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<CheckNetworkServiceAvailabilityAsyncInterface>(system);
}

void Module::Interface::IsUserAccountSwitchLocked(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");
    FileSys::NACP nacp;
    const auto res = system.GetAppLoader().ReadControlData(nacp);

    bool is_locked = false;

    if (res != Loader::ResultStatus::Success) {
        const FileSys::PatchManager pm{system.GetApplicationProcessProgramID(),
                                       system.GetFileSystemController(),
                                       system.GetContentProvider()};
        const auto nacp_unique = pm.GetControlMetadata().first;

        if (nacp_unique != nullptr) {
            is_locked = nacp_unique->GetUserAccountSwitchLock();
        } else {
            LOG_ERROR(Service_ACC, "nacp_unique is null!");
        }
    } else {
        is_locked = nacp.GetUserAccountSwitchLock();
    }

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(is_locked);
}

void Module::Interface::InitializeApplicationInfoV2(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::BeginUserRegistration(HLERequestContext& ctx) {
    const auto user_id = Common::UUID::MakeRandom();
    profile_manager->CreateNewUser(user_id, "citron");

    LOG_INFO(Service_ACC, "called, uuid={}", user_id.FormattedString());

    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(user_id);
}

void Module::Interface::CompleteUserRegistration(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    Common::UUID user_id = rp.PopRaw<Common::UUID>();

    LOG_INFO(Service_ACC, "called, uuid={}", user_id.FormattedString());

    profile_manager->WriteUserSaveFile();

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::GetProfileEditor(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    Common::UUID user_id = rp.PopRaw<Common::UUID>();

    LOG_DEBUG(Service_ACC, "called, user_id=0x{}", user_id.RawString());

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IProfileEditor>(system, user_id, *profile_manager);
}

void Module::Interface::ListQualifiedUsers(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");

    // All users should be qualified. We don't actually have parental control or anything to do with
    // nintendo online currently. We're just going to assume the user running the game has access to
    // the game regardless of parental control settings.
    ctx.WriteBuffer(profile_manager->GetAllUsers());
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::ListOpenContextStoredUsers(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");

    ctx.WriteBuffer(profile_manager->GetStoredOpenedUsers());
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::StoreSaveDataThumbnailApplication(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();

    LOG_WARNING(Service_ACC, "(STUBBED) called, uuid=0x{}", uuid.RawString());

    // TODO(ogniK): Check if application ID is zero on acc initialize. As we don't have a reliable
    // way of confirming things like the TID, we're going to assume a non zero value for the time
    // being.
    constexpr u64 tid{1};
    StoreSaveDataThumbnail(ctx, uuid, tid);
}

void Module::Interface::ClearSaveDataThumbnail(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::CreateGuestLoginRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    // Create a dummy UUID for the guest login request
    const Common::UUID dummy_uuid{};

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IGuestLoginRequest>(system, dummy_uuid);
}

void Module::Interface::LoadOpenContext(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::DebugActivateOpenContextRetention(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    // Create a dummy UUID for the session object
    const Common::UUID dummy_uuid{};

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<ISessionObject>(system, dummy_uuid);
}

void Module::Interface::GetBaasAccountManagerForSystemService(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();

    LOG_INFO(Service_ACC, "called, uuid=0x{}", uuid.RawString());

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IManagerForSystemService>(system, uuid);
}

void Module::Interface::StoreSaveDataThumbnailSystem(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();
    const auto tid = rp.Pop<u64_le>();

    LOG_WARNING(Service_ACC, "(STUBBED) called, uuid=0x{}, tid={:016X}", uuid.RawString(), tid);
    StoreSaveDataThumbnail(ctx, uuid, tid);
}

void Module::Interface::GetPinCodeLength(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0);
}

void Module::Interface::StoreSaveDataThumbnail(HLERequestContext& ctx, const Common::UUID& uuid,
                                               const u64 tid) {
    IPC::ResponseBuilder rb{ctx, 2};

    if (tid == 0) {
        LOG_ERROR(Service_ACC, "TitleID is not valid!");
        rb.Push(Account::ResultInvalidApplication);
        return;
    }

    if (uuid.IsInvalid()) {
        LOG_ERROR(Service_ACC, "User ID is not valid!");
        rb.Push(Account::ResultInvalidUserId);
        return;
    }
    const auto thumbnail_size = ctx.GetReadBufferSize();
    if (thumbnail_size != THUMBNAIL_SIZE) {
        LOG_ERROR(Service_ACC, "Buffer size is empty! size={:X} expecting {:X}", thumbnail_size,
                  THUMBNAIL_SIZE);
        rb.Push(Account::ResultInvalidArrayLength);
        return;
    }

    // TODO(ogniK): Construct save data thumbnail
    rb.Push(ResultSuccess);
}

void Module::Interface::TrySelectUserWithoutInteraction(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto is_network_service_account_required = rp.Pop<bool>();
    LOG_DEBUG(Service_ACC, "called, is_network_service_account_required={}, user_count={}",
             is_network_service_account_required, profile_manager->GetUserCount());
    IPC::ResponseBuilder rb{ctx, 6};
    if (profile_manager->GetUserCount() != 1) {
        LOG_DEBUG(Service_ACC, "-> ResultSuccess w/ InvalidUUID (user_count != 1)");
        rb.Push(ResultSuccess);
        rb.PushRaw(Common::InvalidUUID);
        return;
    }

    const auto user_list = profile_manager->GetAllUsers();
    if (std::ranges::all_of(user_list, [](const auto& user) { return user.IsInvalid(); })) {
        LOG_DEBUG(Service_ACC, "-> ResultUnknown (all users invalid)");
        rb.Push(ResultUnknown); // TODO(ogniK): Find the correct error code
        rb.PushRaw(Common::InvalidUUID);
        return;
    }

    // Select the first user we have
    LOG_DEBUG(Service_ACC, "-> ResultSuccess uuid=0x{}", profile_manager->GetUser(0)->RawString());
    rb.Push(ResultSuccess);
    rb.PushRaw(profile_manager->GetUser(0)->uuid);
}

void Module::Interface::GetUserRegistrationNotifier(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<INotifier>(system, dummy_uuid);
}

void Module::Interface::GetUserStateChangeNotifier(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<INotifier>(system, dummy_uuid);
}

void Module::Interface::GetBaasUserAvailabilityChangeNotifier(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<INotifier>(system, dummy_uuid);
}

void Module::Interface::GetProfileUpdateNotifier(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<INotifier>(system, dummy_uuid);
}

void Module::Interface::GetProfileSyncNotifier(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<INotifier>(system, dummy_uuid);
}

void Module::Interface::LoadSaveDataThumbnail(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();
    const auto tid = rp.Pop<u64_le>();

    LOG_WARNING(Service_ACC, "(STUBBED) called, uuid=0x{}, tid={:016X}", uuid.RawString(), tid);

    // Return empty buffer for now
    std::vector<u8> thumbnail(THUMBNAIL_SIZE);
    ctx.WriteBuffer(thumbnail);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::GetSaveDataThumbnailExistence(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();
    const auto tid = rp.Pop<u64_le>();

    LOG_WARNING(Service_ACC, "(STUBBED) called, uuid=0x{}, tid={:016X}", uuid.RawString(), tid);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(false); // Thumbnail does not exist
}

void Module::Interface::ListOpenUsersInApplication(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    ctx.WriteBuffer(profile_manager->GetOpenUsers());
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::ActivateOpenContextRetention(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<ISessionObject>(system, dummy_uuid);
}

void Module::Interface::EnsureSignedDeviceIdentifierCacheForNintendoAccountAsync(
    HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<EnsureSignedDeviceIdentifierCacheAsyncInterface>(system);
}

void Module::Interface::LoadSignedDeviceIdentifierCacheForNintendoAccount(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    // Return dummy data
    std::array<u8, 0x40> device_identifier{};
    ctx.WriteBuffer(device_identifier);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::GetUserLastOpenedApplication(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    Common::UUID user_id = rp.PopRaw<Common::UUID>();

    LOG_WARNING(Service_ACC, "(STUBBED) called, user_id=0x{}", user_id.RawString());

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<u64>(0); // No application opened
}

void Module::Interface::ActivateOpenContextHolder(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::CancelUserRegistration(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    Common::UUID user_id = rp.PopRaw<Common::UUID>();

    LOG_INFO(Service_ACC, "called, uuid={}", user_id.FormattedString());

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::DeleteUser(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    Common::UUID user_id = rp.PopRaw<Common::UUID>();

    LOG_INFO(Service_ACC, "called, uuid={}", user_id.FormattedString());

    profile_manager->RemoveUser(user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::SetUserPosition(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto position = rp.Pop<u32>();
    const auto uuid = rp.PopRaw<Common::UUID>();

    LOG_WARNING(Service_ACC, "(STUBBED) called, position={}, uuid=0x{}", position,
                uuid.RawString());

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::CompleteUserRegistrationForcibly(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    Common::UUID user_id = rp.PopRaw<Common::UUID>();

    LOG_INFO(Service_ACC, "called, uuid={}", user_id.FormattedString());

    profile_manager->WriteUserSaveFile();

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::CreateFloatingRegistrationRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IFloatingRegistrationRequest>(system, dummy_uuid);
}

void Module::Interface::CreateProcedureToRegisterUserWithNintendoAccount(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IOAuthProcedureForUserRegistration>(system, dummy_uuid);
}

void Module::Interface::ResumeProcedureToRegisterUserWithNintendoAccount(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IOAuthProcedureForUserRegistration>(system, dummy_uuid);
}

void Module::Interface::CreateProcedureToCreateUserWithNintendoAccount(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IOAuthProcedureForUserRegistration>(system, dummy_uuid);
}

void Module::Interface::ResumeProcedureToCreateUserWithNintendoAccount(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IOAuthProcedureForUserRegistration>(system, dummy_uuid);
}

void Module::Interface::ResumeProcedureToCreateUserWithNintendoAccountAfterApplyResponse(
    HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IOAuthProcedureForUserRegistration>(system, dummy_uuid);
}

void Module::Interface::AuthenticateServiceAsync(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<AuthenticateServiceAsyncInterface>(system);
}

void Module::Interface::GetBaasAccountAdministrator(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();

    LOG_INFO(Service_ACC, "called, uuid=0x{}", uuid.RawString());

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IAdministrator>(system, uuid);
}

void Module::Interface::SynchronizeNetworkServiceAccountsSnapshotAsync(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<SynchronizeNetworkServiceAccountsSnapshotAsyncInterface>(system);
}

void Module::Interface::ProxyProcedureForGuestLoginWithNintendoAccount(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IOAuthProcedureForExternalNsa>(system, dummy_uuid);
}

void Module::Interface::ProxyProcedureForFloatingRegistrationWithNintendoAccount(
    HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IOAuthProcedureForExternalNsa>(system, dummy_uuid);
}

void Module::Interface::ProxyProcedureForDeviceMigrationAuthenticatingOperatingUser(
    HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IOAuthProcedure>(system, dummy_uuid);
}

void Module::Interface::ProxyProcedureForDeviceMigrationDownload(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IOAuthProcedure>(system, dummy_uuid);
}

void Module::Interface::SuspendBackgroundDaemon(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    const Common::UUID dummy_uuid{};
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<ISessionObject>(system, dummy_uuid);
}

void Module::Interface::CreateDeviceMigrationUserExportRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::UploadNasCredential(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::CreateDeviceMigrationUserImportRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::DeleteUserMigrationInfo(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::SetUserUnqualifiedForDebug(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::UnsetUserUnqualifiedForDebug(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::ListUsersUnqualifiedForDebug(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    ctx.WriteBuffer(std::vector<Common::UUID>{});
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::RefreshFirmwareSettingsForDebug(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::DebugInvalidateTokenCacheForUser(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::DebugSetUserStateClose(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::DebugSetUserStateOpen(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

Module::Interface::Interface(std::shared_ptr<Module> module_,
                             std::shared_ptr<ProfileManager> profile_manager_,
                             Core::System& system_, const char* name)
    : ServiceFramework{system_, name}, module{std::move(module_)},
      profile_manager{std::move(profile_manager_)} {}

Module::Interface::~Interface() = default;

void LoopProcess(Core::System& system) {
    auto module = std::make_shared<Module>();
    auto profile_manager = std::make_shared<ProfileManager>();
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("acc:aa",
                                         std::make_shared<ACC_AA>(module, profile_manager, system));
    server_manager->RegisterNamedService("acc:e",
                                         std::make_shared<ACC_E>(module, profile_manager, system));
    server_manager->RegisterNamedService(
        "acc:e:u1", std::make_shared<ACC_E_U1>(module, profile_manager, system));
    server_manager->RegisterNamedService(
        "acc:e:u2", std::make_shared<ACC_E_U2>(module, profile_manager, system));
    server_manager->RegisterNamedService("acc:su",
                                         std::make_shared<ACC_SU>(module, profile_manager, system));
    server_manager->RegisterNamedService("acc:u0",
                                         std::make_shared<ACC_U0>(module, profile_manager, system));
    server_manager->RegisterNamedService("acc:u1",
                                         std::make_shared<ACC_U1>(module, profile_manager, system));
    server_manager->RegisterNamedService("dauth:0", std::make_shared<DAUTH_0>(system));

    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::Account
