// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#ifdef CITRON_ENABLE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

// Plain BCAT packages served by /api/bcat/<title> (Pokemon, Switch Sports, Smash Ultimate...).
// A package is a ZIP of the title's whole delivery cache: every entry is "<directory>/<file>",
// exactly what the game walks with EnumerateDeliveryCacheDirectory and the per-directory
// Open/Read calls. The package replaces the title's cache folder as a whole, so a directory or
// file the server stopped serving disappears with it, and nothing outside that folder (another
// title's cache) is ever written.
namespace Nextendo::PackageBcat {
inline constexpr std::size_t MaxSize = 32 * 1024 * 1024;
inline constexpr std::size_t MaxEntries = 512;
// Same limit as the BCAT DirectoryName / FileName (32 bytes, NUL included).
inline constexpr std::size_t MaxNameLength = 31;
inline constexpr std::string_view HashFileName = ".nextendo_bcat_hash";
inline std::mutex InstallMutex;

inline std::filesystem::path StagePath(const std::filesystem::path& target) {
    auto path = target;
    path += ".nextendo-stage";
    return path;
}

inline std::filesystem::path BackupPath(const std::filesystem::path& target) {
    auto path = target;
    path += ".nextendo-backup";
    return path;
}

// A promotion interrupted between its two renames leaves the previous cache in the backup
// folder; put it back so the game still finds it (even if this launch has no network).
// Caller holds InstallMutex.
inline void RecoverInterruptedLocked(const std::filesystem::path& target) {
    std::error_code ec;
    const auto backup = BackupPath(target);
    if (!std::filesystem::exists(target, ec) && !ec &&
        std::filesystem::is_directory(backup, ec)) {
        std::filesystem::rename(backup, target, ec);
    }
}

inline void RecoverInterrupted(const std::filesystem::path& target) {
    std::lock_guard lock{InstallMutex};
    RecoverInterruptedLocked(target);
}

// BCAT names: 1 to 31 characters, letters, digits, '_', '-' and '.'.
inline bool IsValidName(std::string_view name) {
    return !name.empty() && name.size() <= MaxNameLength && name != "." && name != ".." &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
           });
}

