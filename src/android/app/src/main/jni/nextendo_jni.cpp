// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Nextendo Network JNI bindings, kept out of native.cpp so upstream edits to
// that file never conflict with this fork's Android additions.

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <thread>

#include <fmt/format.h>

#include "common/android/android_common.h"
#include "common/android/id_cache.h"
#include "common/nextendo_account.h"
#include "common/nextendo_avatar.h"
#include "common/nextendo_compatible_titles.h"
#include "common/nextendo_friends.h"
#include "common/settings.h"

#include "citron/nextendo_byaml.h"
#include "citron/nextendo_save_sync.h"
#include "core/hle/service/friend/friend.h"
#include "web_service/nextendo_api.h"

#include "native.h"

namespace {

std::string EscapeJson(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

std::string FriendJson(const WebService::NextendoApi::Friend& entry) {
    return "{\"pid\":" + std::to_string(entry.pid) + ",\"name\":\"" + EscapeJson(entry.name) +
           "\",\"status\":" + std::to_string(entry.presence_status) + ",\"friend_code\":\"" +
           EscapeJson(entry.friend_code) + "\",\"app_name\":\"" + EscapeJson(entry.app_name) +
           "\",\"image\":\"" + entry.image_base64 + "\"}";
}

void RefreshFriendsCache() {
    const auto list = WebService::NextendoApi::GetFriends();
    if (!list.ok) {
        return;
    }

    std::vector<Common::NextendoFriends::Entry> cache;
    cache.reserve(list.friends.size());
    for (const auto& entry : list.friends) {
        Common::NextendoFriends::Entry cached;
        cached.pid = entry.pid;
        cached.name = entry.name;
        cached.status = entry.presence_status;
        cached.app_field = entry.app_field;
        cached.image = Common::NextendoAvatar::DecodeBase64(entry.image_base64);
        cache.push_back(std::move(cached));
    }

    Common::NextendoFriends::Set(std::move(cache));
    Service::Friend::NotifyFriendsListUpdated();
}

jmethodID NextendoOAuthUrlMethod() {
    static const jmethodID id = [] {
        JNIEnv* env = Common::Android::GetEnvForThread();
        return env->GetStaticMethodID(Common::Android::GetNativeLibraryClass(),
                                      "onNextendoOAuthUrl", "(Ljava/lang/String;)V");
    }();
    return id;
}

jmethodID NextendoSignInResultMethod() {
    static const jmethodID id = [] {
        JNIEnv* env = Common::Android::GetEnvForThread();
        return env->GetStaticMethodID(Common::Android::GetNativeLibraryClass(),
                                      "onNextendoSignInResult", "(ZLjava/lang/String;)V");
    }();
    return id;
}

} // namespace

extern "C" {

jstring Java_org_citron_citron_1emu_NativeLibrary_getNextendoAccountStatus(JNIEnv* env,
                                                                           jobject jobj) {
    if (Common::NextendoAccount::IsLinked()) {
        return Common::Android::ToJString(env, Common::NextendoAccount::GetUsername());
    }
    return Common::Android::ToJString(env, "");
}

jstring Java_org_citron_citron_1emu_NativeLibrary_getNextendoOnlineStatus(JNIEnv* env,
                                                                          jobject jobj) {
    const auto status = WebService::NextendoApi::GetOnlineStatus();
    if (!status.queried || status.allow) {
        return Common::Android::ToJString(env, "");
    }
    return Common::Android::ToJString(env, status.message.empty() ? status.reason : status.message);
}

jint Java_org_citron_citron_1emu_NativeLibrary_nextendoPingBackend(JNIEnv* env, jobject jobj) {
    return WebService::NextendoApi::PingBackend().value_or(-1);
}

void Java_org_citron_citron_1emu_NativeLibrary_nextendoSignOut(JNIEnv* env, jobject jobj) {
    Common::NextendoAccount::Clear();
    Common::NextendoFriends::Set({});
}

void Java_org_citron_citron_1emu_NativeLibrary_nextendoRefreshFriends(JNIEnv* env, jobject jobj) {
    RefreshFriendsCache();
}

jstring Java_org_citron_citron_1emu_NativeLibrary_nextendoFriendsJson(JNIEnv* env, jobject jobj) {
    const auto entries = Common::NextendoFriends::Get();
    std::string json = "[";
    bool first = true;
    for (const auto& entry : entries) {
        if (entry.pid == 0) {
            continue;
        }
        json += first ? "" : ",";
        json += "{\"pid\":" + std::to_string(entry.pid) + ",\"name\":\"" + EscapeJson(entry.name) +
                "\",\"status\":" + std::to_string(entry.status) + "}";
        first = false;
    }
    json += "]";
    return Common::Android::ToJString(env, json);
}

jstring Java_org_citron_citron_1emu_NativeLibrary_nextendoFriendsListJson(JNIEnv* env,
                                                                          jobject jobj) {
    const auto list = WebService::NextendoApi::GetFriends();
    std::string json = "{\"ok\":" + std::string(list.ok ? "true" : "false") + ",\"friends\":[";
    for (std::size_t i = 0; i < list.friends.size(); ++i) {
        json += i == 0 ? "" : ",";
        json += FriendJson(list.friends[i]);
    }
    json += "],\"requests\":[";
    for (std::size_t i = 0; i < list.requests.size(); ++i) {
        json += i == 0 ? "" : ",";
        json += FriendJson(list.requests[i]);
    }
    json += "]}";
    return Common::Android::ToJString(env, json);
}

jstring Java_org_citron_citron_1emu_NativeLibrary_nextendoAddFriend(JNIEnv* env, jobject jobj,
                                                                    jstring code) {
    return Common::Android::ToJString(
        env, WebService::NextendoApi::AddFriendByCode(Common::Android::GetJString(env, code)));
}

jstring Java_org_citron_citron_1emu_NativeLibrary_nextendoAcceptFriend(JNIEnv* env, jobject jobj,
                                                                       jlong pid) {
    return Common::Android::ToJString(env,
                                      WebService::NextendoApi::AcceptFriend(static_cast<u64>(pid)));
}

jstring Java_org_citron_citron_1emu_NativeLibrary_nextendoDeclineFriend(JNIEnv* env, jobject jobj,
                                                                        jlong pid) {
    return Common::Android::ToJString(
        env, WebService::NextendoApi::DeclineFriend(static_cast<u64>(pid)));
}

jstring Java_org_citron_citron_1emu_NativeLibrary_nextendoRemoveFriend(JNIEnv* env, jobject jobj,
                                                                       jlong pid) {
    return Common::Android::ToJString(env,
                                      WebService::NextendoApi::RemoveFriend(static_cast<u64>(pid)));
}

jstring Java_org_citron_citron_1emu_NativeLibrary_nextendoOnlineCountsJson(JNIEnv* env,
                                                                           jobject jobj) {
    const auto counts = WebService::NextendoApi::GetOnlineCounts();
    std::string json = "{";
    bool first = true;
    for (const auto& [title_id, count] : counts) {
        if (!first) {
            json += ",";
        }
        std::string key = title_id;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        json += "\"" + key + "\":" + std::to_string(count);
        first = false;
    }
    json += "}";
    return Common::Android::ToJString(env, json);
}

jboolean Java_org_citron_citron_1emu_NativeLibrary_isNextendoTitle(JNIEnv* env, jobject jobj,
                                                                   jlong program_id) {
    return Nextendo::CompatibleTitles::Table().count(static_cast<u64>(program_id)) > 0;
}

jstring Java_org_citron_citron_1emu_NativeLibrary_nextendoRequiredVersion(JNIEnv* env,
                                                                          jobject jobj,
                                                                          jlong program_id) {
    const auto& table = Nextendo::CompatibleTitles::Table();
    const auto it = table.find(static_cast<u64>(program_id));
    return Common::Android::ToJString(env, it != table.end() ? it->second : "");
}

void Java_org_citron_citron_1emu_NativeLibrary_nextendoPresenceTick(JNIEnv* env, jobject jobj,
                                                                    jlong program_id,
                                                                    jstring japp_name) {
    if (!Common::NextendoAccount::IsLinked()) {
        return;
    }

    const std::string app_name =
        japp_name != nullptr ? Common::Android::GetJString(env, japp_name) : std::string{};
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(program_id));
    const std::string app_id = program_id != 0 ? buf : std::string{};

    s32 status = 0;
    std::string app_field;
    const bool have_update = Common::NextendoFriends::TakeLocalPresenceForPublish(status, app_field);

    static std::string last_app_id;
    if (!have_update && app_id == last_app_id) {
        return;
    }
    last_app_id = app_id;
    if (!have_update) {
        status = Common::NextendoFriends::GetLocalStatus();
        app_field = Common::NextendoFriends::GetLocalAppField();
    }

    std::thread{[status, app_field, app_id, app_name] {
        WebService::NextendoApi::PushPresence(status, app_field, app_id, app_name);
    }}.detach();
}

void Java_org_citron_citron_1emu_NativeLibrary_nextendoSyncPlayTime(JNIEnv* env, jobject jobj,
                                                                    jlong program_id,
                                                                    jlong seconds) {
    if (program_id == 0 || seconds <= 0) {
        return;
    }
    std::thread{[program_id, seconds] {
        WebService::NextendoApi::HistoryEntry entry;
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(program_id));
        entry.title_id = buf;
        entry.seconds = static_cast<u64>(seconds);
        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm utc{};
        gmtime_r(&now, &utc);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &utc);
        entry.last_played = ts;
        WebService::NextendoApi::SyncHistory({entry});
    }}.detach();
}

