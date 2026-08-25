// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Loader::NextendoS3Patches {

// Applies citron's built-in Splatoon 3 patches (certificate-pinning bypass, peer hostname fix)
// to a loaded NSO, keyed by its build ID. Returns nso unchanged if build_id isn't one of the two
// known Splatoon 3 builds.
//
// Baked in rather than shipped as an exefs_patches mod, and for the same reason
// NextendoNetwork/Ryujinx-Nextendo's NextendoS3Patches.cs is: Splatoon 3 refuses to boot with
// any mod enabled (see main.cpp's boot-time check), so a patch living on disk as a mod would be
// blocked by the very rule it needs to get past. These bytes never touch the mod-loading path
// at all, so they aren't affected by it. Without them the game's TLS stops at ClientHello and
// nothing connects -- this bypasses the game's own in-game certificate pinning, which is a
// separate, later check than the TLS-layer verification already bypassed for every other
// Nextendo title in ssl_backend_openssl.cpp's SetVerifyOption.
//
// nso must be [NSOHeader][decompressed segment data], the exact same layout
// PatchManager::PatchNSO expects -- this is designed to run right alongside it in nso.cpp,
// independent of whether normal mod patches applied.
// module_name est le NSO en cours de chargement (« rtld », « main », « sdk »). Il ne sert qu'a
// savoir s'il faut CRIER quand rien ne correspond : les correctifs visent « main », et un « main »
// dont l'identifiant de build est inconnu veut dire que le jeu tourne SANS eux — c'est-a-dire avec
// son epinglage de certificat intact, donc sans aucune chance de se connecter a nos serveurs.
//
// Pourquoi ce cri existe : le 2026-08-25, Splatoon 3 demarrait sur l'executable du JEU DE BASE
// (build 19FE149D…) parce que l'ExeFS de la mise a jour 11.3.0 n'etait pas applique. Aucun des
// trois builds connus ne correspondait, donc zero correctif — EN SILENCE. Le jeu terminait son TLS,
// refusait le certificat epingle, et abandonnait son propre appel avant d'emettre le moindre
// HEADERS : un RST_STREAM(REFUSED_STREAM) sur un flux jamais ouvert, et l'erreur 2321-4992. Des
// semaines d'enquete ont cherche la cause dans gRPC-core et dans l'ordonnanceur du noyau. Une seule
// ligne d'avertissement ici aurait suffi a l'eviter.
std::vector<u8> ApplyIfMatch(const std::array<u8, 0x20>& build_id, std::vector<u8> nso,
                             std::string_view module_name);

} // namespace Loader::NextendoS3Patches
