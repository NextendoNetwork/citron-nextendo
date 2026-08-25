// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/uisettings.h"
#include "citron/updater/updater_service.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scm_rev.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStandardPaths>

#include <fstream>
#include <regex>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace Updater {

// /releases omits nightly-windows entirely even though it's fetchable by tag -- fetch by tag.
const std::string NIGHTLY_UPDATE_URL =
#if defined(_WIN32)
    "https://api.github.com/repos/NextendoNetwork/citron-nextendo/releases/tags/nightly-windows";
#elif defined(__linux__)
    "https://api.github.com/repos/NextendoNetwork/citron-nextendo/releases/tags/nightly-linux";
#else
    "https://api.github.com/repos/NextendoNetwork/citron-nextendo/releases";
#endif

std::string ExtractCommitHash(const std::string& version_string) {
    // Hashes in git describe often start with 'g'.
    // We match 7-40 hex characters, optionally preceded by 'g'.
    std::regex re("(?:\\b|[gG])([0-9a-fA-F]{7,40})\\b");
    std::smatch match;
    if (std::regex_search(version_string, match, re) && match.size() > 1) {
        return match[1].str();
    }
    return "";
}

std::string ExtractVersionTag(const std::string& version_string) {
    // Matches tag parts like v1.2.3 or 2026.02.1 at the start of the string.
    // We stop at the first hyphen to avoid the -count-gHASH part.
    // We also ensure it's not just a hex character that could be part of a hash.
    std::regex re("^v?([0-9]+\\.[0-9.]*)");
    std::smatch match;
    if (std::regex_search(version_string, match, re) && match.size() > 1) {
        return match[1].str();
    }
    return "";
}

// Helper function to calculate the SHA256 hash of a file.
QByteArray GetFileChecksum(const std::filesystem::path& file_path) {
    QFile file(QString::fromStdString(file_path.string()));
    if (file.open(QIODevice::ReadOnly)) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (hash.addData(&file)) {
            return hash.result();
        }
    }
    return QByteArray();
}

UpdaterService::UpdaterService(QObject* parent) : QObject(parent) {
    network_manager = std::make_unique<QNetworkAccessManager>(this);
    InitializeSSL();
    app_directory = GetApplicationDirectory();
    temp_download_path = GetTempDirectory();
    backup_path = GetBackupDirectory();
    EnsureDirectoryExists(temp_download_path);
    EnsureDirectoryExists(backup_path);
    LOG_INFO(Frontend, "UpdaterService initialized");
}

UpdaterService::~UpdaterService() {
    if (current_reply) {
        current_reply->abort();
        current_reply->deleteLater();
    }
    CleanupFiles();
}

void UpdaterService::InitializeSSL() {
    LOG_INFO(Frontend, "Attempting to initialize SSL support...");

    // Check if SSL is supported
    if (!QSslSocket::supportsSsl()) {
        LOG_WARNING(Frontend, "SSL support not available");
        LOG_WARNING(Frontend, "Build-time SSL version: {}",
                    QSslSocket::sslLibraryBuildVersionString().toStdString());
        LOG_WARNING(Frontend, "Runtime SSL version: {}",
                    QSslSocket::sslLibraryVersionString().toStdString());

#ifdef _WIN32
        // Try to provide helpful information about missing DLLs
        std::filesystem::path app_dir =
            std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
        std::filesystem::path crypto_dll = app_dir / "libcrypto-3-x64.dll";
        std::filesystem::path ssl_dll = app_dir / "libssl-3-x64.dll";

        LOG_WARNING(Frontend, "libcrypto-3-x64.dll exists: {}",
                    std::filesystem::exists(crypto_dll));
        LOG_WARNING(Frontend, "libssl-3-x64.dll exists: {}", std::filesystem::exists(ssl_dll));
#endif
        return;
    }

    LOG_INFO(Frontend, "SSL library version: {}",
             QSslSocket::sslLibraryVersionString().toStdString());

    QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
    auto certs = QSslConfiguration::systemCaCertificates();
    if (!certs.isEmpty()) {
        sslConfig.setCaCertificates(certs);
    } else {
        LOG_WARNING(Frontend, "No system CA certificates available");
    }
    sslConfig.setProtocol(QSsl::SecureProtocols);
    QSslConfiguration::setDefaultConfiguration(sslConfig);
    LOG_INFO(Frontend, "SSL initialized successfully");
}

