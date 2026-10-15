// SPDX-License-Identifier: GPL-2.0-or-later
// Release packages and extracted executables are verified before copying.
namespace HomebrewSetup {
struct Asset {
    const char* url;
    const char* hash;
    const char* file;
    const char* member;
    const char* destination;
    qint64 size;
    const char* binary_hash;
};
static constexpr Asset assets[]{
        {"https://github.com/switchbrew/nx-hbmenu/releases/download/v3.6.1/nx-hbmenu_v3.6.1.zip", "4790c58fdd75c4bcc644fa8135fa11c380cb3ebaf685e400e336031972410ee7", "nx-hbmenu_v3.6.1.zip", "hbmenu.nro", "hbmenu.nro", 1249201, "6cf6d130515723ebe0041c2479420caf9726895087439ff7b2e528c2ed39c100"},
        {"https://github.com/switchbrew/nx-hbloader/releases/download/v2.4.5/hbl.nsp", "853df8526557b82c6b2018a8a102d22e8306687415e49653530dfd9a1d603b32", "hbl.nsp", "hbl.nsp", "hbl.nsp", 40946, "853df8526557b82c6b2018a8a102d22e8306687415e49653530dfd9a1d603b32"},
        {"https://github.com/exelix11/SwitchThemeInjector/releases/download/nxt-3.0.2/NXThemesInstaller.nro", "36ac0fac5477e08a176b2f155b8b3c170c1291004d943541348599bae8cf97a1", "NXThemesInstaller.nro", "NXThemesInstaller.nro", "switch/NXThemesInstaller/NXThemesInstaller.nro", 10467778, "36ac0fac5477e08a176b2f155b8b3c170c1291004d943541348599bae8cf97a1"},
        {"https://github.com/suchmememanyskill/themezer-nx/releases/download/3.3.1/themezer-nx.nro", "c83741eafe3a999e717b056055d42df91369555d94285077ce6c117b16c0fe62", "themezer-nx.nro", "themezer-nx.nro", "switch/ThemezerNX/themezer-nx.nro", 9730240, "c83741eafe3a999e717b056055d42df91369555d94285077ce6c117b16c0fe62"},
        {"https://github.com/NaGaa95/sphaira/releases/download/1.0.8/sphaira.zip", "3e7ebacd421917d426cc1ab49622ec4a1ea7943fb571d62aea7176089dc1384c", "sphaira.zip", "sphaira.nro", "switch/sphaira/sphaira.nro", 2369894, "687a7a7203a4389e886a185ec05e98e2068c3749c9681b593f2484d38b2c1a15"},
        {"https://github.com/J-D-K/JKSV/releases/download/12/02/2025/JKSV.nro", "a2b7f6372a4b81f3c3194aa6f96f2824bdea853b9a8ebf8c4a008d0d865a0cbf", "JKSV.nro", "JKSV.nro", "switch/JKSV/JKSV.nro", 10130471, "a2b7f6372a4b81f3c3194aa6f96f2824bdea853b9a8ebf8c4a008d0d865a0cbf"},
};
static bool ValidBinary(const QByteArray& bytes, const Asset& asset) {
    return bytes.size() >= 0x80 && (QString::fromUtf8(asset.destination).endsWith(QStringLiteral(".nro"))
        ? bytes.mid(0x10, 4) == QByteArrayLiteral("NRO0") : bytes.left(4) == QByteArrayLiteral("PFS0"));
}
static bool FileReady(const QString& path, const Asset& asset) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.size() >= 0x80 && ValidBinary(file.read(0x80), asset);
}
static QString SdRoot() {
    return QString::fromStdString(Common::FS::GetCitronPath(Common::FS::CitronPath::SDMCDir).generic_string());
}
static bool AllReady() {
    const auto sdmc = SdRoot();
    return std::all_of(std::begin(assets), std::end(assets), [&](const auto& asset) {
        return FileReady(sdmc + QLatin1Char('/') + QString::fromUtf8(asset.destination), asset);
    });
}
static bool HashMatches(const QByteArray& bytes, const char* expected) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == QByteArray(expected);
}