jstring Java_org_citron_citron_1emu_NativeLibrary_nextendoEnsureBcat(JNIEnv* env, jobject jobj,
                                                                     jlong program_id) {
    switch (Nextendo::Byaml::Ensure(static_cast<u64>(program_id))) {
    case Nextendo::Byaml::Result::Installed:
        return Common::Android::ToJString(env, "installed");
    case Nextendo::Byaml::Result::Failed:
        return Common::Android::ToJString(env, "failed");
    case Nextendo::Byaml::Result::Current:
        break;
    }
    return Common::Android::ToJString(env, "");
}

jstring Java_org_citron_citron_1emu_NativeLibrary_nextendoCloudSavePull(JNIEnv* env, jobject jobj,
                                                                        jlong program_id,
                                                                        jboolean force) {
    const bool manual = force == JNI_TRUE;
    if (!manual && !Settings::values.nextendo_cloud_sync_enabled.GetValue()) {
        return Common::Android::ToJString(env, "disabled");
    }
    const auto result = Nextendo::SaveSync::Pull(EmulationSession::GetInstance().System(),
                                                 static_cast<u64>(program_id), manual);
    switch (result) {
    case Nextendo::SaveSync::Result::Ok:
        return Common::Android::ToJString(env, "applied");
    case Nextendo::SaveSync::Result::NoData:
        return Common::Android::ToJString(env, "none");
    case Nextendo::SaveSync::Result::LocalKept:
        return Common::Android::ToJString(env, "kept");
    case Nextendo::SaveSync::Result::NoSaveDir:
        return Common::Android::ToJString(env, "no_dir");
    case Nextendo::SaveSync::Result::Failed:
        break;
    }
    return Common::Android::ToJString(env, "failed");
}