void UpdaterService::CheckForUpdates() {
    if (update_in_progress.load()) {
        emit UpdateError(QStringLiteral("Update operation already in progress"));
        return;
    }
    const QString channel = QStringLiteral("Nightly");
    const std::string update_url = NIGHTLY_UPDATE_URL;
    LOG_INFO(Frontend, "Checking for updates from: {}", update_url);
    QUrl url{QString::fromStdString(update_url)};
    QNetworkRequest request{url};
    request.setRawHeader("User-Agent", QByteArrayLiteral("Citron-Updater/1.0"));
    request.setRawHeader("Accept", QByteArrayLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    QNetworkReply* reply = network_manager->get(request);
    current_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, channel]() {
        if (current_reply == reply) {
            current_reply = nullptr;
        }
        if (reply->error() == QNetworkReply::NoError) {
            ParseUpdateResponse(reply->readAll(), channel);
        } else {
            emit UpdateError(QStringLiteral("Update check failed: %1").arg(reply->errorString()));
        }
        reply->deleteLater();
    });
}

void UpdaterService::ConfigureSSLForRequest(QNetworkRequest& request) {
    if (!QSslSocket::supportsSsl())
        return;
    QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    sslConfig.setProtocol(QSsl::SecureProtocols);
    request.setSslConfiguration(sslConfig);
}

