// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

#ifdef CITRON_ENABLE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

#include "citron/mod_manager/zip_extractor.h"
#include "common/fs/fs.h"
#include "common/logging.h"

namespace ModManager {

bool ZipExtractor::CanExtract() {
#ifdef CITRON_ENABLE_LIBARCHIVE
    return true;
#endif
#ifdef _WIN32
    // Check for PowerShell (built-in) or 7z
    if (!QStandardPaths::findExecutable(QStringLiteral("powershell")).isEmpty()) {
        return true;
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("7z")).isEmpty()) {
        return true;
    }
#else
    // Check for 7z, unzip, or unar
    if (!QStandardPaths::findExecutable(QStringLiteral("7z")).isEmpty()) {
        return true;
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("7za")).isEmpty()) {
        return true;
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("unzip")).isEmpty()) {
        return true;
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("unar")).isEmpty()) {
        return true;
    }
#endif
    return false;
}

bool ZipExtractor::ExtractToPath(const QString& zip_path, const QString& dest_path) {
    QDir().mkpath(dest_path);

#ifdef CITRON_ENABLE_LIBARCHIVE
    {
        struct archive* a = archive_read_new();
        struct archive* ext = archive_write_disk_new();
        if (!a || !ext) {
            if (a)
                archive_read_free(a);
            if (ext)
                archive_write_free(ext);
            return false;
        }

        archive_read_support_format_zip(a);
        archive_read_support_filter_all(a);
        archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
        archive_write_disk_set_standard_lookup(ext);

#ifdef _WIN32
        const auto zip_path_fs = Common::FS::PathFromUTF8(zip_path.toStdString());
        if (archive_read_open_filename_w(a, zip_path_fs.c_str(), 10240) != ARCHIVE_OK) {
#else
        const std::string zip_path_std = zip_path.toStdString();
        if (archive_read_open_filename(a, zip_path_std.c_str(), 10240) != ARCHIVE_OK) {
#endif
            archive_read_free(a);
            archive_write_free(ext);
            return false;
        }

        const std::filesystem::path dest_root = Common::FS::PathFromUTF8(dest_path.toStdString());
        struct archive_entry* entry;
        bool any_entry = false;
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            any_entry = true;
#ifdef _WIN32
            const wchar_t* entry_name = archive_entry_pathname_w(entry);
            const std::filesystem::path entry_path =
                entry_name != nullptr
                    ? dest_root / entry_name
                    : dest_root / Common::FS::PathFromUTF8(archive_entry_pathname(entry));
            archive_entry_copy_pathname_w(entry, entry_path.c_str());
#else
            const std::filesystem::path entry_path =
                dest_root / Common::FS::PathFromUTF8(archive_entry_pathname(entry));
            archive_entry_set_pathname(entry, Common::FS::PathToUTF8String(entry_path).c_str());
#endif

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
        if (any_entry) {
            return true;
        }
        // Fall through to the external-tool paths below if libarchive found nothing
        // (e.g. a format it doesn't recognize) rather than silently reporting success.
    }
#endif

#ifdef _WIN32
    // On Windows, use PowerShell's Expand-Archive for .zip, or 7z for others.
    // Paths go through the environment so a folder outside the console code page still extracts,
    // and the console itself stays hidden.
    if (zip_path.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        QString powershell = QStandardPaths::findExecutable(QStringLiteral("powershell"));
        if (!powershell.isEmpty()) {
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("CITRON_MOD_ZIP"), zip_path);
            env.insert(QStringLiteral("CITRON_MOD_DEST"), dest_path);

            QProcess process;
            process.setProgram(powershell);
            process.setProcessEnvironment(env);
            process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
                args->flags |= CREATE_NO_WINDOW;
            });
            process.setArguments(
                {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                 QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
                 QStringLiteral("-Command"),
                 QStringLiteral("Expand-Archive -LiteralPath $env:CITRON_MOD_ZIP "
                                "-DestinationPath $env:CITRON_MOD_DEST -Force")});
            process.start();
            process.waitForFinished(60000);
            if (process.exitCode() == 0)
                return true;
        }
    }

    // Fallback to 7z
    QString p7z = QStandardPaths::findExecutable(QStringLiteral("7z"));
    if (!p7z.isEmpty()) {
        QProcess process;
        process.setProgram(p7z);
        process.setArguments(
            {QStringLiteral("x"), zip_path, QStringLiteral("-o") + dest_path, QStringLiteral("-y")});
        process.start();
        process.waitForFinished(120000);
        return process.exitCode() == 0;
    }
