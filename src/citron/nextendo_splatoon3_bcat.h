// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef CITRON_ENABLE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

namespace Nextendo::Splatoon3Bcat {
inline constexpr unsigned long long TitleId = 0x0100C2500FC20000ULL;
inline constexpr std::array<std::string_view, 4> Regions{
    "ap-default", "eu-default", "jp-default", "us-default"};
inline constexpr std::array<std::string_view, 6> Files{
    "summary.yml", "base.pack.zs.enc", "bh.pack.zs.enc", "wa.pack.zs.enc",
    "wb.pack.zs.enc", "wc.pack.zs.enc"};
inline constexpr std::size_t MaxSize = 32 * 1024 * 1024;
inline std::mutex InstallMutex;

inline bool IsInstalled(const std::filesystem::path& root) {
    std::error_code ec;
    for (const auto region : Regions) {
        for (const auto file : Files) {
            const auto path = root / region / file;
            if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, ec)) ||
                ec || std::filesystem::file_size(path, ec) == 0 || ec) {
                return false;
            }
        }
    }
    return true;
}

// Validate in memory before touching the installed package. No archive path is used
// on disk until it has passed the single-region/single-filename allowlist.
inline bool Install(std::span<const unsigned char> bytes, const std::filesystem::path& target) {
#ifdef CITRON_ENABLE_LIBARCHIVE
    std::lock_guard lock{InstallMutex};
    if (bytes.empty() || bytes.size() > MaxSize) {
        throw std::runtime_error("Invalid BCAT ZIP size");
    }
    std::unique_ptr<archive, decltype(&archive_read_free)> reader{archive_read_new(), archive_read_free};
    if (!reader || archive_read_support_format_zip(reader.get()) != ARCHIVE_OK ||
        archive_read_open_memory(reader.get(), bytes.data(), bytes.size()) != ARCHIVE_OK) {
        throw std::runtime_error("Cannot open BCAT ZIP");
    }
    std::map<std::string, std::vector<unsigned char>> contents;
    std::size_t total = 0, count = 0;
    archive_entry* entry = nullptr;
    int status;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        if (++count > 512 || !archive_entry_pathname(entry) ||
            archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
            archive_entry_is_encrypted(entry) != 0) {
            throw std::runtime_error("Invalid BCAT entry");
        }
        std::string name{archive_entry_pathname(entry)};
        const bool directory = archive_entry_filetype(entry) == AE_IFDIR;
        if (directory && !name.empty() && name.back() == '/') name.pop_back();
        const auto slash = name.find('/');
        const auto region = name.substr(0, slash);
        if (std::find(Regions.begin(), Regions.end(), region) == Regions.end()) {
            throw std::runtime_error("Invalid BCAT region");
        }
        if (directory && slash == std::string::npos) continue;
        if (directory || archive_entry_filetype(entry) != AE_IFREG ||
            slash == std::string::npos) {
            throw std::runtime_error("Invalid BCAT file");
        }
        const auto file = name.substr(slash + 1);
        if (file.empty() || file.size() > 31 || file == "." || file == ".." ||
            !std::all_of(file.begin(), file.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            })) {
            throw std::runtime_error("Unsafe BCAT filename");
        }
        std::string folded = name;
        for (auto& c : folded) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        for (const auto& [old_name, unused] : contents) {
            std::string old_folded = old_name;
            for (auto& c : old_folded) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (old_folded == folded) throw std::runtime_error("Duplicate BCAT file");
        }
        const auto size = archive_entry_size(entry);
        if (size <= 0 || static_cast<unsigned long long>(size) > MaxSize - total) {
            throw std::runtime_error("Invalid expanded BCAT size");
        }
        auto& data = contents[name];
        data.resize(static_cast<std::size_t>(size));
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto n = archive_read_data(reader.get(), data.data() + offset, data.size() - offset);
            if (n <= 0) throw std::runtime_error("Truncated BCAT file");
            offset += static_cast<std::size_t>(n);
        }
        unsigned char extra;
        if (archive_read_data(reader.get(), &extra, 1) != 0) {
            throw std::runtime_error("BCAT size/checksum mismatch");
        }
        total += data.size();
    }
    if (status != ARCHIVE_EOF) throw std::runtime_error("Invalid BCAT archive");
    for (const auto region : Regions) for (const auto file : Files) {
        if (!contents.contains(std::string(region) + "/" + std::string(file))) {
            throw std::runtime_error("Incomplete BCAT package");
        }
    }
    auto stage = target;
    stage += ".nextendo-stage";
    auto backup = target;
    backup += ".nextendo-backup";
    // Recover an interrupted promotion before beginning another one.
    if (!std::filesystem::exists(target) && std::filesystem::exists(backup)) {
        std::filesystem::rename(backup, target);
    }
    bool identical = IsInstalled(target);
    std::size_t disk_files = 0;
    if (identical) {
        for (const auto& item : std::filesystem::recursive_directory_iterator(target)) {
            if (item.is_symlink()) { identical = false; break; }
            if (item.is_regular_file()) ++disk_files;
        }
        identical = identical && disk_files == contents.size();
    }
    if (identical) for (const auto& [name, data] : contents) {
        std::ifstream in{target / name, std::ios::binary};
        const std::vector<unsigned char> existing{std::istreambuf_iterator<char>{in}, {}};
        if (!in || existing != data) { identical = false; break; }
    }
    if (identical) return false;
    std::filesystem::remove_all(stage);
    try {
        for (const auto& [name, data] : contents) {
            const auto path = stage / name;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out{path, std::ios::binary | std::ios::trunc};
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            out.close();
            if (!out) throw std::runtime_error("Cannot write BCAT staging file");
        }
        std::filesystem::remove_all(backup);
        const bool had_old = std::filesystem::exists(target);
        if (had_old) std::filesystem::rename(target, backup);
        try {
            std::filesystem::rename(stage, target);
        } catch (...) {
            if (had_old) std::filesystem::rename(backup, target);
            throw;
        }
        std::error_code ec;
        std::filesystem::remove_all(backup, ec);
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove_all(stage, ec);
        throw;
    }
    return true;
#else
    (void)bytes;
    (void)target;
    throw std::runtime_error("Splatoon 3 BCAT requires libarchive support");
#endif
}
} // namespace Nextendo::Splatoon3Bcat