void UpdaterService::DownloadAndInstallUpdate(const std::string& download_url) {
    if (update_in_progress.load()) {
        emit UpdateError(QStringLiteral("Update operation already in progress"));
        return;
    }
    if (download_url.empty()) {
        emit UpdateError(QStringLiteral("Invalid download URL."));
        return;
    }

    update_in_progress.store(true);
    cancel_requested.store(false);

    LOG_INFO(Frontend, "Starting update download from {}", download_url);

    QUrl url(QString::fromStdString(download_url));
    QNetworkRequest request(url);
    // GitHub's release CDN blocks non-browser User-Agents; see ssbu_mod_installer.cpp.
    request.setRawHeader("User-Agent",
                         QByteArrayLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                           "AppleWebKit/537.36 (KHTML, like Gecko) "
                                           "Chrome/131.0.0.0 Safari/537.36"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    ConfigureSSLForRequest(request);
    current_reply = network_manager->get(request);
    connect(current_reply, &QNetworkReply::downloadProgress, this,
            &UpdaterService::OnDownloadProgress);
    connect(current_reply, &QNetworkReply::finished, this, &UpdaterService::OnDownloadFinished);
    connect(current_reply, &QNetworkReply::errorOccurred, this, &UpdaterService::OnDownloadError);
}

void UpdaterService::CancelUpdate() {
    if (!update_in_progress.load())
        return;
    cancel_requested.store(true);
    if (current_reply) {
        current_reply->abort();
    }
    LOG_INFO(Frontend, "Update cancelled by user");
    emit UpdateCompleted(UpdateResult::Cancelled, QStringLiteral("Update cancelled by user"));
    update_in_progress.store(false);
}

void UpdaterService::AbortCheck() {
    if (current_reply) {
        current_reply->abort();
        current_reply = nullptr;
    }
}

std::string UpdaterService::GetCurrentVersion() const {
    const QString channel = QStringLiteral("Nightly");

    const std::string build_version = Common::g_build_version;

    // First priority: version.txt (only relevant for Stable installations)
    std::filesystem::path search_path;
#ifdef __linux__
    const char* appimage_path_env = qgetenv("APPIMAGE").constData();
    if (appimage_path_env && strlen(appimage_path_env) > 0) {
        search_path = std::filesystem::path(appimage_path_env).parent_path();
    } else {
        search_path = app_directory;
    }
#else
    search_path = app_directory;
#endif

    std::filesystem::path version_file = search_path / CITRON_VERSION_FILE;
    if (std::filesystem::exists(version_file)) {
        std::ifstream file(version_file);
        if (file.is_open()) {
            std::string version_from_file;
            std::getline(file, version_from_file);
            if (!version_from_file.empty()) {
                // Trim trailing metadata/whitespace from version.txt (e.g. "1.0.0 (Release)")
                return version_from_file.substr(0, version_from_file.find_first_of(" \t\r\n"));
            }
        }
    }

    // If the user's setting is Nightly, we prioritize the commit hash.
    if (channel == QStringLiteral("Nightly")) {
        if (!build_version.empty()) {
            std::string hash = ExtractCommitHash(build_version);
            if (!hash.empty()) {
                return hash;
            }
        }
    } else {
        // Otherwise (channel is Stable), we try to extract the tag from build_version if
        // version.txt is missing. This happens when a Nightly user checks for Stable updates.
        std::string tag = ExtractVersionTag(build_version);
        if (!tag.empty()) {
            return tag;
        }
    }

    // Common fallback: try to extract a hash if we haven't found a tag yet,
    // otherwise just return the full build version.
    std::string hash_fallback = ExtractCommitHash(build_version);
    if (!hash_fallback.empty()) {
        return hash_fallback;
    }

    return build_version;
}

bool UpdaterService::IsUpdateInProgress() const {
    return update_in_progress.load();
}

bool UpdaterService::IsPgoBuild() {
#ifdef CITRON_ENABLE_PGO_USE
    return true;
#else
    return false;
#endif
}

bool UpdaterService::CheckPgoWarning(QWidget* parent) {
    if (!IsPgoBuild()) {
        return true;
    }
    QMessageBox::StandardButton answer =
        QMessageBox::warning(parent, tr("PGO Build Detected"),
                             tr("You are currently running a highly optimized PGO (Profile-Guided "
                                "Optimization) build of Citron. "
                                "Updating will replace your current binary with a standard "
                                "release, and you will lose your PGO optimizations.\n\n"
                                "Do you wish to proceed with the update?"),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return answer == QMessageBox::Yes;
}

void UpdaterService::OnDownloadFinished() {
    if (cancel_requested.load() || !current_reply) {
        update_in_progress.store(false);
        return;
    }
    if (current_reply->error() != QNetworkReply::NoError) {
        emit UpdateError(QStringLiteral("Download failed: %1").arg(current_reply->errorString()));
        update_in_progress.store(false);
        return;
    }

    QByteArray downloaded_data = current_reply->readAll();
    const QString channel = QStringLiteral("Nightly");

#if defined(_WIN32)
    QString filename = QStringLiteral("citron_update_%1.zip")
                           .arg(QString::fromStdString(current_update_info.version));
    std::filesystem::path download_path = temp_download_path / filename.toStdString();
    QFile file(QString::fromStdString(download_path.string()));
    if (!file.open(QIODevice::WriteOnly)) {
        emit UpdateCompleted(UpdateResult::Failed,
                             QStringLiteral("Failed to save downloaded file"));
        update_in_progress.store(false);
        return;
    }
    file.write(downloaded_data);
    file.close();
    LOG_INFO(Frontend, "Download completed: {}", download_path.string());

    pending_update_zip_path = download_path;
    const QString backup_path_str = QString::fromStdString(GetPendingBackupPath().string());
    emit UpdateInstallProgress(
        100, QStringLiteral("Your current version will be backed up to:\n%1").arg(backup_path_str));
    emit UpdateCompleted(
        UpdateResult::Success,
        QStringLiteral("Update downloaded successfully. Citron Neo will restart to apply it."));
    update_in_progress.store(false);
#elif defined(__linux__)

    LOG_INFO(Frontend, "AppImage download completed.");

    const char* appimage_path_env = qgetenv("APPIMAGE").constData();
    if (!appimage_path_env || strlen(appimage_path_env) == 0) {
        emit UpdateError(QStringLiteral("Failed to update: Not running from an AppImage."));
        update_in_progress.store(false);
        return;
    }

    std::filesystem::path original_appimage_path = appimage_path_env;
    std::filesystem::path appimage_dir = original_appimage_path.parent_path();
    std::error_code ec;

    // Check if backups are enabled before doing anything.
    if (UISettings::values.updater_enable_backups.GetValue()) {
        const std::string& custom_backup_path = UISettings::values.updater_backup_path.GetValue();
        std::filesystem::path backup_dir;

        if (!custom_backup_path.empty()) {
            // User has specified a custom path.
            backup_dir = custom_backup_path;
        } else {
            // Default behavior: create 'backup' folder next to the AppImage.
            backup_dir = appimage_dir / "backup";
        }

        // Create the backup directory
        std::filesystem::create_directories(backup_dir, ec);
        if (ec) {
            LOG_ERROR(Frontend, "Failed to create backup directory: {}", ec.message());
        } else {
            // Create the backup copy of the old AppImage
            std::string current_version = GetCurrentVersion();
            std::string backup_filename = "citron-backup-" +
                                          (current_version.empty() ? "unknown" : current_version) +
                                          ".AppImage";
            std::filesystem::path backup_filepath = backup_dir / backup_filename;
            std::filesystem::copy_file(original_appimage_path, backup_filepath,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                LOG_ERROR(Frontend, "Failed to copy AppImage to backup location: {}", ec.message());
            } else {
                LOG_INFO(Frontend, "Created backup of old AppImage at: {}",
                         backup_filepath.string());
                emit UpdateInstallProgress(
                    50, QStringLiteral("Created backup of old AppImage at: %1")
                            .arg(QString::fromStdString(backup_filepath.string())));
            }
        }
    }

    std::filesystem::path new_appimage_path = original_appimage_path.string() + ".new";
    QFile new_file(QString::fromStdString(new_appimage_path.string()));
    if (!new_file.open(QIODevice::WriteOnly)) {
        emit UpdateError(QStringLiteral("Failed to save new AppImage version."));
        update_in_progress.store(false);
        return;
    }
    new_file.write(downloaded_data);
    new_file.close();

    if (!new_file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                 QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                 QFileDevice::ExeOther)) {
        emit UpdateError(QStringLiteral("Failed to make the new AppImage executable."));
        std::filesystem::remove(new_appimage_path, ec);
        update_in_progress.store(false);
        return;
    }

    std::filesystem::rename(new_appimage_path, original_appimage_path, ec);
    if (ec) {
        LOG_WARNING(Frontend,
                    "std::filesystem::rename failed: {}. Attempting stage 1 fallback (copy)...",
                    ec.message());
        std::filesystem::copy_file(new_appimage_path, original_appimage_path,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_WARNING(
                Frontend,
                "copy_file failed: {}. Attempting stage 2 fallback (move running binary)...",
                ec.message());
            std::filesystem::path old_appimage_path = original_appimage_path.string() + ".old";
            std::filesystem::rename(original_appimage_path, old_appimage_path, ec);
            if (!ec) {
                std::filesystem::rename(new_appimage_path, original_appimage_path, ec);
            }
        } else {
            std::filesystem::remove(new_appimage_path, ec);
            ec.clear(); // Copy succeeded!
        }

        if (ec) {
            LOG_ERROR(Frontend, "All AppImage replacement fallbacks failed: {}", ec.message());
            emit UpdateError(QStringLiteral("Failed to replace old AppImage."));
            update_in_progress.store(false);
            return;
        }
    }

    std::filesystem::path version_file_path = appimage_dir / CITRON_VERSION_FILE;
    if (channel == QStringLiteral("Stable")) {
        LOG_INFO(Frontend, "Writing stable version marker: {}", current_update_info.version);
        std::ofstream version_file(version_file_path);
        if (version_file.is_open()) {
            version_file << current_update_info.version;
        }
    } else {
        LOG_INFO(Frontend, "Nightly update, removing stable version marker if it exists.");
        if (std::filesystem::exists(version_file_path)) {
            std::filesystem::remove(version_file_path, ec);
        }
    }

    LOG_INFO(Frontend, "AppImage updated successfully.");
    emit UpdateCompleted(UpdateResult::Success,
                         QStringLiteral("Update successful. Please restart the application."));
    update_in_progress.store(false);
#endif
}

void UpdaterService::OnDownloadProgress(qint64 bytes_received, qint64 bytes_total) {
    if (bytes_total > 0) {
        emit UpdateDownloadProgress(static_cast<int>((bytes_received * 100) / bytes_total),
                                    bytes_received, bytes_total);
    } else if (bytes_received > 0) {
        emit UpdateDownloadProgress(0, bytes_received, bytes_received);
    }
}

void UpdaterService::OnDownloadError(QNetworkReply::NetworkError) {
    if (current_reply) {
        emit UpdateError(QStringLiteral("Network error: %1").arg(current_reply->errorString()));
    }
    update_in_progress.store(false);
}

void UpdaterService::ParseUpdateResponse(const QByteArray& response, const QString& channel) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(response, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        emit UpdateError(QStringLiteral("Failed to parse update response."));
        return;
    }

#if defined(__linux__)
    QString current_appimage = QString::fromUtf8(qgetenv("APPIMAGE"));
    QString current_variant;
    if (current_appimage.contains(QStringLiteral("x86_64_v3"), Qt::CaseInsensitive)) {
        current_variant = QStringLiteral("x86_64_v3");
    } else if (current_appimage.contains(QStringLiteral("aarch64"), Qt::CaseInsensitive) ||
               current_appimage.contains(QStringLiteral("arm64"), Qt::CaseInsensitive)) {
        current_variant = QStringLiteral("aarch64");
    } else {
        current_variant = QStringLiteral("x86_64");
    }
#endif

    // NIGHTLY_UPDATE_URL already points at this platform's specific release-by-tag, so the
    // response is a single release object, not a list to scan.
    QJsonObject release_obj = doc.object();
    std::string latest_version;
    if (channel == QStringLiteral("Stable")) {
        latest_version = release_obj.value(QStringLiteral("tag_name")).toString().toStdString();
    } else {
        latest_version = ExtractCommitHash(
            release_obj.value(QStringLiteral("name")).toString().toStdString());
    }

    if (latest_version.empty()) {
        emit UpdateError(QStringLiteral("Could not find a recent update for your platform."));
        return;
    }

    UpdateInfo update_info;
    update_info.version = latest_version;
    update_info.changelog = release_obj.value(QStringLiteral("body")).toString().toStdString();
    update_info.release_date =
        release_obj.value(QStringLiteral("published_at")).toString().toStdString();

    QJsonArray assets = release_obj.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue& asset_value : assets) {
        QJsonObject asset_obj = asset_value.toObject();
        QString asset_name = asset_obj.value(QStringLiteral("name")).toString();

#if defined(__linux__)
        if (asset_name.endsWith(QStringLiteral(".AppImage"), Qt::CaseInsensitive)) {
            bool match_variant = false;
            if (current_variant == QStringLiteral("x86_64_v3")) {
                match_variant =
                    asset_name.contains(QStringLiteral("x86_64_v3"), Qt::CaseInsensitive);
            } else if (current_variant == QStringLiteral("aarch64")) {
                match_variant =
                    asset_name.contains(QStringLiteral("aarch64"), Qt::CaseInsensitive) ||
                    asset_name.contains(QStringLiteral("arm64"), Qt::CaseInsensitive);
            } else {
                match_variant =
                    asset_name.contains(QStringLiteral("x86_64"), Qt::CaseInsensitive) &&
                    !asset_name.contains(QStringLiteral("x86_64_v3"), Qt::CaseInsensitive);
            }
            if (match_variant) {
                std::string asset_hash = ExtractCommitHash(asset_name.toStdString());
                if (!asset_hash.empty()) {
                    update_info.version = asset_hash;
                }
                DownloadOption option;
                option.name = asset_name.toStdString();
                option.url = asset_obj.value(QStringLiteral("browser_download_url"))
                                 .toString()
                                 .toStdString();
                update_info.download_options.push_back(option);
            }
        }
#elif defined(_WIN32)
        // For Windows, find the .zip file but explicitly skip PGO builds.
        if (asset_name.endsWith(QStringLiteral(".zip")) &&
            !asset_name.contains(QStringLiteral("PGO"), Qt::CaseInsensitive)) {
            std::string asset_hash = ExtractCommitHash(asset_name.toStdString());
            if (!asset_hash.empty()) {
                update_info.version = asset_hash;
            }
            DownloadOption option;
            option.name = asset_name.toStdString();
            option.url = asset_obj.value(QStringLiteral("browser_download_url"))
                             .toString()
                             .toStdString();
            update_info.download_options.push_back(option);
        }
#endif
    }

    if (!update_info.download_options.empty()) {
        update_info.is_newer_version = CompareVersions(GetCurrentVersion(), update_info.version);
        current_update_info = update_info;
        emit UpdateCheckCompleted(update_info.is_newer_version, update_info);
        return;
    }
    emit UpdateError(QStringLiteral("Could not find a recent update for your platform."));
}