template <typename Extract>
bool Prepare(QWidget* parent, const Extract& extract, bool show_success = true) {
    const auto sdmc = SdRoot();
    QProgressDialog progress(QObject::tr("Preparing homebrew tools..."), QObject::tr("Cancel"),
                             0, static_cast<int>(std::size(assets)), parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    QNetworkAccessManager manager;
    QStringList results;
    bool complete = true;
    int count = 0;
    for (const auto& asset : assets) {
        const QString name = QString::fromUtf8(asset.member);
        progress.setLabelText(QObject::tr("Preparing %1...").arg(name));
        progress.setValue(count++);
        QApplication::processEvents();
        if (progress.wasCanceled()) {
            complete = false;
            results << QObject::tr("Preparation canceled. Your saved configuration is unchanged.");
            break;
        }
        const QString destination = sdmc + QLatin1Char('/') + QString::fromUtf8(asset.destination);
        const auto prepare_one = [&]() -> QString {
            if (QFileInfo::exists(destination)) {
                return FileReady(destination, asset) ? QString{} :
                    QObject::tr("Existing file is invalid and was preserved: %1").arg(destination);
            }
            QByteArray bytes;
            const QString cache = QCoreApplication::applicationDirPath() + QStringLiteral("/homebrew/");
            QFile raw(cache + QStringLiteral("unpacked/") + name);
            if (raw.open(QIODevice::ReadOnly) && raw.size() < 64 * 1024 * 1024) bytes = raw.readAll();
            bool binary_ready = HashMatches(bytes, asset.binary_hash);
            if (!binary_ready) {
                bytes.clear();
                QFile bundled(cache + QString::fromUtf8(asset.file));
                if (bundled.open(QIODevice::ReadOnly) && bundled.size() == asset.size) bytes = bundled.readAll();
                const auto verified = [&] { return bytes.size() == asset.size && HashMatches(bytes, asset.hash); };
                if (!verified() && !Settings::values.airplane_mode.GetValue()) {
                    bytes.clear();
                    QNetworkRequest request(QUrl(QString::fromUtf8(asset.url)));
                    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                    request.setRawHeader("User-Agent", "Citron-Homebrew-Setup");
                    request.setTransferTimeout(60000);
                    auto* reply = manager.get(request);
                    QEventLoop loop;
                    QTimer deadline;
                    deadline.setSingleShot(true);
                    QObject::connect(&deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
                    QObject::connect(&progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
                    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
                        bytes += reply->readAll();
                        if (bytes.size() > asset.size) reply->abort();
                    });
                    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
                    deadline.start(60000);
                    loop.exec();
                    bytes += reply->readAll();
                    if (reply->error() != QNetworkReply::NoError) bytes.clear();
                    delete reply;
                }
                if (!verified()) return QObject::tr("Official package is missing, unavailable, or failed verification.");
                if (QString::fromUtf8(asset.file).endsWith(QStringLiteral(".zip"))) {
                    QTemporaryDir staging(Citron::TemporaryDirectoryTemplate());
                    if (!staging.isValid()) return QObject::tr("Could not create the temporary extraction folder: %1").arg(staging.errorString());
                    const QString zip = staging.path() + QStringLiteral("/release.zip");
                    QFile file(zip);
                    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
                        return QObject::tr("Could not write the temporary package: %1").arg(file.errorString());
                    file.close();
                    const QString unpacked = staging.path() + QStringLiteral("/unpacked");
                    if (!extract(zip.toStdString(), unpacked.toStdString()))
                        return QObject::tr("Could not extract the official package.");
                    QDirIterator it(unpacked, QStringList{name}, QDir::Files, QDirIterator::Subdirectories);
                    if (!it.hasNext()) return QObject::tr("The extracted package has no %1.").arg(name);
                    QFile nro(it.next());
                    if (it.hasNext()) return QObject::tr("The extracted package has multiple files named %1.").arg(name);
                    if (!nro.open(QIODevice::ReadOnly)) return QObject::tr("Could not read the extracted NRO.");
                    bytes = nro.readAll();
                }
                binary_ready = HashMatches(bytes, asset.binary_hash);
            }
            if (!binary_ready || !ValidBinary(bytes, asset)) return QObject::tr("The homebrew executable failed verification.");
            if (!QDir{}.mkpath(QFileInfo(destination).absolutePath()))
                return QObject::tr("Could not create the destination folder: %1").arg(QFileInfo(destination).absolutePath());
            QSaveFile output(destination);
            if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
                return QObject::tr("Could not save the file: %1").arg(output.errorString());
            return {};
        };
        const auto error = prepare_one();
        if (!error.isEmpty()) { complete = false; results << name + QStringLiteral(": ") + error; }
        else results << QObject::tr("%1: ready").arg(name);
    }
    complete = complete && AllReady();
    progress.setValue(static_cast<int>(std::size(assets)));
    if (!complete || show_success) {
        QMessageBox::information(parent, QObject::tr("Homebrew tools"),
            (complete ? QObject::tr("Homebrew tools are ready. From HOME, press Ctrl+H or L+R+Minus (Player 1).") :
             QObject::tr("Your Citron configuration is saved. Only homebrew preparation needs attention. Use Tools > Prepare Homebrew Tools to retry; you do not need to repeat Setup Wizard.")) +
            QStringLiteral("\n\n") + results.join(QLatin1Char('\n')));
    }
    return complete;
}
} // namespace HomebrewSetup
