// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/nextendo_save_sync.h"

#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <cstdlib>
#include <fstream>
#include <iterator>
#endif

#include <fmt/format.h>

#include "common/nextendo_compatible_titles.h"
#include "common/logging.h"
#include "common/nextendo_account.h"
#include "core/core.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/filesystem/filesystem.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#endif

#ifdef CITRON_ENABLE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

namespace Nextendo::SaveSync {

namespace {

#if defined(CITRON_ENABLE_LIBARCHIVE) || defined(_WIN32)

// Only ever called from the real-backend paths below (libarchive, or the Windows PowerShell
// fallback) -- kept inside this guard so an unused-function error doesn't fire on builds with
// neither (e.g. a non-Windows build without libarchive).
bool IsEligible(u64 title_id) {
    return Nextendo::CompatibleTitles::Table().count(title_id) != 0 &&
          Common::NextendoAccount::IsLinked();
}

bool HasLocalContent(const FileSys::VirtualDir& dir) {
    if (!dir) {
        return false;
    }
    if (!dir->GetFiles().empty()) {
        return true;
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        if (sub && HasLocalContent(sub)) {
            return true;
        }
    }
    return false;
}

#endif // CITRON_ENABLE_LIBARCHIVE || _WIN32

#ifdef CITRON_ENABLE_LIBARCHIVE

void AddDirectoryToArchive(struct archive* a, const FileSys::VirtualDir& dir,
                           const std::string& prefix) {
    for (const auto& file : dir->GetFiles()) {
        if (!file) {
            continue;
        }
        const std::vector<u8> bytes = file->ReadAllBytes();

        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, (prefix + file->GetName()).c_str());
        archive_entry_set_size(entry, static_cast<la_int64_t>(bytes.size()));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        if (archive_write_header(a, entry) == ARCHIVE_OK && !bytes.empty()) {
            archive_write_data(a, bytes.data(), bytes.size());
        }
        archive_entry_free(entry);
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        if (sub) {
            AddDirectoryToArchive(a, sub, prefix + sub->GetName() + "/");
        }
    }
}

la_ssize_t ArchiveAppendCallback(struct archive*, void* client_data, const void* buff,
                                 size_t length) {
    auto* out = static_cast<std::vector<u8>*>(client_data);
    const auto* bytes = static_cast<const u8*>(buff);
    out->insert(out->end(), bytes, bytes + length);
    return static_cast<la_ssize_t>(length);
}

std::vector<u8> ZipDirectory(const FileSys::VirtualDir& dir) {
    std::vector<u8> out;
    struct archive* a = archive_write_new();
    if (!a) {
        return out;
    }
    archive_write_set_format_zip(a);
    archive_write_open(a, &out, nullptr, ArchiveAppendCallback, nullptr);
    AddDirectoryToArchive(a, dir, "");
    archive_write_close(a);
    archive_write_free(a);
    return out;
}

bool UnzipToDirectory(std::span<const u8> zip_data, const std::filesystem::path& dest) {
    struct archive* a = archive_read_new();
    struct archive* ext = archive_write_disk_new();
    if (!a || !ext) {
        return false;
    }

    archive_read_support_format_zip(a);
    archive_read_support_filter_all(a);
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_memory(a, zip_data.data(), zip_data.size()) != ARCHIVE_OK) {
        archive_read_free(a);
        archive_write_free(ext);
        return false;
    }

    std::filesystem::create_directories(dest);

    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const std::filesystem::path entry_path = dest / archive_entry_pathname(entry);
        archive_entry_set_pathname(entry, entry_path.string().c_str());

        if (archive_write_header(ext, entry) != ARCHIVE_OK) {
            continue;
        }
        if (archive_entry_size(entry) > 0) {
            const void* buff;
            size_t size;
            la_int64_t offset;
            while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
                if (archive_write_data_block(ext, buff, size, offset) != ARCHIVE_OK) {
                    break;
                }
            }
        }
        archive_write_finish_entry(ext);
    }

    archive_read_free(a);
    archive_write_free(ext);
    return true;
}

#endif // CITRON_ENABLE_LIBARCHIVE

#if !defined(CITRON_ENABLE_LIBARCHIVE) && defined(_WIN32)

// No libarchive available for this target (e.g. the llvm-mingw cross-compiled Windows build has
// no mingw port of it wired up). GetFullPath() on a save directory is always a real path on
// disk, so shell out to PowerShell's Compress-Archive/Expand-Archive against it directly --
// the same fallback idiom GMainWindow::ExtractZipToDirectory already uses for firmware zips.