bool UpdaterService::CompareVersions(const std::string& current, const std::string& latest) const {
    if (current.empty()) {
        return true;
    }
    if (latest.empty()) {
        return false;
    }

    bool is_newer = (current != latest);
    return is_newer;
}

#ifdef _WIN32
std::filesystem::path UpdaterService::GetPendingBackupPath() const {
    return backup_path / ("backup_" + current_update_info.version);
}

bool UpdaterService::LaunchUpdateHelper() {
    const std::filesystem::path helper_path = app_directory / "citron-updater-helper.exe";
    if (!std::filesystem::exists(helper_path)) {
        LOG_ERROR(Frontend, "Update helper executable not found at: {}", helper_path.string());
        return false;
    }
    if (pending_update_zip_path.empty() || !std::filesystem::exists(pending_update_zip_path)) {
        LOG_ERROR(Frontend, "No pending update archive to hand off to the helper.");
        return false;
    }

    QStringList arguments;
    arguments << QStringLiteral("--pid") << QString::number(QCoreApplication::applicationPid());
    arguments << QStringLiteral("--zip")
              << QString::fromStdString(pending_update_zip_path.string());
    arguments << QStringLiteral("--app-dir") << QString::fromStdString(app_directory.string());
    arguments << QStringLiteral("--version")
              << QString::fromStdString(current_update_info.version);

    const bool launched =
        QProcess::startDetached(QString::fromStdString(helper_path.string()), arguments);
    if (launched) {
        LOG_INFO(Frontend, "Update helper launched successfully: {}", helper_path.string());
    } else {
        LOG_ERROR(Frontend, "Failed to launch update helper executable.");
    }
    return launched;
}
#endif

