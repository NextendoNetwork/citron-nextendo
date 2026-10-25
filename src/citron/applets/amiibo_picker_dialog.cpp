// SPDX-FileCopyrightText: Copyright 2026 Citron contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "citron/applets/amiibo_picker_dialog.h"

#include <algorithm>
#include <vector>

#include "citron/hotkeys.h"
#include "common/fs/path_util.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QSplitter>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr auto CatalogUrl = "https://raw.githubusercontent.com/Ryubing/Nfc/refs/heads/main/tags.json";
constexpr auto FavoritesCategory = "__citron_favorites__";

QJsonArray ParseCatalog(const QByteArray& bytes) {
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(bytes, &error);
    return error.error == QJsonParseError::NoError && document.isObject()
               ? document.object().value(QStringLiteral("amiibo")).toArray()
               : QJsonArray{};
}

bool MatchesTitle(const QJsonObject& game, const QString& title_id) {
    if (title_id.isEmpty()) {
        return true;
    }
    const QJsonArray ids = game.value(QStringLiteral("gameID")).toArray();
    for (const auto& id : ids) {
        if (id.toString().toUpper().contains(title_id)) {
            return true;
        }
    }
    return game.value(QStringLiteral("gameId")).toString().toUpper().contains(title_id);
}

QByteArray DownloadCatalog(QNetworkAccessManager& network) {
    QNetworkRequest request{QUrl{QString::fromLatin1(CatalogUrl)}};
    request.setTransferTimeout(7000);
    auto* reply = network.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(7500, &loop, &QEventLoop::quit);
    loop.exec();
    QByteArray result;
    if (reply->isFinished() && reply->error() == QNetworkReply::NoError) {
        result = reply->readAll();
    }
    reply->deleteLater();
    return result;
}
} // namespace

AmiiboPickerDialog::AmiiboPickerDialog(QWidget* parent, QString title_id,
                                       Core::HID::EmulatedController* controller,
                                       QString last_scanned_amiibo_id)
    : QDialog(parent), title_id{std::move(title_id)},
      last_scanned_amiibo_id{std::move(last_scanned_amiibo_id)},
      network{new QNetworkAccessManager(this)} {
    setWindowTitle(tr("Select an Amiibo"));
    resize(960, 640);

    auto* root = new QVBoxLayout(this);
    auto* controls = new QHBoxLayout;
    search_box = new QLineEdit(this);
    search_box->setPlaceholderText(tr("Search Amiibo"));
    category_combo = new QComboBox(this);
    category_combo->setMinimumWidth(180);
    show_all = new QCheckBox(tr("Show all Amiibo"), this);
    controls->addWidget(search_box, 1);
    controls->addWidget(category_combo);
    controls->addWidget(show_all);
    root->addLayout(controls);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    amiibo_list = new QListWidget(splitter);
    amiibo_list->setSelectionMode(QAbstractItemView::SingleSelection);
    auto* panel = new QWidget(splitter);
    auto* panel_layout = new QVBoxLayout(panel);
    image_label = new QLabel(panel);
    image_label->setAlignment(Qt::AlignCenter);
    image_label->setMinimumSize(230, 260);
    details_label = new QLabel(panel);
    details_label->setWordWrap(true);
    details_label->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    favorite_button = new QPushButton(tr("☆ Add to Favorites"), panel);
    favorite_button->setEnabled(false);
    panel_layout->addWidget(image_label, 1);
    panel_layout->addWidget(details_label);
    panel_layout->addWidget(favorite_button);
    splitter->addWidget(amiibo_list);
    splitter->addWidget(panel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    root->addWidget(splitter, 1);

    auto* buttons = new QHBoxLayout;
    auto* bin_button = new QPushButton(tr("Load .bin file..."), this);
    auto* cancel_button = new QPushButton(tr("Cancel"), this);
    scan_button = new QPushButton(tr("Scan selected Amiibo"), this);
    scan_button->setEnabled(false);
    scan_button->setDefault(true);
    buttons->addWidget(bin_button);
    buttons->addStretch(1);
    buttons->addWidget(cancel_button);
    buttons->addWidget(scan_button);
    root->addLayout(buttons);

    connect(search_box, &QLineEdit::textChanged, this, [this] {
        PopulateCategories();
        FilterCatalog();
    });
    connect(category_combo, &QComboBox::currentIndexChanged, this,
            [this](int) { FilterCatalog(); });
    connect(show_all, &QCheckBox::toggled, this, [this] {
        PopulateCategories();
        FilterCatalog();
    });
    connect(amiibo_list, &QListWidget::currentRowChanged, this,
            [this](int row) { UpdateSelection(row); });
    connect(amiibo_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { ScanSelectedAmiibo(); });
    connect(scan_button, &QPushButton::clicked, this, [this] { ScanSelectedAmiibo(); });
    connect(favorite_button, &QPushButton::clicked, this, [this] { ToggleFavorite(); });
    connect(bin_button, &QPushButton::clicked, this, [this] { LoadLocalBinInstead(); });
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);

    if (controller) {
        const auto bind = [this, controller](const char* buttons, auto callback) {
            auto shortcut = std::make_unique<ControllerShortcut>(controller);
            shortcut->SetKey(std::string{buttons});
            connect(shortcut.get(), &ControllerShortcut::Activated, this, callback,
                    Qt::QueuedConnection);
            controller_shortcuts.push_back(std::move(shortcut));
        };
        bind("Dpad_Up", [this] {
            const int row = amiibo_list->currentRow();
            if (row > 0) {
                amiibo_list->setCurrentRow(row - 1);
            }
        });
        bind("Dpad_Down", [this] {
            const int row = amiibo_list->currentRow();
            if (row + 1 < amiibo_list->count()) {
                amiibo_list->setCurrentRow(row + 1);
            }
        });
        bind("Dpad_Left", [this] {
            if (category_combo->count() > 0) {
                category_combo->setCurrentIndex(
                    (category_combo->currentIndex() + category_combo->count() - 1) %
                    category_combo->count());
            }
        });
        bind("Dpad_Right", [this] {
            if (category_combo->count() > 0) {
                category_combo->setCurrentIndex((category_combo->currentIndex() + 1) %
                                                 category_combo->count());
            }
        });
        bind("A", [this] { ScanSelectedAmiibo(); });
        bind("B", [this] { reject(); });
        bind("X", [this] { ToggleFavorite(); });
        bind("Y", [this] { show_all->setChecked(!show_all->isChecked()); });
    }

    LoadFavorites();
    LoadCatalog();
    amiibo_list->setFocus(Qt::OtherFocusReason);
}

