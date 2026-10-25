/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "playurl.h"

namespace Chomikuj
{

bool isChomikujUrl(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("chomikuj")
        || scheme == QLatin1String("chomikujstream");
}

QUrl streamingUrl(const QUrl &url)
{
    if (url.scheme().compare(QLatin1String("chomikuj"), Qt::CaseInsensitive) != 0) {
        return url;
    }

    QUrl result = url;
    result.setScheme(QStringLiteral("chomikujstream"));
    return result;
}

} // namespace Chomikuj
