// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Usage: citron-updater-helper.exe --pid <citron_pid> --zip <path> --app-dir <path> --version <str>

#include <archive.h>
#include <archive_entry.h>

#include <windows.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::wofstream g_log;

void Log(const std::wstring& line) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    tm local_tm{};
    localtime_s(&local_tm, &now);
    wchar_t timestamp[32];
    wcsftime(timestamp, sizeof(timestamp) / sizeof(wchar_t), L"%H:%M:%S", &local_tm);
    if (g_log.is_open()) {
        g_log << L"[" << timestamp << L"] " << line << L"\n";
        g_log.flush();
    }
}

std::wstring Widen(const char* utf8) {
    if (!utf8) {
        return L"";
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (size <= 0) {
        return L"";
    }
    std::wstring result(static_cast<size_t>(size) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, result.data(), size);
    return result;
}

bool EqualsIgnoreCase(const std::filesystem::path& a, const wchar_t* b) {
    return _wcsicmp(a.c_str(), b) == 0;
}

void WriteResult(const std::filesystem::path& app_dir, bool success, const std::wstring& version,
                  const std::filesystem::path& backup_dir, const std::wstring& detail) {
    std::wofstream out(app_dir / L"update_result.txt", std::ios::trunc);
    if (!out.is_open()) {
        return;
    }
    out << L"STATUS=" << (success ? L"SUCCESS" : L"FAILED") << L"\n";
    out << L"VERSION=" << version << L"\n";
    out << L"BACKUP_DIR=" << backup_dir.wstring() << L"\n";
    out << L"DETAIL=" << detail << L"\n";
}

void WaitForCitronExit(DWORD pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) {
        Log(L"Target process " + std::to_wstring(pid) + L" already exited (or inaccessible).");
        return;
    }
    Log(L"Waiting for process " + std::to_wstring(pid) + L" to exit...");
    const DWORD wait_result = WaitForSingleObject(process, 60000);
    CloseHandle(process);
    Log(wait_result == WAIT_OBJECT_0 ? L"Process exited." : L"Wait timed out after 60s, proceeding anyway.");
}

bool RelaunchCitron(const std::filesystem::path& app_dir) {
    const std::filesystem::path exe_path = app_dir / L"citron.exe";
    if (!std::filesystem::exists(exe_path)) {
        Log(L"Cannot relaunch: " + exe_path.wstring() + L" does not exist.");
        return false;
    }

    for (int attempt = 1; attempt <= 5; ++attempt) {
        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);
        PROCESS_INFORMATION process_info{};
        const BOOL launched =
            CreateProcessW(exe_path.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr,
                           app_dir.c_str(), &startup_info, &process_info);
        if (launched) {
            CloseHandle(process_info.hProcess);
            CloseHandle(process_info.hThread);
            Log(L"Relaunched citron.exe successfully (attempt " + std::to_wstring(attempt) + L").");
            return true;
        }
        const DWORD error = GetLastError();
        Log(L"Relaunch attempt " + std::to_wstring(attempt) + L" failed, error code " +
            std::to_wstring(error));
        if (attempt < 5) {
            Sleep(500);
        }
    }
    Log(L"Failed to relaunch citron.exe after 5 attempts.");
    return false;
}

bool BackupCurrentInstall(const std::filesystem::path& app_dir, const std::filesystem::path& backup_dir,
                          size_t& backed_up_count, std::wstring& detail_out) {
    std::error_code ec;
    backed_up_count = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             app_dir, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        const std::filesystem::path rel = std::filesystem::relative(it->path(), app_dir, ec);
        if (ec || rel.empty()) {
            continue;
        }
        const std::filesystem::path top = *rel.begin();
        if (EqualsIgnoreCase(top, L"user") || EqualsIgnoreCase(top, L"backup")) {
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::filesystem::path dest = backup_dir / rel;
        std::filesystem::create_directories(dest.parent_path(), ec);
        std::filesystem::copy_file(it->path(), dest, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            detail_out = L"Failed to back up " + rel.wstring() + L": " + Widen(ec.message().c_str());
            return false;
        }
        ++backed_up_count;
    }
    return true;
}