AmiiboPickerDialog::~AmiiboPickerDialog() = default;

void AmiiboPickerDialog::LoadFavorites() {
    const auto path = Common::FS::GetCitronPath(Common::FS::CitronPath::AmiiboDir) /
                      "AmiiboFavorites.json";
    QFile file{QString::fromStdString(path.string())};
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray()) {
        return;
    }
    for (const auto& value : document.array()) {
        if (value.isString()) {
            favorites.insert(value.toString());
        }
    }
}

void AmiiboPickerDialog::SaveFavorites() const {
    const auto path = Common::FS::GetCitronPath(Common::FS::CitronPath::AmiiboDir) /
                      "AmiiboFavorites.json";
    QDir().mkpath(QString::fromStdString(path.parent_path().string()));
    QJsonArray values;
    for (const auto& id : favorites) {
        values.append(id);
    }
    QSaveFile file{QString::fromStdString(path.string())};
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(values).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

void AmiiboPickerDialog::LoadCatalog() {
    const auto path = Common::FS::GetCitronPath(Common::FS::CitronPath::AmiiboDir) /
                      "Amiibo.json";
    QDir().mkpath(QString::fromStdString(path.parent_path().string()));
    const QString cache_path = QString::fromStdString(path.string());
    QByteArray cached;
    QFile cache{cache_path};
    if (cache.open(QIODevice::ReadOnly)) {
        cached = cache.readAll();
        entries = ParseCatalog(cached);
    }
    const QFileInfo cache_info{cache_path};
    const bool cache_is_stale =
        entries.isEmpty() ||
        cache_info.lastModified().secsTo(QDateTime::currentDateTime()) > 24 * 60 * 60;
    if (cache_is_stale) {
        const QByteArray downloaded = DownloadCatalog(*network);
        const QJsonArray updated = ParseCatalog(downloaded);
        if (!updated.isEmpty()) {
            entries = updated;
            QFile output{cache_path};
            if (output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                output.write(downloaded);
            }
        }
    }
    if (entries.isEmpty()) {
        details_label->setText(
            tr("The Amiibo catalog could not be loaded. Check your connection and try again."));
        return;
    }
    PopulateCategories();
    FilterCatalog();
}

void AmiiboPickerDialog::PopulateCategories() {
    const QString selected = category_combo->currentData().toString();
    const QString query = search_box->text().trimmed();
    category_combo->clear();
    category_combo->addItem(tr("All categories"), QString{});
    category_combo->addItem(tr("★ Favorites"), QString::fromLatin1(FavoritesCategory));

    QSet<QString> categories;
    for (const auto& value : entries) {
        const QJsonObject entry = value.toObject();
        const QString name = entry.value(QStringLiteral("name")).toString();
        const QString id = entry.value(QStringLiteral("head")).toString() +
                           entry.value(QStringLiteral("tail")).toString();
        const QString series = entry.value(QStringLiteral("amiiboSeries")).toString();
        // Keep the category list in sync with rows FilterCatalog can actually display.
        if (name.isEmpty() || id.size() != 16 || series.isEmpty() ||
            !name.contains(query, Qt::CaseInsensitive)) {
            continue;
        }

        bool compatible = title_id.isEmpty();
        for (const auto& game : entry.value(QStringLiteral("gamesSwitch")).toArray()) {
            if (MatchesTitle(game.toObject(), title_id.toUpper())) {
                compatible = true;
                break;
            }
        }
        if (show_all->isChecked() || compatible) {
            categories.insert(series);
        }
    }

    std::vector<QString> sorted_categories;
    sorted_categories.reserve(static_cast<std::size_t>(categories.size()));
    for (const auto& category : categories) {
        sorted_categories.push_back(category);
    }
    std::sort(sorted_categories.begin(), sorted_categories.end(),
              [](const QString& left, const QString& right) {
                  return left.compare(right, Qt::CaseInsensitive) < 0;
              });
    for (const auto& category : sorted_categories) {
        category_combo->addItem(category, category);
    }

    const int restore = category_combo->findData(selected);
    category_combo->setCurrentIndex(restore >= 0 ? restore : 0);
}
void AmiiboPickerDialog::FilterCatalog() {
    const QString query = search_box->text().trimmed();
    const QString category = category_combo->currentData().toString();
    QString preserve_id;
    const int old_row = amiibo_list->currentRow();
    if (old_row >= 0 && old_row < visible_entries.size()) {
        const QJsonObject selected = entries.at(visible_entries.at(old_row)).toObject();
        preserve_id = selected.value(QStringLiteral("head")).toString() +
                      selected.value(QStringLiteral("tail")).toString();
    }

    std::vector<int> indices;
    indices.reserve(static_cast<std::size_t>(entries.size()));
    for (qsizetype i = 0; i < entries.size(); ++i) {
        indices.push_back(static_cast<int>(i));
    }
    std::sort(indices.begin(), indices.end(), [this](int a, int b) {
        return entries.at(a).toObject().value(QStringLiteral("name")).toString().compare(
                   entries.at(b).toObject().value(QStringLiteral("name")).toString(),
                   Qt::CaseInsensitive) < 0;
    });

    visible_entries.clear();
    amiibo_list->clear();
    int selected_row = -1;
    int last_scanned_row = -1;
    for (int index : indices) {
        const QJsonObject entry = entries.at(index).toObject();
        const QString id = entry.value(QStringLiteral("head")).toString() +
                           entry.value(QStringLiteral("tail")).toString();
        const QString name = entry.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || id.size() != 16 ||
            !name.contains(query, Qt::CaseInsensitive)) {
            continue;
        }

        const bool is_favorite = favorites.contains(id);
        if (category == QString::fromLatin1(FavoritesCategory) && !is_favorite) {
            continue;
        }
        if (!category.isEmpty() && category != QString::fromLatin1(FavoritesCategory) &&
            entry.value(QStringLiteral("amiiboSeries")).toString() != category) {
            continue;
        }

        bool compatible = title_id.isEmpty();
        for (const auto& game : entry.value(QStringLiteral("gamesSwitch")).toArray()) {
            if (MatchesTitle(game.toObject(), title_id.toUpper())) {
                compatible = true;
                break;
            }
        }
        if (!show_all->isChecked() && !compatible) {
            continue;
        }

        const QString display_name = is_favorite ? QStringLiteral("★ %1").arg(name) : name;
        amiibo_list->addItem(display_name);
        visible_entries.append(index);
        if (id == preserve_id) {
            selected_row = visible_entries.size() - 1;
        }
        if (id == last_scanned_amiibo_id) {
            last_scanned_row = visible_entries.size() - 1;
        }
    }

    if (visible_entries.isEmpty()) {
        scan_button->setEnabled(false);
        favorite_button->setEnabled(false);
        image_label->clear();
        details_label->setText(tr("No matching Amiibo."));
        return;
    }
    amiibo_list->setCurrentRow(selected_row >= 0 ? selected_row
                                                  : (last_scanned_row >= 0 ? last_scanned_row : 0));
}