bool UpdaterService::CleanupFiles() {
    try {
        // Skip while handed off to the helper -- don't delete the archive out from under it.
        if (pending_update_zip_path.empty() && std::filesystem::exists(temp_download_path)) {
            std::filesystem::remove_all(temp_download_path);
        }
#ifdef _WIN32
        std::vector<std::filesystem::path> backup_dirs;
        if (std::filesystem::exists(backup_path)) {
            for (const auto& entry : std::filesystem::directory_iterator(backup_path)) {
                if (entry.is_directory() &&
                    entry.path().filename().string().starts_with("backup_")) {
                    backup_dirs.push_back(entry.path());
                }
            }
        }
        if (backup_dirs.size() > 3) {
            std::sort(backup_dirs.begin(), backup_dirs.end(), [](const auto& a, const auto& b) {
                return std::filesystem::last_write_time(a) > std::filesystem::last_write_time(b);
            });
            for (size_t i = 3; i < backup_dirs.size(); ++i) {
                std::filesystem::remove_all(backup_dirs[i]);
            }
        }
#endif
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(Frontend, "Failed to cleanup files: {}", e.what());
        return false;
    }
}

std::filesystem::path UpdaterService::GetTempDirectory() const {
    return std::filesystem::path(
               QStandardPaths::writableLocation(QStandardPaths::TempLocation).toStdString()) /
           "citron_updater";
}

