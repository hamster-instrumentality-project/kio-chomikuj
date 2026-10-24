/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>

namespace Chomikuj
{

QString decodedSecret();
QString sign(const QString &path, const QString &query, const QByteArray &body);

// Percent-encodes a query parameter the same way Python's urllib.parse.quote
// does (UTF-8, %20 for spaces, uppercase hex).
QString encodeQueryComponent(const QString &value);
QString buildQuery(const QList<QPair<QString, QString>> &parameters);

} // namespace Chomikuj