void AmiiboPickerDialog::UpdateSelection(int row) {
    if (row < 0 || row >= visible_entries.size()) {
        scan_button->setEnabled(false);
        favorite_button->setEnabled(false);
        return;
    }
    const QJsonObject entry = entries.at(visible_entries.at(row)).toObject();
    const QString id = entry.value(QStringLiteral("head")).toString() +
                       entry.value(QStringLiteral("tail")).toString();
    const QString name = entry.value(QStringLiteral("name")).toString();
    scan_button->setEnabled(id.size() == 16);
    favorite_button->setEnabled(id.size() == 16);
    favorite_button->setText(favorites.contains(id) ? tr("★ Remove from Favorites")
                                                     : tr("☆ Add to Favorites"));

    QString details =
        QStringLiteral("%1\n%2\n%3")
            .arg(entry.value(QStringLiteral("character")).toString(),
                 entry.value(QStringLiteral("gameSeries")).toString(),
                 entry.value(QStringLiteral("amiiboSeries")).toString());
    QStringList usages;
    for (const auto& value : entry.value(QStringLiteral("gamesSwitch")).toArray()) {
        const QJsonObject game = value.toObject();
        if (!MatchesTitle(game, title_id.toUpper())) {
            continue;
        }
        for (const auto& usage : game.value(QStringLiteral("amiiboUsage")).toArray()) {
            QString usage_text = usage.toObject().value(QStringLiteral("Usage")).toString();
            if (!usage_text.isEmpty()) {
                usages.append(usage_text.replace(QLatin1Char('/'), QLatin1Char('\n')));
            }
        }
    }
    if (!usages.isEmpty()) {
        details += QStringLiteral("\n\n%1\n%2").arg(tr("In this game:"), usages.join(QLatin1Char('\n')));
    }
    details_label->setText(details.trimmed());
    image_label->clear();
    if (image_reply) {
        image_reply->abort();
    }
    const QUrl url{entry.value(QStringLiteral("image")).toString()};
    if (!url.isValid() || url.isEmpty()) {
        return;
    }
    QNetworkRequest request{url};
    request.setTransferTimeout(5000);
    QNetworkReply* reply = network->get(request);
    image_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (image_reply == reply && reply->error() == QNetworkReply::NoError) {
            QPixmap pixmap;
            if (pixmap.loadFromData(reply->readAll())) {
                image_label->setPixmap(
                    pixmap.scaled(image_label->size(), Qt::KeepAspectRatio,
                                  Qt::SmoothTransformation));
            }
        }
        if (image_reply == reply) {
            image_reply = nullptr;
        }
        reply->deleteLater();
    });
    Q_UNUSED(name);
}

