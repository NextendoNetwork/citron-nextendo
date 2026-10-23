// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cassert>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "common/nextendo_compatible_titles.h"
#include "web_service/game_catalog_manifest.h"

using nlohmann::json;
using namespace Nextendo::CompatibleTitles;
using WebService::GameCatalog::VerifyCatalog;

namespace {

constexpr u64 MarioKart8Deluxe = 0x0100152000022000;
constexpr u64 Splatoon2Eu = 0x0100f8f0000a2000;
constexpr u64 Splatoon2Us = 0x01003bc0000a0000;
constexpr u64 AmongUs = 0x0100b0c013912000;
constexpr u64 AnyVersionTitle = 0x0100aaaa00000001;
constexpr u64 DisabledTitle = 0x0100aaaa00000002;
constexpr u64 NeedsDlcCheck = 0x0100aaaa00000003;
constexpr u64 NotTracked = 0x0100000000ffffff;

std::string Base64(std::span<const unsigned char> bytes) {
    std::string result((bytes.size() + 2) / 3 * 4 + 1, '\0');
    const int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                                       bytes.data(), static_cast<int>(bytes.size()));
    result.resize(static_cast<std::size_t>(length));
    return result;
}

std::string Utc(std::time_t time) {
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&time));
    return text;
}

void ExpectRejected(const std::function<void()>& action) {
    bool failed = false;
    try {
        action();
    } catch (const std::exception&) {
        failed = true;
    }
    assert(failed);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3) {
        std::ifstream input(argv[1], std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
        const auto catalog = VerifyCatalog(body, argv[2], static_cast<std::int64_t>(std::time(nullptr)));
        assert(catalog.table.contains(MarioKart8Deluxe));
        std::cout << "Production game catalog signature verified, revision " << catalog.revision
                  << ", " << catalog.table.size() << " titles\n";
        return 0;
    }

    std::array<unsigned char, 32> seed{};
    seed.fill(9); // Test-only key; unrelated to the production signer.
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()),
        EVP_PKEY_free);
    assert(key);
    std::array<unsigned char, 32> public_bytes{};
    std::size_t public_length = public_bytes.size();
    assert(EVP_PKEY_get_raw_public_key(key.get(), public_bytes.data(), &public_length) == 1);
    const auto public_key = Base64(public_bytes);

    auto sign = [&](const json& payload, const std::string& key_id = "catalog-key-1") {
        const std::string text = payload.dump();
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                        EVP_MD_CTX_free);
        assert(EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1);
        std::array<unsigned char, 64> signature{};
        std::size_t size = signature.size();
        assert(EVP_DigestSign(context.get(), signature.data(), &size,
                              reinterpret_cast<const unsigned char*>(text.data()),
                              text.size()) == 1);
        return json{{"key_id", key_id},
                    {"payload", Base64(std::span(
                                    reinterpret_cast<const unsigned char*>(text.data()), text.size()))},
                    {"signature", Base64(signature)}}
            .dump();
    };

    const std::time_t now = std::time(nullptr);
    auto exact = [](std::initializer_list<const char*> ids, json accepted) {
        return json{{"title_ids", json(ids)},
                    {"enabled", true},
                    {"required_capabilities", {"versions.exact.v1"}},
                    {"versions", {{"policy", "exact"}, {"accepted", accepted}, {"recommended", accepted[0]}}}};
    };
    json games = json::array({
        exact({"0100152000022000"}, {"4.0.1"}),
        exact({"0100f8f0000a2000", "01003bc0000a0000"}, {"5.5.2"}),
        exact({"0100b0c013912000"}, {"2026.19.0", "2026.18.0"}),
        json{{"title_ids", {"0100aaaa00000001"}},
             {"enabled", true},
             {"required_capabilities", {"versions.any.v1"}},
             {"versions", {{"policy", "any"}}}},
        json{{"title_ids", {"0100aaaa00000002"}}, {"enabled", false}, {"required_capabilities", json::array()}},
        json{{"title_ids", {"0100aaaa00000003"}},
             {"enabled", true},
             {"required_capabilities", {"versions.exact.v1", "dlc.required.v1"}},
             {"versions", {{"policy", "exact"}, {"accepted", {"1.0.19"}}}}},
    });
    auto document = [&](json entries, std::uint64_t revision = 7, std::time_t issued = 0,
                         std::time_t expires = 0) {
        issued = issued ? issued : now - 60;
        expires = expires ? expires : issued + 86000;
        return json{{"document_type", "game_catalog"}, {"schema_version", 1},
                    {"revision", revision},          {"issued_at", Utc(issued)},
                    {"expires_at", Utc(expires)},    {"games", entries}};
    };

    // JD uses the existing title-driven badge, count polling and version UI.
    constexpr u64 JustDance2022 = 0x0100ea6014bb8000;
    assert(BuiltInTable().at(JustDance2022) == "327835.644926");
    assert(IsVersionOk(JustDance2022, "327835.644926"));
    assert(IsVersionOk(JustDance2022, "326704.600931"));
    assert(!IsVersionOk(JustDance2022, "327835.644927"));
    assert(!IsVersionOk(JustDance2022, "326704.600930"));
    auto jd = VerifyCatalog(sign(document(json::array({
        exact({"0100ea6014bb8000"}, {"327835.644926", "326704.600931"}),
        exact({"0100b3c014bda000"}, {"3.1.1"})
    }))), public_key, now);
    assert(jd.table.at(JustDance2022) == "327835.644926");
    assert(jd.also_accepted.at(JustDance2022) == std::vector<std::string>{"326704.600931"});
    assert(jd.table.at(0x0100b3c014bda000) == "3.1.1");

    // A valid catalog decides everything once published.
    auto requirements = VerifyCatalog(sign(document(games)), public_key, now);
    assert(requirements.revision == 7);
    assert(ActiveCatalogRevision() == 0);
    assert(&Table() == &BuiltInTable());
    PublishCatalog(requirements);
    assert(ActiveCatalogRevision() == 7);
    assert(Table().size() == 7);
    assert(Table().at(MarioKart8Deluxe) == "4.0.1");
    assert(IsVersionOk(MarioKart8Deluxe, "4.0.1"));
    assert(IsVersionOk(MarioKart8Deluxe, "4.0.1.0")); // numeric spelling is normalized
    assert(!IsVersionOk(MarioKart8Deluxe, "4.0.0"));
    assert(!IsVersionOk(MarioKart8Deluxe, "4.0.2"));
    assert(Table().contains(Splatoon2Eu) && Table().contains(Splatoon2Us));
    assert(IsVersionOk(Splatoon2Us, "5.5.2") && !IsVersionOk(Splatoon2Us, "5.5.1"));
    assert(IsVersionOk(AmongUs, "2026.19.0") && IsVersionOk(AmongUs, "2026.18.0"));
    assert(!IsVersionOk(AmongUs, "2026.17.0"));
    assert(IsVersionOk(AnyVersionTitle, "9.9.9"));
    assert(Table().contains(DisabledTitle) && !IsVersionOk(DisabledTitle, ""));
    assert(Table().contains(NeedsDlcCheck) && !IsVersionOk(NeedsDlcCheck, "1.0.19"));
    assert(!Table().contains(NotTracked) && IsVersionOk(NotTracked, "1.0.0"));

    // Documents that must never be used.
    ExpectRejected([&] { VerifyCatalog(sign(document(games), "npln-patches-key-1"), public_key, now); });
    ExpectRejected([&] {
        std::array<unsigned char, 32> other_seed{};
        other_seed.fill(3);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> other(
            EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, other_seed.data(), other_seed.size()),
            EVP_PKEY_free);
        std::array<unsigned char, 32> other_public{};
        std::size_t other_length = other_public.size();
        EVP_PKEY_get_raw_public_key(other.get(), other_public.data(), &other_length);
        VerifyCatalog(sign(document(games)), Base64(other_public), now);
    });
    ExpectRejected([&] { VerifyCatalog(sign(document(games, 7, now - 90000, now - 4000)), public_key, now); });
    ExpectRejected([&] { VerifyCatalog(sign(document(games, 7, now + 3600)), public_key, now); });
    ExpectRejected([&] {
        json duplicate = games;
        duplicate.push_back(exact({"0100152000022000"}, {"4.0.0"}));
        VerifyCatalog(sign(document(duplicate)), public_key, now);
    });
    ExpectRejected([&] {
        json bad = json::array({exact({"0100152000022000"}, {"4.0.1"})});
        bad[0]["versions"]["recommended"] = "4.0.2";
        VerifyCatalog(sign(document(bad)), public_key, now);
    });
    ExpectRejected([&] {
        auto envelope = json::parse(sign(document(games)));
        envelope["payload"] = Base64(std::span(reinterpret_cast<const unsigned char*>("{}"), 2));
        VerifyCatalog(envelope.dump(), public_key, now);
    });

    // A cached copy may be read after expiry, but an expired catalog no longer decides anything.
    const auto stale = VerifyCatalog(sign(document(games, 8, now - 90000, now - 4000)), public_key, now, true);
    PublishCatalog(stale);
    assert(ActiveCatalogRevision() == 0);
    assert(&Table() == &BuiltInTable());
    assert(Table().size() == BuiltInTable().size());

    std::cout << "Game catalog verification and requirements: all checks passed\n";
    return 0;
}