void RestoreFromBackup(const std::filesystem::path& app_dir, const std::filesystem::path& backup_dir) {
    Log(L"Restoring from backup...");
    std::error_code ec;
    size_t restored = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(backup_dir, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file(ec)) {
            continue;
        }
        const std::filesystem::path rel = std::filesystem::relative(it->path(), backup_dir, ec);
        if (ec) {
            continue;
        }
        const std::filesystem::path dest = app_dir / rel;
        std::filesystem::create_directories(dest.parent_path(), ec);
        std::filesystem::copy_file(it->path(), dest, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            ++restored;
        }
    }
    Log(L"Restored " + std::to_wstring(restored) + L" file(s) from backup.");
}

// Reads via libarchive, writes via std::ofstream -- archive_write_disk's own write path was
// producing zero-filled files for large binaries in this environment (likely AV real-time
// scanning); plain ofstream writes proved reliable, so libarchive is only used for decompression.
bool ExtractToScratch(const std::filesystem::path& zip_path, const std::filesystem::path& scratch_dir,
                      size_t& extracted_count, std::wstring& detail_out) {
    struct archive* reader = archive_read_new();
    if (!reader) {
        detail_out = L"Failed to initialize libarchive.";
        return false;
    }
    archive_read_support_format_zip(reader);
    archive_read_support_filter_all(reader);

#ifdef _WIN32
    if (archive_read_open_filename_w(reader, zip_path.c_str(), 10240) != ARCHIVE_OK) {
#else
    if (archive_read_open_filename(reader, zip_path.string().c_str(), 10240) != ARCHIVE_OK) {
#endif
        detail_out = L"Failed to open update archive: " + Widen(archive_error_string(reader));
        archive_read_free(reader);
        return false;
    }

    extracted_count = 0;
    struct archive_entry* entry;
    std::error_code ec;

    while (archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
#ifdef _WIN32
        const wchar_t* entry_name = archive_entry_pathname_w(entry);
        const std::filesystem::path rel_path =
            entry_name != nullptr ? std::filesystem::path{entry_name}
                                  : std::filesystem::path{archive_entry_pathname(entry)};
#else
        const std::filesystem::path rel_path = archive_entry_pathname(entry);
#endif
        const std::filesystem::path dest_path = scratch_dir / rel_path;

        if (archive_entry_filetype(entry) == AE_IFDIR) {
            std::filesystem::create_directories(dest_path, ec);
            continue;
        }

        std::filesystem::create_directories(dest_path.parent_path(), ec);

        std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            detail_out = L"Failed to create " + rel_path.wstring() + L" in scratch directory.";
            Log(L"ERROR: " + detail_out);
            archive_read_free(reader);
            return false;
        }

        const void* buffer;
        size_t size;
        la_int64_t offset;
        int r;
        while ((r = archive_read_data_block(reader, &buffer, &size, &offset)) == ARCHIVE_OK) {
            out.seekp(offset);
            out.write(reinterpret_cast<const char*>(buffer), size);
            if (!out) {
                detail_out = L"Failed to write " + rel_path.wstring() + L" to scratch directory.";
                Log(L"ERROR: " + detail_out);
                out.close();
                archive_read_free(reader);
                return false;
            }
        }
        if (r != ARCHIVE_EOF) {
            detail_out = L"Failed to read " + rel_path.wstring() + L": " + Widen(archive_error_string(reader));
            Log(L"ERROR: " + detail_out);
            out.close();
            archive_read_free(reader);
            return false;
        }
        out.close();

        ++extracted_count;
    }

    archive_read_close(reader);
    archive_read_free(reader);
    return true;
}

bool MoveExtractedFiles(const std::filesystem::path& scratch_dir, const std::filesystem::path& app_dir,
                        size_t& moved_count, std::wstring& detail_out) {
    wchar_t self_path_buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self_path_buf, MAX_PATH);
    std::error_code self_ec;
    const std::filesystem::path self_path = std::filesystem::canonical(self_path_buf, self_ec);

    std::error_code ec;
    moved_count = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(scratch_dir, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file(ec)) {
            continue;
        }
        const std::filesystem::path rel = std::filesystem::relative(it->path(), scratch_dir, ec);
        if (ec) {
            continue;
        }
        const std::filesystem::path dest = app_dir / rel;
        std::filesystem::create_directories(dest.parent_path(), ec);

        std::error_code dest_ec;
        const bool dest_is_self = !self_ec && std::filesystem::exists(dest, dest_ec) &&
                                  std::filesystem::equivalent(dest, self_path, dest_ec);
        if (dest_is_self) {
            // Can't delete/overwrite our own running executable's file in place, but Windows
            // allows renaming it out of the way while it's still mapped in.
            std::filesystem::path retired = dest;
            retired += L".old";
            std::error_code retire_ec;
            std::filesystem::remove(retired, retire_ec);
            retire_ec.clear();
            std::filesystem::rename(dest, retired, retire_ec);
        } else {
            std::filesystem::remove(dest, ec);
            ec.clear();
        }

        std::filesystem::rename(it->path(), dest, ec);
        if (ec) {
            ec.clear();
            std::filesystem::copy_file(it->path(), dest, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                detail_out = L"Failed to move " + rel.wstring() + L" into place: " + Widen(ec.message().c_str());
                return false;
            }
        }
        ++moved_count;
    }
    return true;
}