jstring Java_org_citron_citron_1emu_NativeLibrary_nextendoCloudSavePush(JNIEnv* env, jobject jobj,
                                                                        jlong program_id,
                                                                        jboolean manual) {
    if (manual != JNI_TRUE && !Settings::values.nextendo_cloud_sync_enabled.GetValue()) {
        return Common::Android::ToJString(env, "disabled");
    }
    auto& system = EmulationSession::GetInstance().System();
    // Mirror the desktop: the emulation thread has exited, rebuild a fresh save-data factory
    // before capturing so the archive sees this session's files and nothing the game had
    // open while running.
    if (auto filesystem = system.GetFilesystem()) {
        system.GetFileSystemController().InitializeContentSystem(*filesystem, true);
    }
    const auto result = Nextendo::SaveSync::Push(system, static_cast<u64>(program_id));
    if (result == Nextendo::SaveSync::Result::Ok) {
        return Common::Android::ToJString(env, "uploaded");
    }
    return Common::Android::ToJString(
        env, result == Nextendo::SaveSync::Result::NoData ? "none" : "failed");
}

jstring Java_org_citron_citron_1emu_NativeLibrary_nextendoCloudSaveProbe(JNIEnv* env, jobject jobj,
                                                                         jlong program_id) {
    const auto save = WebService::NextendoApi::PullSave(
        fmt::format("{:016x}", static_cast<u64>(program_id)));
    return Common::Android::ToJString(env, save && !save->empty() ? "available" : "none");
}

void Java_org_citron_citron_1emu_NativeLibrary_setNextendoCaCertPath(JNIEnv* env, jobject jobj,
                                                                     jstring jpath) {
    const std::string path = Common::Android::GetJString(env, jpath);
    WebService::NextendoApi::SetCaCertPathOverride(path);
}

void Java_org_citron_citron_1emu_NativeLibrary_nextendoSignIn(JNIEnv* env, jobject jobj) {
    std::thread([] {
        JNIEnv* thread_env = Common::Android::GetEnvForThread();
        const auto open_url = [thread_env](const std::string& url) {
            const jstring jurl = Common::Android::ToJString(thread_env, url);
            thread_env->CallStaticVoidMethod(Common::Android::GetNativeLibraryClass(),
                                             NextendoOAuthUrlMethod(), jurl);
            thread_env->DeleteLocalRef(jurl);
        };
        auto result = WebService::NextendoApi::SignInWithBrowser(open_url);
        if (result.ok) {
            Common::NextendoAccount::Save(result.pid, result.username, result.friend_code,
                                          result.token);
            RefreshFriendsCache();
            Common::NextendoFriends::SetLocalStatus(Common::NextendoFriends::PresenceOnline);
            // Go online on the network so friends can see us.
            std::thread{[] { WebService::NextendoApi::PushPresence(1, "", "", ""); }}.detach();
        }
        const std::string message = result.ok ? result.username : result.error;
        const jstring jmsg = Common::Android::ToJString(thread_env, message);
        thread_env->CallStaticVoidMethod(Common::Android::GetNativeLibraryClass(),
                                         NextendoSignInResultMethod(),
                                         static_cast<jboolean>(result.ok), jmsg);
        thread_env->DeleteLocalRef(jmsg);
    }).detach();
}

} // extern "C"