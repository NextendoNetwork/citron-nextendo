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
#include <thread>

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
        std::string name;
        name.reserve(entry.name.size());
        for (const char c : entry.name) {
            if (c == '"' || c == '\\') {
                name += '\\';
            }
            name += c;
        }
        json += first ? "" : ",";
        json += "{\"pid\":" + std::to_string(entry.pid) + ",\"name\":\"" + name +
                "\",\"status\":" + std::to_string(entry.status) + "}";
        first = false;
    }
    json += "]";
    return Common::Android::ToJString(env, json);
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

void Java_org_citron_citron_1emu_NativeLibrary_nextendoCloudSavePull(JNIEnv* env, jobject jobj,
                                                                      jlong program_id) {
    if (!Settings::values.nextendo_cloud_sync_enabled.GetValue()) {
        return;
    }
    Nextendo::SaveSync::Pull(EmulationSession::GetInstance().System(),
                             static_cast<u64>(program_id));
}

void Java_org_citron_citron_1emu_NativeLibrary_nextendoCloudSavePush(JNIEnv* env, jobject jobj,
                                                                     jlong program_id) {
    if (!Settings::values.nextendo_cloud_sync_enabled.GetValue()) {
        return;
    }
    auto& system = EmulationSession::GetInstance().System();
    // Mirror the desktop: the emulation thread has exited, rebuild a fresh save-data factory
    // before capturing so the archive sees this session's files and nothing the game had
    // open while running.
    if (auto filesystem = system.GetFilesystem()) {
        system.GetFileSystemController().InitializeContentSystem(*filesystem, true);
    }
    auto zip = Nextendo::SaveSync::CaptureForPush(system, static_cast<u64>(program_id));
    if (zip.empty()) {
        return;
    }
    const u64 title_id = static_cast<u64>(program_id);
    std::thread{[title_id, zip = std::move(zip)]() mutable {
        Nextendo::SaveSync::UploadCaptured(title_id, std::move(zip));
    }}.detach();
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