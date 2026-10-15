// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include "citron/util/temporary_directory.h"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("citron-neo: The switch fell off"));
    QTemporaryDir old_default;
#ifdef Q_OS_WIN
    assert(!old_default.isValid()); // Reproduces the shipped Windows failure.
#endif
    const auto root = QDir(QDir::tempPath()).canonicalPath() + QLatin1Char('/');
    QString first_path;
    {
        QTemporaryDir first(Citron::TemporaryDirectoryTemplate());
        QTemporaryDir second(Citron::TemporaryDirectoryTemplate());
        assert(first.isValid() && second.isValid());
        assert(first.path() != second.path());
        assert(QFileInfo(first.path()).canonicalFilePath().startsWith(root));
        assert(QFileInfo(second.path()).canonicalFilePath().startsWith(root));
        assert(!QFileInfo(first.path()).fileName().contains(QLatin1Char(':')));
        first_path = first.path();
        QFile package(first.path() + QStringLiteral("/release.zip"));
        assert(package.open(QIODevice::WriteOnly));
        assert(package.write("fixture") == 7);
        package.close();
        assert(QDir(first.path()).mkpath(QStringLiteral("unpacked")));
    }
    assert(!QFileInfo::exists(first_path)); // The unique extraction folder is cleaned up.
}
