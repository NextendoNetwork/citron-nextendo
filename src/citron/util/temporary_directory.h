// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDir>
#include <QString>

namespace Citron {
inline QString TemporaryDirectoryTemplate() {
    // QTemporaryDir's default embeds applicationName(), which may contain ':'
    // or other characters that Windows does not permit in a directory name.
    return QDir(QDir::tempPath()).filePath(QStringLiteral("citron-XXXXXX"));
}
} // namespace Citron