std::filesystem::path UpdaterService::GetApplicationDirectory() const {
    return std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
}

std::filesystem::path UpdaterService::GetBackupDirectory() const {
    return GetApplicationDirectory() / BACKUP_DIRECTORY;
}

bool UpdaterService::EnsureDirectoryExists(const std::filesystem::path& path) const {
    try {
        if (!std::filesystem::exists(path)) {
            std::filesystem::create_directories(path);
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(Frontend, "Failed to create directory {}: {}", path.string(), e.what());
        return false;
    }
}

bool UpdaterService::HasStagedUpdate(const std::filesystem::path& app_directory) {
#ifdef _WIN32
    std::filesystem::path staging_path = app_directory / "update_staging";
    std::filesystem::path manifest_file = staging_path / "update_manifest.txt";
    return std::filesystem::exists(staging_path) && std::filesystem::exists(manifest_file) &&
           std::filesystem::is_directory(staging_path);
#else
    return false;
#endif
}

bool UpdaterService::ApplyStagedUpdate(const std::filesystem::path& app_directory) {
#ifdef _WIN32
    try {
        std::filesystem::path staging_path = app_directory / "update_staging";
        std::filesystem::path manifest_file = staging_path / "update_manifest.txt";
        if (!std::filesystem::exists(staging_path) || !std::filesystem::exists(manifest_file))
            return false;
        LOG_INFO(Frontend, "Applying staged update from: {}", staging_path.string());
        std::filesystem::path backup_path_dir = app_directory / "backup_before_update";
        if (std::filesystem::exists(backup_path_dir))
            std::filesystem::remove_all(backup_path_dir);
        std::filesystem::create_directories(backup_path_dir);
        for (const auto& entry : std::filesystem::recursive_directory_iterator(staging_path)) {
            if (entry.path().filename() == "update_manifest.txt")
                continue;
            if (entry.is_regular_file()) {
                std::filesystem::path relative_path =
                    std::filesystem::relative(entry.path(), staging_path);
                std::filesystem::path dest_path = app_directory / relative_path;
                if (std::filesystem::exists(dest_path)) {
                    std::filesystem::path backup_dest = backup_path_dir / relative_path;
                    std::filesystem::create_directories(backup_dest.parent_path());
                    std::filesystem::copy_file(dest_path, backup_dest);
                }
                std::filesystem::create_directories(dest_path.parent_path());
                std::filesystem::copy_file(entry.path(), dest_path,
                                           std::filesystem::copy_options::overwrite_existing);
            }
        }
        std::ifstream manifest(manifest_file);
        std::string line, version;
        while (std::getline(manifest, line)) {
            if (line.starts_with("UPDATE_VERSION=")) {
                version = line.substr(15);
                break;
            }
        }
        if (!version.empty()) {
            std::filesystem::path version_file = app_directory / "version.txt";
            std::ofstream vfile(version_file);
            if (vfile.is_open())
                vfile << version;
        }
        std::filesystem::remove_all(staging_path);
        LOG_INFO(Frontend, "Update applied successfully. Version: {}", version);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(Frontend, "Failed to apply staged update: {}", e.what());
        return false;
    }
#else
    return false;
#endif
}

} // namespace Updater

#include "updater_service.moc"
