/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "chomikujtypes.h"

#include <QMimeDatabase>

namespace Chomikuj
{

QString File::fullName() const
{
    return joinName(name, extension);
}

void splitName(const QString &fullName, QString *base, QString *extension)
{
    const int dot = fullName.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) {
        if (base) {
            *base = fullName.left(dot);
        }
        if (extension) {
            *extension = fullName.mid(dot + 1);
        }
    } else {
        if (base) {
            *base = fullName;
        }
        if (extension) {
            extension->clear();
        }
    }
}

QString joinName(const QString &base, const QString &extension)
{
    if (extension.isEmpty()) {
        return base;
    }
    const QString suffix = QLatin1Char('.') + extension;
    if (base.endsWith(suffix)) {
        return base;
    }
    return base + suffix;
}

QString fileExtension(const QString &name)
{
    QString base;
    QString extension;
    splitName(name, &base, &extension);
    return extension;
}

QString mimeTypeForName(const QString &name)
{
    return QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension).name();
}

} // namespace Chomikuj