inline std::string FoldCase(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

using Contents = std::map<std::string, std::vector<unsigned char>>;

// Read and validate the whole package in memory. Throws on anything that is not a plain
// "<directory>/<file>" delivery cache, before a single byte reaches the disk.
inline Contents ReadPackage(std::span<const unsigned char> bytes) {
#ifdef CITRON_ENABLE_LIBARCHIVE
    if (bytes.empty() || bytes.size() > MaxSize) {
        throw std::runtime_error("Invalid BCAT ZIP size");
    }
    std::unique_ptr<archive, decltype(&archive_read_free)> reader{archive_read_new(),
                                                                  archive_read_free};
    if (!reader || archive_read_support_format_zip(reader.get()) != ARCHIVE_OK ||
        archive_read_open_memory(reader.get(), bytes.data(), bytes.size()) != ARCHIVE_OK) {
        throw std::runtime_error("Cannot open BCAT ZIP");
    }
    Contents contents;
    std::set<std::string> folded_names;
    std::vector<unsigned char> buffer(64 * 1024);
    std::size_t total = 0, count = 0;
    archive_entry* entry = nullptr;
    int status;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        if (++count > MaxEntries || !archive_entry_pathname(entry) ||
            archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
            archive_entry_is_encrypted(entry) != 0) {
            throw std::runtime_error("Invalid BCAT entry");
        }
        std::string name{archive_entry_pathname(entry)};
        const bool directory = archive_entry_filetype(entry) == AE_IFDIR;
        if (directory && !name.empty() && name.back() == '/') {
            name.pop_back();
        }
        const auto slash = name.find('/');
        const auto dir_name = name.substr(0, slash);
        if (!IsValidName(dir_name)) {
            throw std::runtime_error("Unsafe BCAT directory name");
        }
        if (directory && slash == std::string::npos) {
            // Explicit directory entries are optional; folders come from the file paths.
            continue;
        }
        if (directory || archive_entry_filetype(entry) != AE_IFREG ||
            slash == std::string::npos) {
            throw std::runtime_error("BCAT entry is not a <directory>/<file> file");
        }
        const auto file_name = name.substr(slash + 1);
        if (!IsValidName(file_name)) {
            // Also rejects a deeper path: '/' is not a valid name character.
            throw std::runtime_error("Unsafe BCAT file name");
        }
        // Windows paths are case-insensitive: two entries that differ only by case would
        // silently overwrite each other.
        if (!folded_names.insert(FoldCase(name)).second) {
            throw std::runtime_error("Duplicate BCAT file");
        }
        // Zips written with a data descriptor (Go's archive/zip, streamed writers) don't
        // announce the size up front: read to the end either way, then check it if known.
        const bool size_known = archive_entry_size_is_set(entry) != 0;
        const auto declared = archive_entry_size(entry);
        if (size_known &&
            (declared < 0 || static_cast<unsigned long long>(declared) > MaxSize - total)) {
            throw std::runtime_error("Invalid expanded BCAT size");
        }
        auto& data = contents[name];
        if (size_known) {
            data.reserve(static_cast<std::size_t>(declared));
        }
        for (;;) {
            const auto n = archive_read_data(reader.get(), buffer.data(), buffer.size());
            if (n < 0) {
                throw std::runtime_error("Corrupt BCAT file (CRC or deflate error)");
            }
            if (n == 0) {
                break;
            }
            if (static_cast<std::size_t>(n) > MaxSize - total - data.size()) {
                throw std::runtime_error("Invalid expanded BCAT size");
            }
            data.insert(data.end(), buffer.begin(), buffer.begin() + n);
        }
        if (size_known && data.size() != static_cast<std::size_t>(declared)) {
            throw std::runtime_error("BCAT size mismatch");
        }
        total += data.size();
    }
    if (status != ARCHIVE_EOF) {
        throw std::runtime_error("Invalid BCAT archive");
    }
    if (contents.empty()) {
        throw std::runtime_error("Empty BCAT package");
    }
    return contents;
#else
    (void)bytes;
    throw std::runtime_error("BCAT packages require libarchive support");
#endif
}

// Install the package as the title's delivery cache folder `target`, with the package hash
// in HashFileName. The new folder is fully written next to the old one, then swapped in with
// two renames; on any failure the previous cache is left (or put back) in place.
inline void Install(std::span<const unsigned char> bytes, const std::filesystem::path& target,
                    std::string_view hash) {
    const auto contents = ReadPackage(bytes);

    std::lock_guard lock{InstallMutex};
    const auto stage = StagePath(target);
    const auto backup = BackupPath(target);
    RecoverInterruptedLocked(target);
    std::filesystem::remove_all(stage);
    try {
        for (const auto& [name, data] : contents) {
            const auto path = stage / name;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out{path, std::ios::binary | std::ios::trunc};
            out.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
            out.close();
            if (!out) {
                throw std::runtime_error("Cannot write BCAT staging file");
            }
        }
        if (!hash.empty()) {
            std::ofstream out{stage / HashFileName, std::ios::binary | std::ios::trunc};
            out.write(hash.data(), static_cast<std::streamsize>(hash.size()));
            out.close();
            if (!out) {
                throw std::runtime_error("Cannot write BCAT package hash");
            }
        }
        std::filesystem::remove_all(backup);
        const bool had_old = std::filesystem::exists(target);
        if (had_old) {
            std::filesystem::rename(target, backup);
        }
        try {
            std::filesystem::rename(stage, target);
        } catch (...) {
            if (had_old) {
                std::error_code ec;
                std::filesystem::rename(backup, target, ec);
            }
            throw;
        }
        std::error_code ec;
        std::filesystem::remove_all(backup, ec);
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove_all(stage, ec);
        throw;
    }
}
} // namespace Nextendo::PackageBcat
