// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <span>
#include <string_view>

#include "common/hex_util.h"
#include "common/logging.h"
#include "core/file_sys/ips_layer.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/nextendo_s3_patches.h"

namespace Loader::NextendoS3Patches {

namespace {

// Bytes match NextendoNetwork/Ryujinx-Nextendo's NextendoS3Patches.cs. Offsets are Ryujinx's
// stored constants as-is (reverted from a +0x100 adjustment -- see project_splatoon3_connectivity
// memory for why that theory didn't pan out against a packet capture).

// Bypasses the certificate check: the game pins Nintendo's certificates and refuses ours.
// 19 bytes, fingerprint 219ee0cfea21fd4b...
constexpr std::array<u8, 19> kCertificateBypass{{
    0x49, 0x50, 0x53, 0x33, 0x32,       // "IPS32"
    0x00, 0x15, 0x7B, 0x20, 0x00, 0x04, // offset 0x00157B20, 4 bytes
    0x2A, 0x00, 0x80, 0x52,
    0x45, 0x45, 0x4F, 0x46, // "EEOF"
}};

// Fixes the hostname the game presents to its peer. 29 bytes, fingerprint aa8a453637092346...
constexpr std::array<u8, 29> kPeerHostnameFix{{
    0x49, 0x50, 0x53, 0x33, 0x32,       // "IPS32"
    0x00, 0x14, 0xE1, 0xB0, 0x00, 0x04, // offset 0x0014E1B0, 4 bytes
    0x1F, 0x20, 0x03, 0xD5,
    0x00, 0x14, 0xDD, 0x80, 0x00, 0x04, // offset 0x0014DD80, 4 bytes
    0xF4, 0x03, 0x1F, 0x2A,
    0x45, 0x45, 0x4F, 0x46, // "EEOF"
}};

struct KnownBuild {
    std::string_view build_id_hex; // Uppercase, trailing zero bytes stripped -- same convention
                                    // PatchManager::PatchNSO already uses for build_id matching.
    bool include_peer_hostname_fix;
};

// NSO build ID -> which patches it needs. An unrecognized build gets nothing, exactly like an
// .ips file whose name didn't match any program. v11.3.0's build was verified byte-identical to
// v11.2.0's at all three patch offsets before adding it here (no re-derivation needed).
constexpr std::array<KnownBuild, 3> kKnownBuilds{{
    {"6830B3A12406CB4716FEC5ADDC35D3E2DC92D212", true},
    {"726D2B882DD9EF10F4A9D73EED088740630FB6C8", false},
    {"28C4287AEE36F7499DA60F3E68B54C70DA382D75", true},
}};

FileSys::VirtualFile MakeIpsFile(std::span<const u8> bytes) {
    return std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(bytes.begin(), bytes.end()),
                                                     "nextendo_s3.ips32");
}

// Applies one patch, logging (not throwing) on failure -- a patch that can't apply shouldn't
// stop the game from booting at all, just from working online, same as a bad exefs_patches file
// would silently no-op today.
std::vector<u8> ApplyOne(std::vector<u8> nso, std::span<const u8> ips_bytes,
                         std::string_view what) {
    auto in_file = std::make_shared<FileSys::VectorVfsFile>(nso, "nso");
    const auto patched = FileSys::PatchIPS(in_file, MakeIpsFile(ips_bytes));
    if (patched == nullptr) {
        LOG_ERROR(Loader, "[Nextendo] Splatoon 3: {} patch failed to apply", what);
        return nso;
    }
    return patched->ReadAllBytes();
}

} // namespace

std::vector<u8> ApplyIfMatch(const std::array<u8, 0x20>& build_id, std::vector<u8> nso,
                             std::string_view module_name) {
    const auto build_id_raw = Common::HexToString(build_id);
    const auto build_id_hex = build_id_raw.substr(0, build_id_raw.find_last_not_of('0') + 1);

    for (const auto& known : kKnownBuilds) {
        if (build_id_hex != known.build_id_hex) {
            continue;
        }

        int applied_count = 1;
        nso = ApplyOne(std::move(nso), kCertificateBypass, "certificate-bypass");
        if (known.include_peer_hostname_fix) {
            nso = ApplyOne(std::move(nso), kPeerHostnameFix, "peer-hostname");
            ++applied_count;
        }

        LOG_INFO(Loader, "[Nextendo] Splatoon 3: {} built-in patch(es) applied (build {})",
                 applied_count, build_id_hex);
        return nso;
    }

    // Rien ne correspond. Sur « main », c'est fatal pour l'en ligne : on le dit fort, une fois.
    if (module_name == "main") {
        LOG_ERROR(Loader,
                  "[Nextendo] Splatoon 3 : AUCUN correctif integre pour ce build ({}). L'en ligne "
                  "NE FONCTIONNERA PAS : l'epinglage de certificat du jeu reste actif, la "
                  "connexion "
                  "NPLN echouera en 2321-4992 apres une poignee de main TLS pourtant reussie. "
                  "Cause la plus frequente : l'ExeFS de la mise a jour n'est pas applique et c'est "
                  "l'executable du JEU DE BASE qui tourne. Verifiez que la mise a jour est bien "
                  "installee ET active pour ce titre.",
                  build_id_hex);
    }

    return nso;
}

} // namespace Loader::NextendoS3Patches