std::vector<u8> ZipDirectoryPowerShell(const FileSys::VirtualDir& dir, u64 title_id) {
    const std::filesystem::path real_dir = dir->GetFullPath();
    const std::filesystem::path tmp_zip =
        std::filesystem::temp_directory_path() / fmt::format("nextendo_save_{:016x}.zip", title_id);
    std::error_code ec;
    std::filesystem::remove(tmp_zip, ec);

    const std::string cmd = "powershell -NoProfile -NonInteractive -Command \"Compress-Archive -Path \\\"" +
                             real_dir.string() + "\\*\\\" -DestinationPath \\\"" + tmp_zip.string() +
                             "\\\" -Force\"";
    if (std::system(cmd.c_str()) != 0) {
        std::filesystem::remove(tmp_zip, ec);
        return {};
    }

    std::vector<u8> out;
    {
        // Windows can't delete a file with an open handle (unlike POSIX) -- close in first.
        std::ifstream in(tmp_zip, std::ios::binary);
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::filesystem::remove(tmp_zip, ec);
    return out;
}

bool UnzipToDirectoryPowerShell(std::span<const u8> zip_data, const std::filesystem::path& dest,
                                 u64 title_id) {
    std::filesystem::create_directories(dest);
    const std::filesystem::path tmp_zip = std::filesystem::temp_directory_path() /
                                           fmt::format("nextendo_save_in_{:016x}.zip", title_id);
    {
        std::ofstream out(tmp_zip, std::ios::binary);
        if (!out) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(zip_data.data()),
                  static_cast<std::streamsize>(zip_data.size()));
    }

    const std::string cmd = "powershell -NoProfile -NonInteractive -Command \"Expand-Archive -Path \\\"" +
                             tmp_zip.string() + "\\\" -DestinationPath \\\"" + dest.string() +
                             "\\\" -Force\"";
    const bool ok = std::system(cmd.c_str()) == 0;
    std::error_code ec;
    std::filesystem::remove(tmp_zip, ec);
    return ok;
}

#endif // !CITRON_ENABLE_LIBARCHIVE && _WIN32

} // namespace

#ifdef CITRON_ENABLE_LIBARCHIVE
bool ExtractZipToDirectory(std::span<const u8> zip_data, const std::filesystem::path& dest) {
    return UnzipToDirectory(zip_data, dest);
}
#endif

Result Pull(Core::System& system, u64 title_id, bool force) {
#if defined(ENABLE_WEB_SERVICE) && (defined(CITRON_ENABLE_LIBARCHIVE) || defined(_WIN32))
    if (!IsEligible(title_id)) {
        LOG_WARNING(Frontend, "Nextendo save pull {:016X}: ineligible (title or account)",
                    title_id);
        return Result::Failed;
    }

    auto save_dir = system.GetFileSystemController().GetSaveDataFactory().GetTitleSaveDirectory(
        title_id);
    if (!force && HasLocalContent(save_dir)) {
        LOG_INFO(Frontend, "Nextendo save pull {:016X}: local save present -> kept (no overwrite)",
                 title_id);
        return Result::LocalKept;
    }

    const auto zip = WebService::NextendoApi::PullSave(fmt::format("{:016x}", title_id));
    if (!zip || zip->empty()) {
        LOG_INFO(Frontend, "Nextendo save pull {:016X}: no cloud save on the server", title_id);
        return Result::NoData;
    }

    if (!save_dir) {
        // The title has never saved on this device: create its save directory (the layout the
        // guest itself would) instead of dropping the restore. Without this a fresh device can
        // never download, and its first local save then overwrites the cloud copy.
        const auto& save_factory = system.GetFileSystemController().GetSaveDataFactory();
        save_dir = save_factory.GetOrCreateTitleSaveDirectory(title_id);
    }
    if (!save_dir) {
        LOG_WARNING(Frontend, "Nextendo save pull {:016X}: no save directory to restore into",
                    title_id);
        return Result::NoSaveDir;
    }

#ifdef CITRON_ENABLE_LIBARCHIVE
    const bool applied = UnzipToDirectory(*zip, save_dir->GetFullPath());
#else
    const bool applied = UnzipToDirectoryPowerShell(*zip, save_dir->GetFullPath(), title_id);
#endif
    if (!applied) {
        return Result::Failed;
    }
    LOG_INFO(Frontend, "Nextendo save pull {:016X}: applied ({} B)", title_id, zip->size());
    return Result::Ok;
#else
    (void)system;
    (void)title_id;
    (void)force;
    return Result::Failed;
#endif
}

std::vector<u8> CaptureForPush(Core::System& system, u64 title_id) {
#if defined(CITRON_ENABLE_LIBARCHIVE) || defined(_WIN32)
    if (!IsEligible(title_id)) {
        return {};
    }
    auto save_dir = system.GetFileSystemController().GetSaveDataFactory().GetTitleSaveDirectory(
        title_id);
    if (!save_dir) {
        return {};
    }
#ifdef CITRON_ENABLE_LIBARCHIVE
    return ZipDirectory(save_dir);
#else
    return ZipDirectoryPowerShell(save_dir, title_id);
#endif
#else
    (void)system;
    (void)title_id;
    return {};
#endif
}

std::string UploadCaptured(u64 title_id, std::vector<u8> zip_bytes) {
#ifdef ENABLE_WEB_SERVICE
    if (zip_bytes.empty()) {
        return "empty";
    }
    const std::string error =
        WebService::NextendoApi::PushSave(fmt::format("{:016x}", title_id), zip_bytes);
    if (!error.empty()) {
        LOG_WARNING(Frontend, "Nextendo save push {:016X} failed: {}", title_id, error);
    } else {
        LOG_INFO(Frontend, "Nextendo save push {:016X}: {} B", title_id, zip_bytes.size());
    }
    return error;
#else
    (void)title_id;
    (void)zip_bytes;
    return "disabled";
#endif
}

Result Push(Core::System& system, u64 title_id) {
    const auto zip = CaptureForPush(system, title_id);
    if (zip.empty()) {
        return Result::NoData;
    }
    return UploadCaptured(title_id, std::move(zip)).empty() ? Result::Ok : Result::Failed;
}

} // namespace Nextendo::SaveSync