bool ApplyUpdate(const std::filesystem::path& zip_path, const std::filesystem::path& app_dir,
                  const std::filesystem::path& backup_dir, const std::filesystem::path& scratch_dir,
                  std::wstring& detail_out) {
    size_t backed_up_count = 0;
    Log(L"Backing up current installation to " + backup_dir.wstring() + L"...");
    if (!BackupCurrentInstall(app_dir, backup_dir, backed_up_count, detail_out)) {
        Log(L"ERROR: " + detail_out);
        return false;
    }
    Log(L"Backed up " + std::to_wstring(backed_up_count) + L" file(s).");

    std::error_code ec;
    std::filesystem::remove_all(scratch_dir, ec);
    std::filesystem::create_directories(scratch_dir, ec);

    size_t extracted_count = 0;
    if (!ExtractToScratch(zip_path, scratch_dir, extracted_count, detail_out)) {
        RestoreFromBackup(app_dir, backup_dir);
        std::filesystem::remove_all(scratch_dir, ec);
        return false;
    }

    size_t moved_count = 0;
    if (!MoveExtractedFiles(scratch_dir, app_dir, moved_count, detail_out)) {
        Log(L"ERROR: " + detail_out);
        RestoreFromBackup(app_dir, backup_dir);
        std::filesystem::remove_all(scratch_dir, ec);
        return false;
    }

    std::filesystem::remove_all(scratch_dir, ec);
    detail_out = std::to_wstring(moved_count) + L" file(s) updated, " +
                 std::to_wstring(backed_up_count) + L" backed up to " + backup_dir.wstring();
    return true;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    DWORD citron_pid = 0;
    std::filesystem::path zip_path;
    std::filesystem::path app_dir;
    std::wstring version;

    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring flag = argv[i];
        const std::wstring value = argv[i + 1];
        if (flag == L"--pid") {
            citron_pid = static_cast<DWORD>(std::wcstoul(value.c_str(), nullptr, 10));
        } else if (flag == L"--zip") {
            zip_path = value;
        } else if (flag == L"--app-dir") {
            app_dir = value;
        } else if (flag == L"--version") {
            version = value;
        }
    }

    if (citron_pid == 0 || zip_path.empty() || app_dir.empty()) {
        return 1;
    }

    g_log.open(app_dir / L"update_helper_log.txt", std::ios::trunc);
    Log(L"citron-updater-helper starting. pid=" + std::to_wstring(citron_pid) +
        L" zip=" + zip_path.wstring() + L" app_dir=" + app_dir.wstring());

    WaitForCitronExit(citron_pid);

    const std::filesystem::path backup_dir = app_dir / L"backup" / (L"backup_" + version);
    std::error_code ec;
    std::filesystem::create_directories(backup_dir, ec);
    if (ec) {
        Log(L"Failed to create backup directory: " + Widen(ec.message().c_str()));
        WriteResult(app_dir, false, version, backup_dir, L"Failed to create backup directory.");
        RelaunchCitron(app_dir);
        return 1;
    }

    const std::filesystem::path scratch_dir =
        std::filesystem::temp_directory_path(ec) / L"citron_updater_extract";

    std::wstring detail;
    const bool success = ApplyUpdate(zip_path, app_dir, backup_dir, scratch_dir, detail);
    Log(success ? L"Update applied: " + detail : L"Update FAILED: " + detail);
    WriteResult(app_dir, success, version, backup_dir, detail);

    std::filesystem::remove(zip_path, ec);

    RelaunchCitron(app_dir);
    return success ? 0 : 1;
}