#else
    // On Linux/macOS, try 7z first
    QString p7z = QStandardPaths::findExecutable(QStringLiteral("7z"));
    if (!p7z.isEmpty()) {
        QProcess process;
        process.setProgram(p7z);
        process.setArguments(
            {QStringLiteral("x"), zip_path, QStringLiteral("-o") + dest_path, QStringLiteral("-y")});
        process.start();
        if (process.waitForFinished(120000) && process.exitCode() == 0) {
            return true;
        }
    }

    // Try unzip for .zip files
    if (zip_path.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        QString unzip = QStandardPaths::findExecutable(QStringLiteral("unzip"));
        if (!unzip.isEmpty()) {
            QProcess process;
            process.setProgram(unzip);
            process.setArguments({QStringLiteral("-o"), zip_path, QStringLiteral("-d"), dest_path});
            process.start();
            if (process.waitForFinished(60000) && process.exitCode() == 0)
                return true;
        }
    }

    // Try 7za
    QString p7za = QStandardPaths::findExecutable(QStringLiteral("7za"));
    if (!p7za.isEmpty()) {
        QProcess process;
        process.setProgram(p7za);
        process.setArguments(
            {QStringLiteral("x"), zip_path, QStringLiteral("-o") + dest_path, QStringLiteral("-y")});
        process.start();
        if (process.waitForFinished(120000) && process.exitCode() == 0)
            return true;
    }

    // Try unar
    QString unar = QStandardPaths::findExecutable(QStringLiteral("unar"));
    if (!unar.isEmpty()) {
        QProcess process;
        process.setProgram(unar);
        process.setArguments({QStringLiteral("-o"), dest_path, QStringLiteral("-f"), zip_path});
        process.start();
        if (process.waitForFinished(120000) && process.exitCode() == 0)
            return true;
    }
#endif

    return false;
}

QStringList ZipExtractor::ListContents(const QString& zip_path) {
    QStringList contents;

#ifdef CITRON_ENABLE_LIBARCHIVE
    {
        struct archive* a = archive_read_new();
        if (a) {
            archive_read_support_format_zip(a);
            archive_read_support_filter_all(a);

#ifdef _WIN32
            const auto zip_path_fs = Common::FS::PathFromUTF8(zip_path.toStdString());
            if (archive_read_open_filename_w(a, zip_path_fs.c_str(), 10240) == ARCHIVE_OK) {
#else
            const std::string zip_path_std = zip_path.toStdString();
            if (archive_read_open_filename(a, zip_path_std.c_str(), 10240) == ARCHIVE_OK) {
#endif
                struct archive_entry* entry;
                while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
                    contents.append(QString::fromUtf8(archive_entry_pathname(entry)));
                    archive_read_data_skip(a);
                }
            }
            archive_read_free(a);
        }
        if (!contents.isEmpty()) {
            return contents;
        }
        // Fall through to the external-tool paths below if libarchive couldn't read it.
    }
#endif

#ifdef _WIN32
    QString p7z = QStandardPaths::findExecutable(QStringLiteral("7z"));
    if (!p7z.isEmpty()) {
        QProcess process;
        process.setProgram(p7z);
        process.setArguments({QStringLiteral("l"), zip_path});
        process.start();
        process.waitForFinished(30000);
        QString output = QString::fromUtf8(process.readAllStandardOutput());
        QStringList lines = output.split(QStringLiteral("\n"));
        bool in_list = false;
        for (const QString& line : lines) {
            if (line.contains(QStringLiteral("-------------------"))) {
                in_list = !in_list;
                continue;
            }
            if (in_list && line.length() > 53) {
                contents.append(line.mid(53).trimmed());
            }
        }
        if (!contents.isEmpty())
            return contents;
    }

    QString powershell = QStandardPaths::findExecutable(QStringLiteral("powershell"));
    if (!powershell.isEmpty()) {
        QProcess process;
        process.setProgram(powershell);
        process.setArguments(
            {QStringLiteral("-Command"),
             QStringLiteral(
                 "(Get-ChildItem -Path (New-Object "
                 "System.IO.Compression.ZipFile)::OpenRead('%1').Entries.FullName) -join \"`n\"")
                 .arg(zip_path)});
        process.start();
        process.waitForFinished(30000);
        QString output = QString::fromUtf8(process.readAllStandardOutput());
        return output.split(QStringLiteral("\n"), Qt::SkipEmptyParts);
    }
#else
    QString p7z = QStandardPaths::findExecutable(QStringLiteral("7z"));
    if (!p7z.isEmpty()) {
        QProcess process;
        process.setProgram(p7z);
        process.setArguments({QStringLiteral("l"), zip_path});
        process.start();
        process.waitForFinished(30000);
        QString output = QString::fromUtf8(process.readAllStandardOutput());

        QStringList lines = output.split(QStringLiteral("\n"));
        bool in_list = false;
        for (const QString& line : lines) {
            if (line.contains(QStringLiteral("-------------------"))) {
                in_list = !in_list;
                continue;
            }
            if (in_list && line.length() > 53) {
                contents.append(line.mid(53).trimmed());
            }
        }
        if (!contents.isEmpty())
            return contents;
    }

    QString unzip = QStandardPaths::findExecutable(QStringLiteral("unzip"));
    if (!unzip.isEmpty()) {
        QProcess process;
        process.setProgram(unzip);
        process.setArguments({QStringLiteral("-l"), zip_path});
        process.start();
        process.waitForFinished(30000);
        QString output = QString::fromUtf8(process.readAllStandardOutput());

        QStringList lines = output.split(QStringLiteral("\n"));
        bool in_file_list = false;
        for (const QString& line : lines) {
            if (line.contains(QStringLiteral("----"))) {
                in_file_list = !in_file_list;
                continue;
            }
            if (in_file_list && !line.trimmed().isEmpty()) {
                QStringList parts =
                    line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
                if (parts.size() >= 4) {
                    contents.append(parts.last());
                }
            }
        }
    }
#endif

    return contents;
}