void AmiiboPickerDialog::ToggleFavorite() {
    const int row = amiibo_list->currentRow();
    if (row < 0 || row >= visible_entries.size()) {
        return;
    }
    const QJsonObject entry = entries.at(visible_entries.at(row)).toObject();
    const QString id = entry.value(QStringLiteral("head")).toString() +
                       entry.value(QStringLiteral("tail")).toString();
    if (favorites.contains(id)) {
        favorites.remove(id);
    } else {
        favorites.insert(id);
    }
    SaveFavorites();

    if (category_combo->currentData().toString() ==
        QString::fromLatin1(FavoritesCategory)) {
        FilterCatalog();
        return;
    }
    amiibo_list->currentItem()->setText(
        favorites.contains(id) ? QStringLiteral("★ %1").arg(entry.value(QStringLiteral("name")).toString())
                               : entry.value(QStringLiteral("name")).toString());
    UpdateSelection(row);
}

void AmiiboPickerDialog::ScanSelectedAmiibo() {
    const int row = amiibo_list->currentRow();
    if (row < 0 || row >= visible_entries.size()) {
        return;
    }
    const QJsonObject entry = entries.at(visible_entries.at(row)).toObject();
    selected_id = entry.value(QStringLiteral("head")).toString() +
                  entry.value(QStringLiteral("tail")).toString();
    selected_name = entry.value(QStringLiteral("name")).toString();
    if (selected_id.size() == 16) {
        accept();
    }
}

void AmiiboPickerDialog::LoadLocalBinInstead() {
    load_bin_requested = true;
    accept();
}