bool ZipExtractor::HasPrefix(const QStringList& entries, const QString& prefix) {
    for (const QString& entry : entries) {
        if (entry.startsWith(prefix, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

ModStructure ZipExtractor::DetectModStructure(const QString& zip_path) {
    QStringList contents = ListContents(zip_path);

    if (contents.isEmpty()) {
        return ModStructure::Unknown;
    }

    bool has_romfs = HasPrefix(contents, QStringLiteral("romfs/")) ||
                     HasPrefix(contents, QStringLiteral("romfs\\"));
    bool has_exefs = HasPrefix(contents, QStringLiteral("exefs/")) ||
                     HasPrefix(contents, QStringLiteral("exefs\\"));
    bool has_cheats = HasPrefix(contents, QStringLiteral("cheats/")) ||
                      HasPrefix(contents, QStringLiteral("cheats\\"));

    if (!has_romfs && !has_exefs && !has_cheats) {
        for (const QString& entry : contents) {
            if (entry.contains(QStringLiteral("/romfs/"), Qt::CaseInsensitive) ||
                entry.contains(QStringLiteral("\\romfs\\"), Qt::CaseInsensitive)) {
                has_romfs = true;
            }
            if (entry.contains(QStringLiteral("/exefs/"), Qt::CaseInsensitive) ||
                entry.contains(QStringLiteral("\\exefs\\"), Qt::CaseInsensitive)) {
                has_exefs = true;
            }
            if (entry.contains(QStringLiteral("/cheats/"), Qt::CaseInsensitive) ||
                entry.contains(QStringLiteral("\\cheats\\"), Qt::CaseInsensitive)) {
                has_cheats = true;
            }
        }
    }

    if (has_cheats && !has_romfs && !has_exefs) {
        return ModStructure::Cheats;
    }
    if (has_romfs && has_exefs) {
        return ModStructure::Mixed;
    }
    if (has_romfs) {
        return ModStructure::RomFS;
    }
    if (has_exefs) {
        return ModStructure::ExeFS;
    }

    bool only_txt = true;
    for (const QString& entry : contents) {
        QFileInfo info(entry);
        if (info.isDir())
            continue;
        if (info.suffix().compare(QStringLiteral("txt"), Qt::CaseInsensitive) != 0) {
            only_txt = false;
            break;
        }
    }
    if (only_txt && !contents.isEmpty())
        return ModStructure::Cheats;

    return ModStructure::Flat;
}

static void RobustMove(const std::filesystem::path& src, const std::filesystem::path& dst) {
    std::error_code ec;
    if (std::filesystem::exists(dst, ec)) {
        std::filesystem::remove_all(dst, ec);
    }
    std::filesystem::create_directories(dst.parent_path(), ec);

    std::filesystem::rename(src, dst, ec);
    if (ec) {
        std::filesystem::copy(src, dst, std::filesystem::copy_options::recursive, ec);
        if (!ec) {
            std::filesystem::remove_all(src, ec);
        }
    }
}

bool ZipExtractor::ExtractAndOrganize(const QString& zip_path, const QString& mod_folder) {
    QTemporaryDir temp_dir;
    if (!temp_dir.isValid())
        return false;

    if (!ExtractToPath(zip_path, temp_dir.path()))
        return false;

    ModStructure structure = DetectModStructure(zip_path);
    std::filesystem::path src_root(temp_dir.path().toStdString());
    std::filesystem::path dst_root(mod_folder.toStdString());

    QDir temp(temp_dir.path());
    QStringList entries = temp.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    if (entries.size() == 1 && QFileInfo(temp.absoluteFilePath(entries[0])).isDir()) {
        src_root /= entries[0].toStdString();
    }

    if (structure == ModStructure::Flat) {
        std::filesystem::path romfs_dst = dst_root / "romfs";
        RobustMove(src_root, romfs_dst);
    } else if (structure == ModStructure::Cheats) {
        std::filesystem::path cheats_src = src_root / "cheats";
        if (!std::filesystem::exists(cheats_src)) {
            std::filesystem::path cheats_dst = dst_root / "cheats";
            RobustMove(src_root, cheats_dst);
        } else {
            RobustMove(src_root, dst_root);
        }
    } else {
        RobustMove(src_root, dst_root);
    }

    return true;
}

} // namespace ModManager
